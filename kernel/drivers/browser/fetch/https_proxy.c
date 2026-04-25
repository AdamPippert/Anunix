/*
 * https_proxy.c — HTTPS fetch via HTTP CONNECT tunnel to anxbproxy.
 *
 * Protocol:
 *   1. TCP connect to ANXBPROXY_IP:ANXBPROXY_PORT.
 *   2. Send CONNECT host:443 HTTP/1.1 to open TLS tunnel.
 *   3. Read proxy's "200 Connection established" response.
 *   4. Send HTTP/1.1 GET through the tunnel.
 *   5. Read the full plaintext response; parse status + body.
 *
 * The proxy terminates TLS; the kernel only sees plain HTTP on the
 * loopback path.  See https_proxy.h for proxy address constants.
 */

#include "https_proxy.h"
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Maximum response size we'll buffer (512 KiB). */
#define PROXY_RECV_CAP  (512u * 1024u)
/* Per-recv() timeout in milliseconds. */
#define RECV_TIMEOUT_MS  10000u

/* ── Internal helpers ────────────────────────────────────────────── */

/*
 * Drain the connection until it closes or cap is reached.
 * Returns total bytes received.
 */
static uint32_t tcp_read_all(struct anx_tcp_conn *conn,
			       uint8_t *buf, uint32_t cap)
{
	uint32_t total = 0;

	while (total < cap) {
		int n = anx_tcp_recv(conn, buf + total,
				     cap - total, RECV_TIMEOUT_MS);
		if (n <= 0)
			break;
		total += (uint32_t)n;
	}
	return total;
}

/*
 * Locate the end of HTTP headers (\r\n\r\n) in buf[0..len).
 * Returns the byte offset of the first body byte, or 0 if not found.
 */
static uint32_t headers_end(const uint8_t *buf, uint32_t len)
{
	uint32_t i;

	if (len < 4)
		return 0;
	for (i = 0; i <= len - 4; i++) {
		if (buf[i]   == '\r' && buf[i+1] == '\n' &&
		    buf[i+2] == '\r' && buf[i+3] == '\n')
			return i + 4;
	}
	return 0;
}

/*
 * Parse the numeric HTTP status code from "HTTP/1.x NNN ...".
 * Returns the code, or -1 on parse failure.
 */
static int parse_status(const uint8_t *buf, uint32_t len)
{
	uint32_t i = 0;

	/* Skip "HTTP/1.x" */
	while (i < len && buf[i] != ' ')
		i++;
	if (i >= len)
		return -1;
	i++; /* skip space */
	if (i + 3 > len)
		return -1;
	if (buf[i] < '1' || buf[i] > '5')
		return -1;
	return (buf[i] - '0') * 100
	     + (buf[i+1] - '0') * 10
	     + (buf[i+2] - '0');
}

/* ── Public API ──────────────────────────────────────────────────── */

int https_fetch(const char *host, const char *path,
		 struct anx_http_response *resp)
{
	struct anx_tcp_conn *conn = NULL;
	uint8_t  hbuf[1024];       /* proxy CONNECT response (always small) */
	uint8_t *rbuf = NULL;
	char     req[512];
	uint32_t off;
	uint32_t total, body_off, body_len;
	int      status;
	int      ret = -1;
	int      n;
	char    *body;

	/* Step 1: connect to the local proxy */
	if (anx_tcp_connect(ANXBPROXY_IP, ANXBPROXY_PORT, &conn) != 0) {
		kprintf("https_proxy: TCP connect failed\n");
		return -1;
	}

	/* Step 2: send HTTP CONNECT tunnel request */
	off  = 0;
	off += anx_strlcpy(req + off, "CONNECT ",         sizeof(req) - off);
	off += anx_strlcpy(req + off, host,               sizeof(req) - off);
	off += anx_strlcpy(req + off, ":443 HTTP/1.1\r\n", sizeof(req) - off);
	off += anx_strlcpy(req + off, "Host: ",           sizeof(req) - off);
	off += anx_strlcpy(req + off, host,               sizeof(req) - off);
	off += anx_strlcpy(req + off, ":443\r\n\r\n",    sizeof(req) - off);

	if (anx_tcp_send(conn, req, off) != 0) {
		kprintf("https_proxy: CONNECT send failed\n");
		goto out;
	}

	/* Step 3: read proxy's response; expect "200 Connection established" */
	n = anx_tcp_recv(conn, hbuf, sizeof(hbuf) - 1, RECV_TIMEOUT_MS);
	if (n <= 0) {
		kprintf("https_proxy: no CONNECT response\n");
		goto out;
	}
	hbuf[n] = '\0';
	if (parse_status(hbuf, (uint32_t)n) != 200) {
		kprintf("https_proxy: proxy rejected CONNECT: %.60s\n",
			(const char *)hbuf);
		goto out;
	}

	/* Step 4: send HTTP/1.1 GET through the established tunnel */
	off  = 0;
	off += anx_strlcpy(req + off, "GET ",                    sizeof(req) - off);
	off += anx_strlcpy(req + off, path,                      sizeof(req) - off);
	off += anx_strlcpy(req + off, " HTTP/1.1\r\n",           sizeof(req) - off);
	off += anx_strlcpy(req + off, "Host: ",                  sizeof(req) - off);
	off += anx_strlcpy(req + off, host,                      sizeof(req) - off);
	off += anx_strlcpy(req + off, "\r\nConnection: close\r\n", sizeof(req) - off);
	off += anx_strlcpy(req + off, "\r\n",                    sizeof(req) - off);

	if (anx_tcp_send(conn, req, off) != 0) {
		kprintf("https_proxy: GET send failed\n");
		goto out;
	}

	/* Step 5: drain the full response into rbuf */
	rbuf = (uint8_t *)anx_alloc(PROXY_RECV_CAP);
	if (!rbuf) {
		kprintf("https_proxy: OOM for response buffer\n");
		goto out;
	}

	total = tcp_read_all(conn, rbuf, PROXY_RECV_CAP - 1);
	if (total == 0) {
		kprintf("https_proxy: empty response\n");
		goto out;
	}
	rbuf[total] = '\0';

	/* Parse status line */
	status = parse_status(rbuf, total);
	if (status < 100 || status > 599) {
		kprintf("https_proxy: bad status %d\n", status);
		goto out;
	}

	/* Locate body start */
	body_off = headers_end(rbuf, total);
	if (body_off == 0) {
		kprintf("https_proxy: header terminator not found\n");
		goto out;
	}

	body_len = total - body_off;
	body = (char *)anx_alloc(body_len + 1);
	if (!body) {
		kprintf("https_proxy: OOM for body\n");
		goto out;
	}
	anx_memcpy(body, rbuf + body_off, body_len);
	body[body_len] = '\0';

	resp->status_code = status;
	resp->body        = body;
	resp->body_len    = body_len;
	ret = 0;

out:
	if (rbuf) anx_free(rbuf);
	anx_tcp_close(conn);
	return ret;
}
