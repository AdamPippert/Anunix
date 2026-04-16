/*
 * http.c — Minimal HTTP/1.1 client.
 *
 * Blocking GET and POST using TCP with Connection: close. Reads
 * until the server closes the connection, then parses the status
 * code and returns the body.
 */

#include <anx/types.h>
#include <anx/http.h>
#include <anx/net.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define HTTP_BUF_SIZE		16384
#define HTTP_RECV_TIMEOUT_MS	10000

/* Check if a string is a dotted-decimal IP; return it or 0 */
static uint32_t parse_ip_str(const char *s)
{
	uint32_t a = 0, b = 0, c = 0, d = 0;
	int dots = 0;
	const char *p = s;

	/* Must start with a digit */
	if (!*p || *p < '0' || *p > '9')
		return 0;

	while (*p) {
		if (*p == '.')
			dots++;
		else if (*p < '0' || *p > '9')
			return 0;
		p++;
	}
	if (dots != 3)
		return 0;

	/* Parse octets */
	p = s;
	while (*p && *p != '.') a = a * 10 + (uint32_t)(*p++ - '0');
	p++;
	while (*p && *p != '.') b = b * 10 + (uint32_t)(*p++ - '0');
	p++;
	while (*p && *p != '.') c = c * 10 + (uint32_t)(*p++ - '0');
	p++;
	while (*p) d = d * 10 + (uint32_t)(*p++ - '0');

	if (a > 255 || b > 255 || c > 255 || d > 255)
		return 0;

	return (a << 24) | (b << 16) | (c << 8) | d;
}

/* Simple integer-to-string for Content-Length */
static int itoa_simple(uint32_t val, char *buf, uint32_t bufsize)
{
	char tmp[12];
	int pos = 0;
	int i;

	if (val == 0) {
		tmp[pos++] = '0';
	} else {
		while (val > 0 && pos < 11) {
			tmp[pos++] = '0' + (char)(val % 10);
			val /= 10;
		}
	}

	if ((uint32_t)pos >= bufsize)
		return ANX_EINVAL;

	/* Reverse into output buffer */
	for (i = 0; i < pos; i++)
		buf[i] = tmp[pos - 1 - i];
	buf[pos] = '\0';
	return pos;
}

/* Append a string to a buffer at the given offset */
static int buf_append(char *buf, uint32_t *off, uint32_t cap,
		       const char *s)
{
	uint32_t len = (uint32_t)anx_strlen(s);

	if (*off + len > cap)
		return ANX_ENOMEM;
	anx_memcpy(buf + *off, s, len);
	*off += len;
	return ANX_OK;
}

/* Build the HTTP request into buf, return length */
static int http_build_request(char *buf, uint32_t cap,
			       const char *method, const char *host,
			       const char *path, const char *extra_headers,
			       const char *content_type,
			       const void *body, uint32_t body_len)
{
	uint32_t off = 0;

	/* Request line */
	if (buf_append(buf, &off, cap, method) != ANX_OK)
		return -1;
	if (buf_append(buf, &off, cap, " ") != ANX_OK)
		return -1;
	if (buf_append(buf, &off, cap, path) != ANX_OK)
		return -1;
	if (buf_append(buf, &off, cap, " HTTP/1.1\r\n") != ANX_OK)
		return -1;

	/* Host header */
	if (buf_append(buf, &off, cap, "Host: ") != ANX_OK)
		return -1;
	if (buf_append(buf, &off, cap, host) != ANX_OK)
		return -1;
	if (buf_append(buf, &off, cap, "\r\n") != ANX_OK)
		return -1;

	/* Connection: close */
	if (buf_append(buf, &off, cap, "Connection: close\r\n") != ANX_OK)
		return -1;

	/* Extra headers (pre-formatted with \r\n terminators) */
	if (extra_headers) {
		if (buf_append(buf, &off, cap, extra_headers) != ANX_OK)
			return -1;
	}

	/* Content headers for POST */
	if (body && body_len > 0 && content_type) {
		char len_str[12];

		if (buf_append(buf, &off, cap, "Content-Type: ") != ANX_OK)
			return -1;
		if (buf_append(buf, &off, cap, content_type) != ANX_OK)
			return -1;
		if (buf_append(buf, &off, cap, "\r\n") != ANX_OK)
			return -1;
		if (buf_append(buf, &off, cap, "Content-Length: ") != ANX_OK)
			return -1;
		itoa_simple(body_len, len_str, sizeof(len_str));
		if (buf_append(buf, &off, cap, len_str) != ANX_OK)
			return -1;
		if (buf_append(buf, &off, cap, "\r\n") != ANX_OK)
			return -1;
	}

	/* End of headers */
	if (buf_append(buf, &off, cap, "\r\n") != ANX_OK)
		return -1;

	/* Body */
	if (body && body_len > 0) {
		if (off + body_len > cap)
			return -1;
		anx_memcpy(buf + off, body, body_len);
		off += body_len;
	}

	return (int)off;
}

/* Parse status code from first line of response */
static int http_parse_status(const char *data, uint32_t len)
{
	/* "HTTP/1.x YYY ..." — status code at offset 9 */
	int code = 0;
	uint32_t i;

	if (len < 12)
		return -1;

	/* Find the space after HTTP version */
	for (i = 0; i < len && data[i] != ' '; i++)
		;
	i++;	/* skip space */

	/* Parse 3-digit status code */
	if (i + 3 > len)
		return -1;
	code = (data[i] - '0') * 100 +
	       (data[i + 1] - '0') * 10 +
	       (data[i + 2] - '0');

	return code;
}

/* Find "\r\n\r\n" header/body separator */
static int http_find_body(const char *data, uint32_t len)
{
	uint32_t i;

	for (i = 0; i + 3 < len; i++) {
		if (data[i] == '\r' && data[i + 1] == '\n' &&
		    data[i + 2] == '\r' && data[i + 3] == '\n')
			return (int)(i + 4);
	}
	return -1;
}

static int http_request(const char *host, uint16_t port, const char *path,
			 const char *method, const char *extra_headers,
			 const char *content_type,
			 const void *body, uint32_t body_len,
			 struct anx_http_response *resp)
{
	uint32_t ip;
	struct anx_tcp_conn *conn;
	char *req_buf;
	char *recv_buf;
	int req_len;
	uint32_t recv_total = 0;
	int n;
	int body_off;
	int ret;

	resp->status_code = 0;
	resp->body = NULL;
	resp->body_len = 0;

	/* Resolve hostname — skip DNS if it's already an IP address */
	ip = parse_ip_str(host);
	if (ip != 0) {
		ret = ANX_OK;
	} else {
		ret = anx_dns_resolve(host, &ip);
		if (ret != ANX_OK) {
			kprintf("http: dns failed for %s (%d)\n", host, ret);
			return ret;
		}
	}

	/* Connect */
	ret = anx_tcp_connect(ip, port, &conn);
	if (ret != ANX_OK) {
		kprintf("http: connect failed (%d)\n", ret);
		return ret;
	}

	/* Build and send request */
	req_buf = anx_alloc(4096);
	if (!req_buf) {
		anx_tcp_close(conn);
		return ANX_ENOMEM;
	}

	req_len = http_build_request(req_buf, 4096, method, host, path,
				     extra_headers, content_type,
				     body, body_len);
	if (req_len < 0) {
		anx_free(req_buf);
		anx_tcp_close(conn);
		return ANX_EINVAL;
	}

	ret = anx_tcp_send(conn, req_buf, (uint32_t)req_len);
	anx_free(req_buf);
	if (ret != ANX_OK) {
		anx_tcp_close(conn);
		return ret;
	}

	/* Receive response (read until close) */
	recv_buf = anx_alloc(HTTP_BUF_SIZE);
	if (!recv_buf) {
		anx_tcp_close(conn);
		return ANX_ENOMEM;
	}

	while (recv_total < HTTP_BUF_SIZE - 1) {
		n = anx_tcp_recv(conn, recv_buf + recv_total,
				 HTTP_BUF_SIZE - 1 - recv_total,
				 HTTP_RECV_TIMEOUT_MS);
		if (n <= 0)
			break;
		recv_total += (uint32_t)n;
	}

	anx_tcp_close(conn);

	if (recv_total == 0) {
		anx_free(recv_buf);
		return ANX_EIO;
	}

	recv_buf[recv_total] = '\0';

	/* Parse response */
	resp->status_code = http_parse_status(recv_buf, recv_total);

	body_off = http_find_body(recv_buf, recv_total);
	if (body_off > 0 && (uint32_t)body_off < recv_total) {
		resp->body_len = recv_total - (uint32_t)body_off;
		resp->body = anx_alloc(resp->body_len + 1);
		if (resp->body) {
			anx_memcpy(resp->body, recv_buf + body_off,
				   resp->body_len);
			resp->body[resp->body_len] = '\0';
		}
	}

	anx_free(recv_buf);
	return ANX_OK;
}

int anx_http_get(const char *host, uint16_t port, const char *path,
		  struct anx_http_response *resp)
{
	return http_request(host, port, path, "GET",
			    NULL, NULL, NULL, 0, resp);
}

int anx_http_get_authed(const char *host, uint16_t port, const char *path,
			 const char *extra_headers,
			 struct anx_http_response *resp)
{
	return http_request(host, port, path, "GET",
			    extra_headers, NULL, NULL, 0, resp);
}

int anx_http_post(const char *host, uint16_t port, const char *path,
		   const char *content_type,
		   const void *body, uint32_t body_len,
		   struct anx_http_response *resp)
{
	return http_request(host, port, path, "POST",
			    NULL, content_type, body, body_len, resp);
}

int anx_http_post_authed(const char *host, uint16_t port, const char *path,
			  const char *extra_headers,
			  const char *content_type,
			  const void *body, uint32_t body_len,
			  struct anx_http_response *resp)
{
	return http_request(host, port, path, "POST",
			    extra_headers, content_type,
			    body, body_len, resp);
}

void anx_http_response_free(struct anx_http_response *resp)
{
	if (resp->body) {
		anx_free(resp->body);
		resp->body = NULL;
	}
	resp->body_len = 0;
}
