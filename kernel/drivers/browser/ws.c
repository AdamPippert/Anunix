/*
 * ws.c — WebSocket server (RFC 6455).
 *
 * Upgrade handshake:
 *   1. Find Sec-WebSocket-Key header in the HTTP request
 *   2. Compute accept = base64(sha1(key || WS_GUID))
 *   3. Send HTTP 101 with Sec-WebSocket-Accept
 *
 * Frame format (simplified — server never masks outgoing frames):
 *   Byte 0: FIN(1) RSV(3) Opcode(4)
 *   Byte 1: MASK(1) PayloadLen(7)
 *   [2 BE bytes if PayloadLen==126, 8 BE bytes if PayloadLen==127]
 *   [4-byte mask key if MASK=1]
 *   [payload data]
 */

#include "ws.h"
#include "../../lib/crypto/sha1.h"
#include "../../lib/base64.h"
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_GUID_LEN 36

/* Find header value; returns pointer into haystack or NULL.
 * name must be lowercase; we do case-insensitive search. */
static const char *find_header(const char *req, uint32_t req_len,
				const char *name)
{
	const char *p = req;
	const char *end = req + req_len;
	size_t nlen = anx_strlen(name);

	while (p < end) {
		/* Find next line */
		const char *line_end = p;
		while (line_end < end && *line_end != '\r' && *line_end != '\n')
			line_end++;

		/* Case-insensitive compare for header name */
		if ((size_t)(line_end - p) > nlen) {
			size_t i;
			bool match = true;
			for (i = 0; i < nlen; i++) {
				char a = p[i];
				char b = name[i];
				if (a >= 'A' && a <= 'Z') a += 32;
				if (a != b) { match = false; break; }
			}
			if (match && p[nlen] == ':') {
				const char *val = p + nlen + 1;
				while (val < line_end && (*val == ' ' || *val == '\t'))
					val++;
				return val;
			}
		}

		/* Skip past \r\n */
		if (line_end < end && *line_end == '\r') line_end++;
		if (line_end < end && *line_end == '\n') line_end++;
		p = line_end;
	}
	return NULL;
}

static uint32_t header_value_len(const char *val, const char *req_end)
{
	const char *end = val;
	while (end < req_end && *end != '\r' && *end != '\n')
		end++;
	return (uint32_t)(end - val);
}

bool anx_ws_upgrade(struct anx_tcp_conn *conn,
		     const char *req_buf, uint32_t req_len)
{
	const char *key_hdr;
	uint32_t    key_len;
	char        concat[64];  /* 24-char key + 36-char GUID */
	uint8_t     digest[SHA1_DIGEST_LEN];
	char        accept[32];
	size_t      accept_len;
	char        resp[256];
	uint32_t    resp_off = 0;
	uint32_t    resp_cap = (uint32_t)sizeof(resp);

	/* Must be a GET with Upgrade: websocket */
	if (anx_strncmp(req_buf, "GET ", 4) != 0)
		return false;

	key_hdr = find_header(req_buf, req_len, "sec-websocket-key");
	if (!key_hdr)
		return false;

	key_len = header_value_len(key_hdr, req_buf + req_len);
	if (key_len == 0 || key_len > 28)
		return false;

	/* concat = key + GUID */
	anx_memcpy(concat, key_hdr, key_len);
	anx_memcpy(concat + key_len, WS_GUID, WS_GUID_LEN);

	/* SHA-1 of concat */
	anx_sha1((const uint8_t *)concat, key_len + WS_GUID_LEN, digest);

	/* Base64-encode the digest */
	accept_len = anx_base64_encode(digest, SHA1_DIGEST_LEN,
				        accept, sizeof(accept));
	accept[accept_len] = '\0';

	/* Build HTTP 101 response */
#define APPEND(s) do { \
	uint32_t _l = (uint32_t)anx_strlen(s); \
	if (resp_off + _l < resp_cap) { \
		anx_memcpy(resp + resp_off, s, _l); resp_off += _l; \
	} } while (0)

	APPEND("HTTP/1.1 101 Switching Protocols\r\n");
	APPEND("Upgrade: websocket\r\n");
	APPEND("Connection: Upgrade\r\n");
	APPEND("Sec-WebSocket-Accept: ");
	anx_memcpy(resp + resp_off, accept, accept_len);
	resp_off += (uint32_t)accept_len;
	APPEND("\r\n");
	APPEND("Access-Control-Allow-Origin: *\r\n");
	APPEND("\r\n");
#undef APPEND

	anx_tcp_srv_send(conn, resp, resp_off);
	return true;
}

/* Encode a frame header into buf; returns number of bytes written */
static uint32_t ws_encode_header(uint8_t *buf, uint8_t opcode,
				   uint32_t payload_len)
{
	uint32_t off = 0;

	buf[off++] = 0x80 | (opcode & 0x0F);  /* FIN=1, opcode */

	if (payload_len < 126) {
		buf[off++] = (uint8_t)payload_len;
	} else if (payload_len < 65536) {
		buf[off++] = 126;
		buf[off++] = (uint8_t)(payload_len >> 8);
		buf[off++] = (uint8_t)(payload_len);
	} else {
		buf[off++] = 127;
		/* 8-byte extended length (upper 4 bytes = 0 for our sizes) */
		buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
		buf[off++] = (uint8_t)(payload_len >> 24);
		buf[off++] = (uint8_t)(payload_len >> 16);
		buf[off++] = (uint8_t)(payload_len >>  8);
		buf[off++] = (uint8_t)(payload_len);
	}
	return off;
}

int anx_ws_send_text(struct anx_tcp_conn *conn,
		      const char *payload, uint32_t len)
{
	uint8_t hdr[10];
	uint32_t hdr_len = ws_encode_header(hdr, WS_OP_TEXT, len);

	anx_tcp_srv_send(conn, (const char *)hdr, hdr_len);
	anx_tcp_srv_send(conn, payload, len);
	return 0;
}

int anx_ws_send_binary(struct anx_tcp_conn *conn,
			const uint8_t *payload, uint32_t len)
{
	uint8_t hdr[10];
	uint32_t hdr_len = ws_encode_header(hdr, WS_OP_BINARY, len);

	anx_tcp_srv_send(conn, (const char *)hdr, hdr_len);
	anx_tcp_srv_send(conn, (const char *)payload, len);
	return 0;
}

bool anx_ws_recv_frame(struct anx_tcp_conn *conn,
		        struct ws_frame *out, uint32_t timeout_ms)
{
	uint8_t  hdr[2];
	int      n;
	uint8_t  opcode;
	bool     masked;
	uint64_t payload_len;
	uint8_t  mask[4];
	uint8_t *buf;
	uint32_t i;

	/* Read first 2 header bytes */
	n = anx_tcp_srv_recv(conn, (char *)hdr, 2, timeout_ms);
	if (n != 2)
		return false;

	out->fin    = (hdr[0] & 0x80) != 0;
	opcode      = hdr[0] & 0x0F;
	masked      = (hdr[1] & 0x80) != 0;
	payload_len = hdr[1] & 0x7F;

	if (payload_len == 126) {
		uint8_t ext[2];
		if (anx_tcp_srv_recv(conn, (char *)ext, 2, 200) != 2)
			return false;
		payload_len = ((uint64_t)ext[0] << 8) | ext[1];
	} else if (payload_len == 127) {
		uint8_t ext[8];
		if (anx_tcp_srv_recv(conn, (char *)ext, 8, 200) != 8)
			return false;
		payload_len = ((uint64_t)ext[4] << 24) | ((uint64_t)ext[5] << 16)
			    | ((uint64_t)ext[6] <<  8) |  (uint64_t)ext[7];
	}

	if (payload_len > WS_MAX_RECV_PAYLOAD)
		return false;

	if (masked) {
		if (anx_tcp_srv_recv(conn, (char *)mask, 4, 200) != 4)
			return false;
	}

	buf = (uint8_t *)anx_alloc((size_t)payload_len + 1);
	if (!buf)
		return false;

	if (payload_len > 0) {
		int got = anx_tcp_srv_recv(conn, (char *)buf,
					    (uint32_t)payload_len, 500);
		if (got < 0 || (uint32_t)got != payload_len) {
			anx_free(buf);
			return false;
		}
	}
	buf[payload_len] = '\0';

	if (masked) {
		for (i = 0; i < payload_len; i++)
			buf[i] ^= mask[i & 3];
	}

	out->opcode      = opcode;
	out->payload     = buf;
	out->payload_len = (uint32_t)payload_len;

	/* Respond to pings automatically */
	if (opcode == WS_OP_PING) {
		anx_ws_send_text(conn, (const char *)buf,
				  (uint32_t)payload_len);
		anx_free(buf);
		return false;
	}

	return true;
}

void anx_ws_close(struct anx_tcp_conn *conn)
{
	/* Send close frame (opcode 0x8, no payload) */
	uint8_t frame[2] = { 0x88, 0x00 };
	anx_tcp_srv_send(conn, (const char *)frame, 2);
	anx_tcp_srv_close(conn);
}
