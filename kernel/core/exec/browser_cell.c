/*
 * browser_cell.c — Browser Renderer Execution Cell.
 *
 * Connects to an anxbrowserd instance, creates a Playwright/Chromium
 * session, then streams JPEG frames over a persistent TCP connection
 * and blits them directly to the framebuffer via anx_jpeg_blit_scaled().
 *
 * Wire anx_browser_cell_tick() into the idle poll loop (shell.c).
 */

#include <anx/types.h>
#include <anx/browser_cell.h>
#include <anx/alloc.h>
#include <anx/net.h>
#include <anx/http.h>
#include <anx/json.h>
#include <anx/jpeg.h>
#include <anx/fb.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Stream receive buffer — 512 KB handles QEMU/720p JPEG frames comfortably */
#define BC_RECV_BUF_SIZE	(512u * 1024u)
#define BC_DEFAULT_PORT		9090
#define BC_DEFAULT_IP		ANX_IP4(10, 0, 2, 2)

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

static char       bc_host[64];
static uint32_t   bc_host_ip;
static uint16_t   bc_port;
static char       bc_sid[64];

static struct anx_tcp_conn *bc_stream;
static uint8_t   *bc_buf;
static uint32_t   bc_buf_len;
static bool       bc_hdr_done;
static bool       bc_running;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Parse a dotted-quad IPv4 string to a uint32_t host-order address. */
static uint32_t parse_ip4(const char *s)
{
	uint32_t ip = 0;
	int i;

	for (i = 0; i < 4; i++) {
		uint32_t oct = 0;

		while (*s >= '0' && *s <= '9')
			oct = oct * 10 + (uint32_t)(*s++ - '0');
		if (oct > 255)
			return 0;
		ip = (ip << 8) | oct;
		if (i < 3) {
			if (*s != '.')
				return 0;
			s++;
		}
	}
	return ip;
}

/* Append a NUL-terminated string to buf[pos]; return new pos. */
static uint32_t sa(char *buf, uint32_t pos, uint32_t size, const char *s)
{
	while (*s && pos + 1 < size)
		buf[pos++] = *s++;
	if (pos < size)
		buf[pos] = '\0';
	return pos;
}

/* Append a uint32_t decimal to buf[pos]; return new pos. */
static uint32_t ua(char *buf, uint32_t pos, uint32_t size, uint32_t v)
{
	char tmp[12];
	int n = 0;

	if (v == 0) {
		if (pos + 1 < size) buf[pos++] = '0';
		if (pos < size) buf[pos] = '\0';
		return pos;
	}
	while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
	while (n-- > 0) {
		if (pos + 1 < size) buf[pos++] = tmp[n];
	}
	if (pos < size) buf[pos] = '\0';
	return pos;
}

/* Locate \r\n\r\n in buf; return offset past it, or 0 if not found. */
static uint32_t skip_headers(const uint8_t *buf, uint32_t len)
{
	uint32_t i;

	for (i = 0; i + 3 < len; i++) {
		if (buf[i] == '\r' && buf[i+1] == '\n' &&
		    buf[i+2] == '\r' && buf[i+3] == '\n')
			return i + 4;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int anx_browser_cell_init(const char *host, uint16_t port)
{
	struct anx_http_response resp;
	struct anx_json_value root;
	struct anx_json_value *id_val;
	const char *id_str;
	const char *body = "{\"headless\":true}";
	char stream_req[256];
	uint32_t p;
	int ret;

	if (bc_running)
		anx_browser_cell_stop();

	/* Connection parameters */
	if (!host || host[0] == '\0') {
		anx_strlcpy(bc_host, "10.0.2.2", sizeof(bc_host));
		bc_host_ip = BC_DEFAULT_IP;
	} else {
		anx_strlcpy(bc_host, host, sizeof(bc_host));
		bc_host_ip = parse_ip4(host);
		if (bc_host_ip == 0) {
			kprintf("browser_cell: invalid host '%s'\n", host);
			return ANX_EINVAL;
		}
	}
	bc_port = port ? port : BC_DEFAULT_PORT;

	/* --- Step 1: Create browser session via HTTP POST --- */
	kprintf("browser_cell: connecting to %s:%u\n",
		bc_host, (uint32_t)bc_port);

	ret = anx_http_post(bc_host, bc_port, "/api/v1/sessions",
			    "application/json",
			    body, (uint32_t)anx_strlen(body), &resp);
	if (ret != ANX_OK || (resp.status_code != 200 && resp.status_code != 201)) {
		kprintf("browser_cell: session create failed (ret=%d http=%d)\n",
			ret, resp.status_code);
		anx_http_response_free(&resp);
		return ANX_EIO;
	}

	/* Parse "session_id" from JSON response */
	kprintf("browser_cell: http %d, body_len=%u\n",
		resp.status_code, resp.body_len);
	ret = anx_json_parse(resp.body, resp.body_len, &root);
	if (ret != ANX_OK) {
		kprintf("browser_cell: JSON parse failed (%d)\n", ret);
		kprintf("browser_cell: body: %s\n",
			resp.body ? resp.body : "(null)");
		anx_http_response_free(&resp);
		return ANX_EIO;
	}
	id_val = anx_json_get(&root, "session_id");
	id_str = id_val ? anx_json_string(id_val) : NULL;
	if (!id_str || id_str[0] == '\0') {
		kprintf("browser_cell: no session_id in body: %s\n",
			resp.body ? resp.body : "(null)");
		anx_json_free(&root);
		anx_http_response_free(&resp);
		return ANX_EIO;
	}
	anx_http_response_free(&resp);
	anx_strlcpy(bc_sid, id_str, sizeof(bc_sid));
	anx_json_free(&root);
	kprintf("browser_cell: session %s created\n", bc_sid);

	/* --- Step 2: Open persistent TCP connection for JPEG stream --- */
	ret = anx_tcp_connect(bc_host_ip, bc_port, &bc_stream);
	if (ret != ANX_OK) {
		kprintf("browser_cell: TCP connect failed (%d)\n", ret);
		return ANX_EIO;
	}

	/* Send GET request for the binary stream endpoint */
	p = 0;
	/* HTTP/1.0 so the server responds without Transfer-Encoding: chunked */
	p = sa(stream_req, p, sizeof(stream_req), "GET /api/v1/sessions/");
	p = sa(stream_req, p, sizeof(stream_req), bc_sid);
	p = sa(stream_req, p, sizeof(stream_req), "/stream_raw HTTP/1.0\r\nHost: ");
	p = sa(stream_req, p, sizeof(stream_req), bc_host);
	p = sa(stream_req, p, sizeof(stream_req), ":");
	p = ua(stream_req, p, sizeof(stream_req), bc_port);
	p = sa(stream_req, p, sizeof(stream_req), "\r\n\r\n");

	ret = anx_tcp_send(bc_stream, stream_req, p);
	if (ret != ANX_OK) {
		kprintf("browser_cell: stream GET send failed (%d)\n", ret);
		anx_tcp_close(bc_stream);
		bc_stream = NULL;
		return ANX_EIO;
	}

	/* --- Step 3: Allocate receive buffer --- */
	bc_buf = (uint8_t *)anx_alloc(BC_RECV_BUF_SIZE);
	if (!bc_buf) {
		anx_tcp_close(bc_stream);
		bc_stream = NULL;
		kprintf("browser_cell: out of memory\n");
		return ANX_ENOMEM;
	}
	bc_buf_len  = 0;
	bc_hdr_done = false;
	bc_running  = true;

	kprintf("browser_cell: streaming from %s:%u/sessions/%s\n",
		bc_host, (uint32_t)bc_port, bc_sid);
	return ANX_OK;
}

int anx_browser_cell_navigate(const char *url)
{
	char path[128];
	char body[512];
	struct anx_http_response resp;
	uint32_t p;
	int ret;

	if (!bc_running || bc_sid[0] == '\0') {
		kprintf("browser_cell: not initialized\n");
		return ANX_EINVAL;
	}
	if (!url || url[0] == '\0') {
		kprintf("browser_cell: empty URL\n");
		return ANX_EINVAL;
	}

	p = 0;
	p = sa(path, p, sizeof(path), "/api/v1/sessions/");
	p = sa(path, p, sizeof(path), bc_sid);
	sa(path, p, sizeof(path), "/navigate");

	p = 0;
	p = sa(body, p, sizeof(body), "{\"url\":\"");
	p = sa(body, p, sizeof(body), url);
	p = sa(body, p, sizeof(body), "\"}");

	ret = anx_http_post(bc_host, bc_port, path,
			    "application/json", body, p, &resp);
	if (ret != ANX_OK) {
		kprintf("browser_cell: navigate failed (%d)\n", ret);
		anx_http_response_free(&resp);
		return ANX_EIO;
	}

	kprintf("browser_cell: navigating to %s (HTTP %d)\n",
		url, resp.status_code);
	anx_http_response_free(&resp);
	return ANX_OK;
}

void anx_browser_cell_tick(void)
{
	uint8_t chunk[4096];
	int n;

	if (!bc_running || !bc_stream || !bc_buf)
		return;

	/* Non-blocking read: 1ms timeout so the shell poll loop stays live */
	n = anx_tcp_recv(bc_stream, chunk, sizeof(chunk), 1);
	if (n > 0) {
		uint32_t avail = BC_RECV_BUF_SIZE - bc_buf_len;
		uint32_t add   = (uint32_t)n;

		if (add > avail)
			add = avail;
		if (add > 0) {
			anx_memcpy(bc_buf + bc_buf_len, chunk, add);
			bc_buf_len += add;
		}
	}

	/* Skip HTTP response headers on first data arrival */
	if (!bc_hdr_done) {
		uint32_t end = skip_headers(bc_buf, bc_buf_len);

		if (end == 0)
			return;
		kprintf("browser_cell: headers done, body at %u\n", end);
		bc_buf_len -= end;
		if (bc_buf_len > 0)
			anx_memmove(bc_buf, bc_buf + end, bc_buf_len);
		bc_hdr_done = true;
	}

	/* Consume complete [4-byte BE length][JPEG] frames */
	while (bc_buf_len >= 4) {
		uint32_t frame_len;
		uint32_t consumed;
		struct anx_jpeg_image img;
		const struct anx_fb_info *fb;
		int dec;

		frame_len = ((uint32_t)bc_buf[0] << 24) |
			    ((uint32_t)bc_buf[1] << 16) |
			    ((uint32_t)bc_buf[2] <<  8) |
			     (uint32_t)bc_buf[3];

		if (frame_len == 0 || frame_len > (BC_RECV_BUF_SIZE - 4)) {
			/* Frame boundary lost — scan forward for next \x00\x00
			 * pair, which marks the high bytes of a plausible frame
			 * length prefix (valid JPEGs are < 64 KB). */
			uint32_t skip = 1;

			while (skip + 1 < bc_buf_len) {
				if (bc_buf[skip] == 0x00 && bc_buf[skip + 1] == 0x00)
					break;
				skip++;
			}
			bc_buf_len -= skip;
			if (bc_buf_len > 0)
				anx_memmove(bc_buf, bc_buf + skip, bc_buf_len);
			return;
		}

		if (bc_buf_len < 4 + frame_len)
			return;		/* wait for rest of frame */

		dec = anx_jpeg_decode(bc_buf + 4, frame_len, &img);
		if (dec == ANX_OK) {
			fb = anx_fb_get_info();
			if (fb && fb->available)
				anx_jpeg_blit_scaled(&img, fb->width, fb->height);
			anx_jpeg_free(&img);
		}

		consumed    = 4 + frame_len;
		bc_buf_len -= consumed;
		if (bc_buf_len > 0)
			anx_memmove(bc_buf, bc_buf + consumed, bc_buf_len);
	}
}

void anx_browser_cell_stop(void)
{
	bc_running = false;
	if (bc_stream) {
		anx_tcp_close(bc_stream);
		bc_stream = NULL;
	}
	if (bc_buf) {
		anx_free(bc_buf);
		bc_buf = NULL;
	}
	bc_buf_len  = 0;
	bc_hdr_done = false;
	bc_sid[0]   = '\0';
	kprintf("browser_cell: stopped\n");
}

bool anx_browser_cell_active(void)
{
	return bc_running;
}
