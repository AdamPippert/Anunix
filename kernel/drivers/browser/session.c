/*
 * session.c — Browser session lifecycle.
 */

#include "session.h"
#include "jpeg_enc.h"
#include "html/tokenizer.h"
#include "html/dom.h"
#include "layout/layout.h"
#include "css/css_parser.h"
#include "css/css_selector.h"
#include "paint/paint.h"
#include "pii/pii_filter.h"
#include "pii/pii_whitelist.h"
#include "js/js_engine.h"
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
	if (s->page_text) {
		anx_free(s->page_text);
		s->page_text = NULL;
	}
	css_sheet_free(&s->css_sheet);
	s->css_index_valid = false;
	js_engine_destroy(&s->js);

	if (s->pii_redacted) {
		anx_free(s->pii_redacted);
		s->pii_redacted = NULL;
	}
	kprintf("browser: destroyed session %s\n", s->session_id);
	anx_memset(s, 0, sizeof(*s));
}

/* ── DOM text extraction ─────────────────────────────────────────── */

/* Tags whose text content is not visible to the user or agent. */
static bool is_invisible_tag(const char *tag)
{
	static const char *hidden[] = {
		"head", "script", "style", "meta", "link",
		"noscript", NULL
	};
	int i;
	for (i = 0; hidden[i]; i++) {
		if (anx_strcmp(tag, hidden[i]) == 0)
			return true;
	}
	return false;
}

static bool is_block_tag(const char *tag)
{
	static const char *blocks[] = {
		"p", "div", "h1", "h2", "h3", "h4", "h5", "h6",
		"li", "blockquote", "pre", "section", "article",
		"header", "footer", "main", "nav", "aside",
		"table", "tr", "td", "th", NULL
	};
	int i;
	for (i = 0; blocks[i]; i++) {
		if (anx_strcmp(tag, blocks[i]) == 0)
			return true;
	}
	return false;
}

static void extract_text_node(const struct dom_node *node,
			       char *buf, size_t cap, size_t *pos)
{
	uint32_t i;

	if (!node) return;

	if (node->type == DOM_TEXT) {
		const char *t = node->txt.text;
		size_t tlen = anx_strlen(t);

		/* Skip pure-whitespace text nodes */
		bool has_content = false;
		size_t j;
		for (j = 0; j < tlen; j++) {
			if (t[j] != ' ' && t[j] != '\t' &&
			    t[j] != '\n' && t[j] != '\r') {
				has_content = true;
				break;
			}
		}
		if (!has_content) return;

		/* Append text with space separator */
		if (*pos > 0 && buf[*pos - 1] != '\n' &&
		    buf[*pos - 1] != ' ' && *pos < cap - 1)
			buf[(*pos)++] = ' ';

		for (j = 0; j < tlen && *pos < cap - 1; j++)
			buf[(*pos)++] = t[j];
		return;
	}

	if (node->type != DOM_ELEMENT) return;

	if (is_invisible_tag(node->el.tag)) return;

	/* Block elements get a newline before their content */
	if (is_block_tag(node->el.tag) && *pos > 0 &&
	    buf[*pos - 1] != '\n' && *pos < cap - 1)
		buf[(*pos)++] = '\n';

	for (i = 0; i < node->el.n_children; i++)
		extract_text_node(node->el.children[i], buf, cap, pos);

	/* Block elements get a newline after their content */
	if (is_block_tag(node->el.tag) && *pos > 0 &&
	    buf[*pos - 1] != '\n' && *pos < cap - 1)
		buf[(*pos)++] = '\n';
}

/* Extract all visible text from the DOM into a heap-allocated string. */
static char *dom_to_text(const struct dom_doc *doc)
{
	char    *buf;
	size_t   cap = PII_MAX_CONTENT;
	size_t   pos = 0;

	buf = anx_alloc(cap);
	if (!buf) return NULL;

	extract_text_node(doc->root, buf, cap, &pos);
	if (pos < cap) buf[pos] = '\0';
	return buf;
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

	/* Reset per-page state */
	if (s->page_text)    { anx_free(s->page_text);    s->page_text    = NULL; }
	if (s->pii_redacted) { anx_free(s->pii_redacted); s->pii_redacted = NULL; }
	s->pii_types[0]    = '\0';
	s->pii_checked     = false;
	s->pii_bypass      = false;
	css_sheet_free(&s->css_sheet);
	css_sheet_init(&s->css_sheet);
	css_index_clear(&s->css_index);
	s->css_index_valid = false;

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

	/* Extract plain text for agent content delivery + PII checking */
	s->page_text = dom_to_text(&s->doc);

	/* Build author CSS index from <style> blocks in the parsed DOM */
	css_sheet_free(&s->css_sheet);
	css_sheet_init(&s->css_sheet);
	css_index_clear(&s->css_index);
	s->css_index_valid = false;
	{
		uint32_t ni;
		for (ni = 0; ni < s->doc.n_nodes; ni++) {
			struct dom_node *node = &s->doc.nodes[ni];
			if (node->type == DOM_ELEMENT &&
			    anx_strcmp(node->el.tag, "style") == 0) {
				uint32_t ci;
				for (ci = 0; ci < node->el.n_children; ci++) {
					struct dom_node *ch = node->el.children[ci];
					if (ch && ch->type == DOM_TEXT &&
					    ch->txt.text[0]) {
						css_parse(ch->txt.text,
							   anx_strlen(ch->txt.text),
							   &s->css_sheet,
							   CSS_ORIGIN_AUTHOR);
					}
				}
			}
		}
		css_index_build(&s->css_index, &s->css_sheet);
		s->css_index_valid = true;
		kprintf("browser: CSS rules parsed: %u\n",
			s->css_sheet.n_rules);
	}

	/* JavaScript: init engine and execute all <script> blocks */
	js_engine_destroy(&s->js);
	anx_memset(&s->js_heap, 0, sizeof(s->js_heap));
	js_engine_init(&s->js, &s->js_heap, &s->doc);
	{
		uint32_t ni;
		for (ni = 0; ni < s->doc.n_nodes; ni++) {
			struct dom_node *node = &s->doc.nodes[ni];
			if (node->type != DOM_ELEMENT) continue;
			if (anx_strcmp(node->el.tag, "script") != 0) continue;
			/* Find first text child */
			uint32_t ci;
			for (ci = 0; ci < node->el.n_children; ci++) {
				struct dom_node *ch = node->el.children[ci];
				if (ch && ch->type == DOM_TEXT && ch->txt.text[0]) {
					js_engine_load(&s->js, ch->txt.text,
					               anx_strlen(ch->txt.text));
					break;
				}
			}
		}
	}

	anx_http_response_free(&resp);

render:
	/* Layout */
	layout_init(&s->layout, SESSION_FB_W);
	layout_document(&s->layout, &s->doc,
			  s->css_index_valid ? &s->css_index : NULL,
			  &s->forms);

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

/* ── PII-safe content delivery ───────────────────────────────────── */

const char *session_agent_content(struct browser_session *s)
{
	if (!s || !s->page_text) return NULL;

	/* Human session or whitelisted domain: pass through without checking */
	if (!s->driver[0] || anx_pii_whitelist_check(s->current_url))
		return s->page_text;

	/* One-time bypass approved by user */
	if (s->pii_bypass) {
		s->pii_bypass = false;
		return s->page_text;
	}

	/* Run PII check lazily on first agent read of this page */
	if (!s->pii_checked) {
		struct pii_result result;
		int rc;

		s->pii_checked = true;
		rc = anx_pii_check(s->page_text, anx_strlen(s->page_text),
				    &result);
		if (rc == 0 && result.found && result.redacted) {
			/* Store redacted version; keep original for bypass */
			s->pii_redacted = result.redacted;
			result.redacted = NULL;   /* transfer ownership */
			anx_strlcpy(s->pii_types, result.types,
				    sizeof(s->pii_types));
		} else {
			/* No PII or model unavailable — clear redacted */
			if (s->pii_redacted) {
				anx_free(s->pii_redacted);
				s->pii_redacted = NULL;
			}
		}
		anx_pii_result_free(&result);
	}

	return s->pii_redacted ? s->pii_redacted : s->page_text;
}

void session_pii_bypass(struct browser_session *s)
{
	if (s) s->pii_bypass = true;
}
