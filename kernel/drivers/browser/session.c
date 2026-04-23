/*
 * session.c — Browser session lifecycle.
 */

#include "session.h"
#include "jpeg_enc.h"
#include "html/tokenizer.h"
#include "html/dom.h"
#include "layout/layout.h"
#include "paint/paint.h"
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/net.h>
#include <anx/http.h>

/* A simple monotonic counter for session IDs */
static uint32_t s_id_counter;
static struct browser_session s_sessions[SESSION_MAX];

/* ── Utility ─────────────────────────────────────────────────────── */

static void u32_to_hex(uint32_t v, char *buf, int digits)
{
	static const char hex[] = "0123456789abcdef";
	int i;
	for (i = digits - 1; i >= 0; i--) {
		buf[i] = hex[v & 0xF];
		v >>= 4;
	}
}

/* ── Public API ──────────────────────────────────────────────────── */

void session_manager_init(void)
{
	anx_memset(s_sessions, 0, sizeof(s_sessions));
	s_id_counter = 0x1000;
}

struct browser_session *session_create(void)
{
	uint32_t i;
	struct browser_session *s = NULL;

	for (i = 0; i < SESSION_MAX; i++) {
		if (!s_sessions[i].active) {
			s = &s_sessions[i];
			break;
		}
	}
	if (!s) return NULL;

	anx_memset(s, 0, sizeof(*s));

	/* Allocate off-screen framebuffer */
	s->fb = (uint32_t *)anx_alloc(
		SESSION_FB_W * SESSION_FB_H * sizeof(uint32_t));
	if (!s->fb) return NULL;

	/* Generate session ID */
	s_id_counter++;
	anx_strlcpy(s->session_id, "sess_", SESSION_ID_LEN);
	u32_to_hex(s_id_counter, s->session_id + 5, 8);
	s->session_id[13] = '\0';

	/* Initial framebuffer: Aether paper background */
	paint_clear(s->fb, SESSION_FB_W, SESSION_FB_H,
		     SESSION_FB_W * 4, 0x00EFECe6U);

	s->active     = true;
	s->created_at = 0;  /* TODO: real timestamp from RTC */
	s->ws_dirty   = true;

	kprintf("browser: created session %s\n", s->session_id);
	return s;
}

struct browser_session *session_find(const char *sid)
{
	uint32_t i;
	if (!sid) return NULL;
	for (i = 0; i < SESSION_MAX; i++) {
		if (s_sessions[i].active &&
		    anx_strcmp(s_sessions[i].session_id, sid) == 0)
			return &s_sessions[i];
	}
	return NULL;
}

void session_destroy(struct browser_session *s)
{
	if (!s) return;
	if (s->ws_conn) {
		anx_tcp_srv_close(s->ws_conn);
		s->ws_conn = NULL;
	}
	if (s->fb) {
		anx_free(s->fb);
		s->fb = NULL;
	}
	kprintf("browser: destroyed session %s\n", s->session_id);
	anx_memset(s, 0, sizeof(*s));
}

uint32_t session_count(void)
{
	uint32_t i, n = 0;
	for (i = 0; i < SESSION_MAX; i++)
		if (s_sessions[i].active) n++;
	return n;
}

struct browser_session *session_next(const struct browser_session *prev)
{
	uint32_t start = 0;
	uint32_t i;

	if (prev) {
		for (i = 0; i < SESSION_MAX; i++) {
			if (&s_sessions[i] == prev) {
				start = i + 1;
				break;
			}
		}
	}
	for (i = start; i < SESSION_MAX; i++) {
		if (s_sessions[i].active)
			return &s_sessions[i];
	}
	return NULL;
}

/* ── Navigation ──────────────────────────────────────────────────── */

/*
 * Parse a URL into host, port, and path.
 * Supports http:// only (HTTPS requires in-kernel TLS, coming later).
 */
static bool parse_url(const char *url, char *host, uint32_t host_cap,
		        uint16_t *port, char *path, uint32_t path_cap)
{
	const char *p = url;

	/* Strip scheme */
	if (anx_strncmp(p, "http://", 7) == 0) {
		p += 7;
		*port = 80;
	} else if (anx_strncmp(p, "https://", 8) == 0) {
		p += 8;
		*port = 443;
	} else {
		return false;
	}

	/* Extract host (up to '/', ':', or end) */
	const char *host_start = p;
	while (*p && *p != '/' && *p != ':') p++;
	uint32_t hlen = (uint32_t)(p - host_start);
	if (hlen == 0 || hlen >= host_cap) return false;
	anx_memcpy(host, host_start, hlen);
	host[hlen] = '\0';

	/* Optional port */
	if (*p == ':') {
		p++;
		uint16_t port_val = 0;
		while (*p >= '0' && *p <= '9') {
			port_val = (uint16_t)(port_val * 10 + (*p - '0'));
			p++;
		}
		*port = port_val;
	}

	/* Path */
	if (*p == '/') {
		anx_strlcpy(path, p, path_cap);
	} else {
		anx_strlcpy(path, "/", path_cap);
	}

	return true;
}

int session_navigate(struct browser_session *s, const char *url)
{
	char     host[256], path[512];
	uint16_t port;
	struct   anx_http_response resp;
	int      ret;

	if (!url || !s) return -1;

	anx_strlcpy(s->current_url, url, SESSION_URL_LEN);
	kprintf("browser: navigating %s to %s\n", s->session_id, url);

	if (!parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
		/* Can't parse URL — render error page */
		anx_strlcpy(s->title, "Navigation Error", sizeof(s->title));
		goto render;
	}

	/* Fetch the page (HTTP only for now; HTTPS via kernel TLS proxy later) */
	anx_memset(&resp, 0, sizeof(resp));
	ret = anx_http_get(host, port, path, &resp);
	if (ret != 0 || !resp.body || resp.status_code < 200 || resp.status_code > 399) {
		kprintf("browser: fetch failed (ret=%d status=%u)\n",
			ret, (uint32_t)resp.status_code);
		anx_strlcpy(s->title, "Connection Error", sizeof(s->title));
		if (resp.body) anx_http_response_free(&resp);
		goto render;
	}

	/* Parse HTML */
	{
		struct html_tokenizer tok;
		struct dom_builder    builder;

		dom_builder_init(&builder, &s->doc);
		html_tokenizer_init(&tok, dom_builder_token, &builder);
		html_tokenizer_feed(&tok, resp.body, anx_strlen(resp.body));
		html_tokenizer_eof(&tok);

		/* Copy title */
		if (s->doc.title[0])
			anx_strlcpy(s->title, s->doc.title, sizeof(s->title));
		else
			anx_strlcpy(s->title, url, sizeof(s->title));
	}

	anx_http_response_free(&resp);

render:
	/* Layout */
	layout_init(&s->layout, SESSION_FB_W);
	layout_document(&s->layout, &s->doc);

	/* Paint */
	paint_clear(s->fb, SESSION_FB_W, SESSION_FB_H,
		     SESSION_FB_W * 4, 0x00EFECe6U);
	paint_execute(&s->layout, s->fb, SESSION_FB_W, SESSION_FB_H,
		       SESSION_FB_W * 4);

	s->ws_dirty = true;
	s->event_seq++;
	return 0;
}

size_t session_snapshot_jpeg(struct browser_session *s,
			       uint8_t *out_buf, size_t out_cap)
{
	if (!s || !s->fb) return 0;
	return anx_jpeg_encode(s->fb, SESSION_FB_W, SESSION_FB_H,
			        SESSION_FB_W * 4, out_buf, out_cap);
}
