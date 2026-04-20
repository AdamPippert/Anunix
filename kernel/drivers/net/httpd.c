/*
 * httpd.c — HTTP API server for programmatic shell access.
 *
 * Listens on a configurable port (default 8080) and accepts
 * HTTP requests to execute shell commands and return output
 * as JSON. Single connection, synchronous.
 *
 * Endpoints:
 *   GET  /api/v1/health   — health check
 *   POST /api/v1/exec     — execute a shell command
 */

#include <anx/types.h>
#include <anx/httpd.h>
#include <anx/net.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/json.h>
#include <anx/shell.h>
#include <anx/fb.h>

#define HTTPD_REQ_BUF_SIZE	4096
#define HTTPD_RESP_BUF_SIZE	65536
#define HTTPD_CAPTURE_SIZE	32768
#define HTTPD_RECV_TIMEOUT_MS	5000

/* Current pending connection for request processing */
static struct anx_tcp_conn *pending_conn;
static bool httpd_ready;

/* Simple integer-to-string */
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
		return -1;

	for (i = 0; i < pos; i++)
		buf[i] = tmp[pos - 1 - i];
	buf[pos] = '\0';
	return pos;
}

/* Append string to buffer at offset, return ANX_OK or ANX_ENOMEM */
static int buf_append(char *buf, uint32_t *off, uint32_t cap,
		       const char *s)
{
	uint32_t len = (uint32_t)anx_strlen(s);

	if (*off + len >= cap)
		return ANX_ENOMEM;
	anx_memcpy(buf + *off, s, len);
	*off += len;
	return ANX_OK;
}

/* Append len bytes from src to buffer at offset */
static int buf_append_n(char *buf, uint32_t *off, uint32_t cap,
			 const char *src, uint32_t len)
{
	if (*off + len >= cap)
		return ANX_ENOMEM;
	anx_memcpy(buf + *off, src, len);
	*off += len;
	return ANX_OK;
}

/*
 * Escape a string for JSON output. Handles \n, \r, \t, \\, \", and
 * control characters. Returns bytes written (not including NUL) or
 * -1 on overflow.
 */
static int json_escape(const char *src, uint32_t src_len,
			char *dst, uint32_t dst_cap)
{
	uint32_t di = 0;
	uint32_t si;

	for (si = 0; si < src_len && src[si]; si++) {
		char c = src[si];

		switch (c) {
		case '"':
			if (di + 2 > dst_cap) return -1;
			dst[di++] = '\\';
			dst[di++] = '"';
			break;
		case '\\':
			if (di + 2 > dst_cap) return -1;
			dst[di++] = '\\';
			dst[di++] = '\\';
			break;
		case '\n':
			if (di + 2 > dst_cap) return -1;
			dst[di++] = '\\';
			dst[di++] = 'n';
			break;
		case '\r':
			if (di + 2 > dst_cap) return -1;
			dst[di++] = '\\';
			dst[di++] = 'r';
			break;
		case '\t':
			if (di + 2 > dst_cap) return -1;
			dst[di++] = '\\';
			dst[di++] = 't';
			break;
		default:
			if ((uint8_t)c < 0x20) {
				/* Skip other control characters */
				continue;
			}
			if (di + 1 > dst_cap) return -1;
			dst[di++] = c;
			break;
		}
	}

	if (di < dst_cap)
		dst[di] = '\0';
	return (int)di;
}

/* Find "\r\n\r\n" header/body separator */
static int find_body(const char *data, uint32_t len)
{
	uint32_t i;

	for (i = 0; i + 3 < len; i++) {
		if (data[i] == '\r' && data[i + 1] == '\n' &&
		    data[i + 2] == '\r' && data[i + 3] == '\n')
			return (int)(i + 4);
	}
	return -1;
}

/* Parse Content-Length from headers */
static uint32_t parse_content_length(const char *headers, uint32_t hdr_len)
{
	const char *p = headers;
	const char *end = headers + hdr_len;

	while (p < end) {
		/* Case-insensitive match for "Content-Length:" */
		if ((p + 16 <= end) &&
		    (p[0] == 'C' || p[0] == 'c') &&
		    (p[1] == 'o' || p[1] == 'O') &&
		    (p[2] == 'n' || p[2] == 'N') &&
		    (p[3] == 't' || p[3] == 'T') &&
		    (p[4] == 'e' || p[4] == 'E') &&
		    (p[5] == 'n' || p[5] == 'N') &&
		    (p[6] == 't' || p[6] == 'T') &&
		    p[7] == '-' &&
		    (p[8] == 'L' || p[8] == 'l') &&
		    (p[9] == 'e' || p[9] == 'E') &&
		    (p[10] == 'n' || p[10] == 'N') &&
		    (p[11] == 'g' || p[11] == 'G') &&
		    (p[12] == 't' || p[12] == 'T') &&
		    (p[13] == 'h' || p[13] == 'H') &&
		    p[14] == ':') {
			const char *v = p + 15;
			uint32_t val = 0;

			while (v < end && *v == ' ')
				v++;
			while (v < end && *v >= '0' && *v <= '9') {
				val = val * 10 + (uint32_t)(*v - '0');
				v++;
			}
			return val;
		}
		/* Skip to next line */
		while (p < end && *p != '\n')
			p++;
		if (p < end)
			p++;
	}
	return 0;
}

/* Send an HTTP response */
static void httpd_send_response(struct anx_tcp_conn *conn,
				 int status_code, const char *status_text,
				 const char *content_type,
				 const char *body, uint32_t body_len)
{
	char *resp;
	uint32_t off = 0;
	uint32_t cap = 512 + body_len;
	char len_str[12];

	resp = anx_alloc(cap);
	if (!resp)
		return;

	buf_append(resp, &off, cap, "HTTP/1.1 ");

	itoa_simple((uint32_t)status_code, len_str, sizeof(len_str));
	buf_append(resp, &off, cap, len_str);
	buf_append(resp, &off, cap, " ");
	buf_append(resp, &off, cap, status_text);
	buf_append(resp, &off, cap, "\r\n");

	buf_append(resp, &off, cap, "Content-Type: ");
	buf_append(resp, &off, cap, content_type);
	buf_append(resp, &off, cap, "\r\n");

	buf_append(resp, &off, cap, "Content-Length: ");
	itoa_simple(body_len, len_str, sizeof(len_str));
	buf_append(resp, &off, cap, len_str);
	buf_append(resp, &off, cap, "\r\n");

	buf_append(resp, &off, cap, "Connection: close\r\n");
	buf_append(resp, &off, cap, "Access-Control-Allow-Origin: *\r\n");
	buf_append(resp, &off, cap, "\r\n");

	if (body && body_len > 0)
		buf_append_n(resp, &off, cap, body, body_len);

	anx_tcp_srv_send(conn, resp, off);
	anx_free(resp);
}

/* Handle GET /api/v1/health */
static void handle_health(struct anx_tcp_conn *conn)
{
	const char *body = "{\"status\": \"healthy\"}";
	uint32_t body_len = (uint32_t)anx_strlen(body);

	httpd_send_response(conn, 200, "OK",
			     "application/json", body, body_len);
}

/* Handle GET /api/v1/fb */
static void handle_fb_info(struct anx_tcp_conn *conn)
{
	const struct anx_fb_info *fb = anx_fb_get_info();
	char body[128];
	uint32_t off = 0;

	if (!fb || !fb->available) {
		const char *s = "{\"available\":false}";

		httpd_send_response(conn, 200, "OK",
				     "application/json",
				     s, (uint32_t)anx_strlen(s));
		return;
	}

	buf_append(body, &off, sizeof(body), "{\"available\":true,\"width\":");
	{ char n[12]; itoa_simple(fb->width,  n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), ",\"height\":");
	{ char n[12]; itoa_simple(fb->height, n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), ",\"pitch\":");
	{ char n[12]; itoa_simple(fb->pitch,  n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), ",\"bpp\":");
	{ char n[12]; itoa_simple((uint32_t)fb->bpp, n, sizeof(n)); buf_append(body, &off, sizeof(body), n); }
	buf_append(body, &off, sizeof(body), "}");

	httpd_send_response(conn, 200, "OK",
			     "application/json", body, off);
}

/* Handle GET /api/v1/display/modes */
static void handle_display_modes(struct anx_tcp_conn *conn)
{
	const struct anx_gop_mode *modes;
	uint8_t  count, current;
	char    *body;
	uint32_t off = 0;
	uint32_t cap = 64 + (uint32_t)ANX_GOP_MODES_MAX * 64;
	uint8_t  i;

	modes = anx_fb_get_gop_modes(&count, &current);

	body = anx_alloc(cap);
	if (!body) {
		const char *err = "{\"error\":\"out of memory\"}";

		httpd_send_response(conn, 500, "Internal Server Error",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
		return;
	}

	buf_append(body, &off, cap, "{\"count\":");
	{ char n[12]; itoa_simple((uint32_t)count, n, sizeof(n)); buf_append(body, &off, cap, n); }
	buf_append(body, &off, cap, ",\"current\":");
	{ char n[12]; itoa_simple((uint32_t)current, n, sizeof(n)); buf_append(body, &off, cap, n); }
	buf_append(body, &off, cap, ",\"modes\":[");

	for (i = 0; i < count; i++) {
		char n[12];

		if (i > 0)
			buf_append(body, &off, cap, ",");
		buf_append(body, &off, cap, "{\"index\":");
		itoa_simple((uint32_t)i, n, sizeof(n));
		buf_append(body, &off, cap, n);
		buf_append(body, &off, cap, ",\"width\":");
		itoa_simple(modes[i].width, n, sizeof(n));
		buf_append(body, &off, cap, n);
		buf_append(body, &off, cap, ",\"height\":");
		itoa_simple(modes[i].height, n, sizeof(n));
		buf_append(body, &off, cap, n);
		buf_append(body, &off, cap, ",\"selected\":");
		buf_append(body, &off, cap, (i == current) ? "true" : "false");
		buf_append(body, &off, cap, "}");
	}
	buf_append(body, &off, cap, "]}");

	httpd_send_response(conn, 200, "OK",
			     "application/json", body, off);
	anx_free(body);
}

/* Handle POST /api/v1/exec */
static void handle_exec(struct anx_tcp_conn *conn,
			 const char *body, uint32_t body_len)
{
	struct anx_json_value root;
	struct anx_json_value *cmd_val;
	const char *command;
	char *capture;
	char *escaped;
	char *resp_body;
	uint32_t resp_off = 0;
	uint32_t resp_cap;
	uint32_t captured;
	int esc_len;
	int ret;

	if (!body || body_len == 0) {
		const char *err = "{\"status\": \"error\", \"output\": \"empty request body\"}";

		httpd_send_response(conn, 400, "Bad Request",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
		return;
	}

	/* Parse JSON request body */
	ret = anx_json_parse(body, body_len, &root);
	if (ret != ANX_OK) {
		const char *err = "{\"status\": \"error\", \"output\": \"invalid JSON\"}";

		httpd_send_response(conn, 400, "Bad Request",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
		return;
	}

	/* Extract "command" field */
	cmd_val = anx_json_get(&root, "command");
	command = anx_json_string(cmd_val);
	if (!command || command[0] == '\0') {
		const char *err = "{\"status\": \"error\", \"output\": \"missing 'command' field\"}";

		anx_json_free(&root);
		httpd_send_response(conn, 400, "Bad Request",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
		return;
	}

	/* Capture kprintf output during command execution */
	capture = anx_alloc(HTTPD_CAPTURE_SIZE);
	if (!capture) {
		const char *err = "{\"status\": \"error\", \"output\": \"out of memory\"}";

		anx_json_free(&root);
		httpd_send_response(conn, 500, "Internal Server Error",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
		return;
	}

	anx_kprintf_capture_start(capture, HTTPD_CAPTURE_SIZE);
	anx_shell_execute(command);
	captured = anx_kprintf_capture_stop();

	anx_json_free(&root);

	/* Escape captured output for JSON */
	escaped = anx_alloc(HTTPD_CAPTURE_SIZE * 2);
	if (!escaped) {
		anx_free(capture);
		return;
	}

	esc_len = json_escape(capture, captured, escaped,
			       HTTPD_CAPTURE_SIZE * 2 - 1);
	if (esc_len < 0)
		esc_len = 0;

	anx_free(capture);

	/* Build JSON response: {"status": "ok", "output": "..."} */
	resp_cap = (uint32_t)esc_len + 64;
	resp_body = anx_alloc(resp_cap);
	if (!resp_body) {
		anx_free(escaped);
		return;
	}

	resp_off = 0;
	buf_append(resp_body, &resp_off, resp_cap,
		    "{\"status\": \"ok\", \"output\": \"");
	buf_append_n(resp_body, &resp_off, resp_cap, escaped,
		      (uint32_t)esc_len);
	buf_append(resp_body, &resp_off, resp_cap, "\"}");

	anx_free(escaped);

	httpd_send_response(conn, 200, "OK",
			     "application/json", resp_body, resp_off);

	anx_free(resp_body);
}

/* Handle a single HTTP request on a connected socket */
static void httpd_handle_request(struct anx_tcp_conn *conn)
{
	char *req_buf;
	uint32_t total = 0;
	int n;
	int body_off;
	uint32_t content_len;
	bool is_get;
	bool is_post;

	req_buf = anx_alloc(HTTPD_REQ_BUF_SIZE);
	if (!req_buf) {
		anx_tcp_srv_close(conn);
		return;
	}

	/* Read request headers (wait for \r\n\r\n) */
	while (total < HTTPD_REQ_BUF_SIZE - 1) {
		n = anx_tcp_srv_recv(conn, req_buf + total,
				      HTTPD_REQ_BUF_SIZE - 1 - total,
				      HTTPD_RECV_TIMEOUT_MS);
		if (n <= 0)
			break;
		total += (uint32_t)n;
		req_buf[total] = '\0';

		/* Check if we have complete headers */
		body_off = find_body(req_buf, total);
		if (body_off > 0) {
			/* Read remaining body if Content-Length says more */
			content_len = parse_content_length(req_buf,
							    (uint32_t)body_off);
			if (content_len > 0) {
				uint32_t body_have = total - (uint32_t)body_off;

				while (body_have < content_len &&
				       total < HTTPD_REQ_BUF_SIZE - 1) {
					n = anx_tcp_srv_recv(conn,
							      req_buf + total,
							      HTTPD_REQ_BUF_SIZE - 1 - total,
							      HTTPD_RECV_TIMEOUT_MS);
					if (n <= 0)
						break;
					total += (uint32_t)n;
					body_have += (uint32_t)n;
				}
				req_buf[total] = '\0';
			}
			break;
		}
	}

	if (total == 0) {
		anx_free(req_buf);
		anx_tcp_srv_close(conn);
		return;
	}

	body_off = find_body(req_buf, total);

	/* Parse method and path from first line */
	is_get = (anx_strncmp(req_buf, "GET ", 4) == 0);
	is_post = (anx_strncmp(req_buf, "POST ", 5) == 0);

	if (is_get) {
		/* Check path */
		if (anx_strncmp(req_buf + 4, "/api/v1/health", 14) == 0) {
			handle_health(conn);
		} else if (anx_strncmp(req_buf + 4, "/api/v1/fb", 10) == 0) {
			handle_fb_info(conn);
		} else if (anx_strncmp(req_buf + 4, "/api/v1/display/modes", 21) == 0) {
			handle_display_modes(conn);
		} else {
			const char *err = "{\"status\": \"error\", \"output\": \"not found\"}";

			httpd_send_response(conn, 404, "Not Found",
					     "application/json",
					     err, (uint32_t)anx_strlen(err));
		}
	} else if (is_post) {
		if (anx_strncmp(req_buf + 5, "/api/v1/exec", 12) == 0) {
			const char *body = NULL;
			uint32_t body_len = 0;

			if (body_off > 0 && (uint32_t)body_off < total) {
				body = req_buf + body_off;
				body_len = total - (uint32_t)body_off;
			}
			handle_exec(conn, body, body_len);
		} else {
			const char *err = "{\"status\": \"error\", \"output\": \"not found\"}";

			httpd_send_response(conn, 404, "Not Found",
					     "application/json",
					     err, (uint32_t)anx_strlen(err));
		}
	} else {
		const char *err = "{\"status\": \"error\", \"output\": \"method not allowed\"}";

		httpd_send_response(conn, 405, "Method Not Allowed",
				     "application/json",
				     err, (uint32_t)anx_strlen(err));
	}

	anx_free(req_buf);
	anx_tcp_srv_close(conn);
}

/* Accept callback — invoked when a TCP handshake completes */
static void httpd_accept(struct anx_tcp_conn *conn, void *arg)
{
	(void)arg;
	pending_conn = conn;
}

int anx_httpd_init(uint16_t port)
{
	int ret;

	ret = anx_tcp_listen(port, httpd_accept, NULL);
	if (ret != ANX_OK) {
		kprintf("httpd: failed to listen on port %u (%d)\n",
			(uint32_t)port, ret);
		return ret;
	}

	httpd_ready = true;
	kprintf("httpd: API server listening on port %u\n", (uint32_t)port);
	return ANX_OK;
}

void anx_httpd_poll(void)
{
	if (!httpd_ready)
		return;

	if (pending_conn) {
		struct anx_tcp_conn *conn = pending_conn;

		pending_conn = NULL;
		httpd_handle_request(conn);
	}
}
