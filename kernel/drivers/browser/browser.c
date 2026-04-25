/*
 * browser.c — Anunix native browser daemon.
 *
 * Implements the ANX-Browser Protocol (same REST+WebSocket API as the
 * Python anxbrowserd Lite) so that the existing web UI and desktop UI
 * connect without modification.
 *
 * REST endpoints (same as Python daemon):
 *   GET  /api/v1/health
 *   POST /api/v1/sessions
 *   GET  /api/v1/sessions
 *   GET  /api/v1/sessions/{sid}
 *   DELETE /api/v1/sessions/{sid}
 *   POST /api/v1/sessions/{sid}/navigate
 *   POST /api/v1/sessions/{sid}/claim
 *   POST /api/v1/sessions/{sid}/release
 *   GET  /api/v1/sessions/{sid}/stream   → WebSocket upgrade
 *
 * WebSocket stream messages:
 *   → {"type":"frame","mime":"image/jpeg","data_b64":"..."}
 *   → {"type":"event","kind":"...","payload":{...},"seq":N,"ts":0}
 *   ← {"type":"hello","actor":"...","role":"viewer"}
 *   ← {"type":"cursor","actor":"...","x":N,"y":N}
 *   ← {"type":"navigate","url":"..."}
 *   ← {"type":"pii_response","action":"redact_once|bypass_once|bypass_always"}
 *
 * PII endpoints:
 *   GET  /api/v1/sessions/{sid}/content   — page text (PII-filtered for agents)
 *   POST /api/v1/sessions/{sid}/pii/bypass — approve one-time bypass
 *   GET  /api/v1/pii/whitelist             — list whitelisted domains
 *   POST /api/v1/pii/whitelist             — add domain
 *   DELETE /api/v1/pii/whitelist           — remove domain
 */

#include <anx/browser.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/net.h>
#include "session.h"
#include "ws.h"
#include "forms/forms.h"
#include "layout/layout.h"
#include "paint/paint.h"
#include "pii/pii_filter.h"
#include "pii/pii_whitelist.h"
#include "../../lib/base64.h"

/* ── TCP listener state ──────────────────────────────────────────── */

static struct anx_tcp_conn *pending_rest;   /* next REST request */
static bool browser_ready;

/* ── Buffer helpers ──────────────────────────────────────────────── */

#define RESP_CAP (1024 * 256)  /* 256KB response buffer */

typedef struct {
	char    *buf;
	uint32_t off;
	uint32_t cap;
} sbuf;

static void sb_str(sbuf *s, const char *str)
{
	uint32_t l = (uint32_t)anx_strlen(str);
	if (s->off + l < s->cap) {
		anx_memcpy(s->buf + s->off, str, l);
		s->off += l;
	}
}

static void sb_u32(sbuf *s, uint32_t v)
{
	char tmp[12];
	int pos = 0, i;
	if (v == 0) { tmp[pos++] = '0'; }
	else { while (v > 0 && pos < 11) { tmp[pos++] = '0' + (char)(v % 10); v /= 10; } }
	for (i = 0; i < pos/2; i++) {
		char t = tmp[i]; tmp[i] = tmp[pos-1-i]; tmp[pos-1-i] = t;
	}
	tmp[pos] = '\0';
	sb_str(s, tmp);
}

static void sb_json_str(sbuf *s, const char *str)
{
	sb_str(s, "\"");
	while (*str) {
		if      (*str == '"')  sb_str(s, "\\\"");
		else if (*str == '\\') sb_str(s, "\\\\");
		else if (*str == '\n') sb_str(s, "\\n");
		else if (*str == '\r') sb_str(s, "\\r");
		else if (*str == '\t') sb_str(s, "\\t");
		else {
			char c[2] = { *str, '\0' };
			sb_str(s, c);
		}
		str++;
	}
	sb_str(s, "\"");
}

/* ── HTTP response helpers ───────────────────────────────────────── */

static void send_json(struct anx_tcp_conn *conn,
		       int status, const char *status_txt,
		       const char *json_body)
{
	char *resp = (char *)anx_alloc(4096);
	if (!resp) return;

	uint32_t body_len = (uint32_t)anx_strlen(json_body);
	sbuf s = { resp, 0, 4096 };

	sb_str(&s, "HTTP/1.1 ");
	sb_u32(&s, (uint32_t)status);
	sb_str(&s, " "); sb_str(&s, status_txt); sb_str(&s, "\r\n");
	sb_str(&s, "Content-Type: application/json\r\n");
	sb_str(&s, "Content-Length: "); sb_u32(&s, body_len); sb_str(&s, "\r\n");
	sb_str(&s, "Access-Control-Allow-Origin: *\r\n");
	sb_str(&s, "Connection: close\r\n\r\n");
	sb_str(&s, json_body);

	anx_tcp_srv_send(conn, s.buf, s.off);
	anx_free(resp);
	anx_tcp_srv_close(conn);
}

static void send_204(struct anx_tcp_conn *conn)
{
	const char *r = "HTTP/1.1 204 No Content\r\n"
			"Access-Control-Allow-Origin: *\r\n"
			"Connection: close\r\n\r\n";
	anx_tcp_srv_send(conn, r, (uint32_t)anx_strlen(r));
	anx_tcp_srv_close(conn);
}

static void send_404(struct anx_tcp_conn *conn)
{
	send_json(conn, 404, "Not Found",
		   "{\"error\":\"not_found\",\"message\":\"not found\"}");
}

static void send_500(struct anx_tcp_conn *conn, const char *msg)
{
	char body[256];
	sbuf s = { body, 0, sizeof(body) };
	sb_str(&s, "{\"error\":\"internal_error\",\"message\":");
	sb_json_str(&s, msg);
	sb_str(&s, "}");
	body[s.off] = '\0';
	send_json(conn, 500, "Internal Server Error", body);
}

/* ── URL / JSON parsing helpers ──────────────────────────────────── */

/* Extract path from request line.  buf must be at least 256 bytes. */
static void extract_path(const char *req, char *path, uint32_t path_cap)
{
	const char *p = req;
	/* Skip method */
	while (*p && *p != ' ') p++;
	if (*p == ' ') p++;
	/* Copy path up to ' ' or '?' */
	uint32_t i = 0;
	while (*p && *p != ' ' && *p != '?' && i + 1 < path_cap)
		path[i++] = *p++;
	path[i] = '\0';
}

/* Very small JSON key extractor: find "key":"value" or "key":N */
static bool json_str(const char *json, const char *key,
		       char *val, uint32_t val_cap)
{
	char needle[64];
	const char *p;
	uint32_t ki;

	/* Build search pattern: "key": */
	needle[0] = '"';
	for (ki = 0; key[ki] && ki < 60; ki++)
		needle[ki + 1] = key[ki];
	needle[ki + 1] = '"'; needle[ki + 2] = ':'; needle[ki + 3] = '\0';

	p = json;
	while (*p) {
		if (anx_strncmp(p, needle, anx_strlen(needle)) == 0) {
			p += anx_strlen(needle);
			while (*p == ' ') p++;
			if (*p == '"') {
				p++;
				uint32_t vi = 0;
				while (*p && *p != '"' && vi + 1 < val_cap)
					val[vi++] = *p++;
				val[vi] = '\0';
				return true;
			}
		}
		p++;
	}
	return false;
}

/* Extract {sid} from path like /api/v1/sessions/sess_xxxxxxxx/... */
static bool extract_sid(const char *path, char *sid, uint32_t sid_cap)
{
	const char *prefix = "/api/v1/sessions/";
	uint32_t plen = (uint32_t)anx_strlen(prefix);
	if (anx_strncmp(path, prefix, plen) != 0) return false;
	const char *p = path + plen;
	uint32_t i = 0;
	while (*p && *p != '/' && i + 1 < sid_cap)
		sid[i++] = *p++;
	sid[i] = '\0';
	return i > 0;
}

/* ── REST route handlers ─────────────────────────────────────────── */

static void handle_health(struct anx_tcp_conn *conn)
{
	char body[128];
	sbuf s = { body, 0, sizeof(body) };
	sb_str(&s, "{\"status\":\"ok\",\"version\":\"0.1.0\",");
	sb_str(&s, "\"sessions\":");
	sb_u32(&s, session_count());
	sb_str(&s, ",\"bridge\":\"disabled\",\"engine\":\"anunix-native\"}");
	body[s.off] = '\0';
	send_json(conn, 200, "OK", body);
}

static void handle_list_sessions(struct anx_tcp_conn *conn)
{
	char *body = (char *)anx_alloc(RESP_CAP);
	if (!body) { send_500(conn, "oom"); return; }

	sbuf s = { body, 0, RESP_CAP };
	sb_str(&s, "{\"sessions\":[");
	bool first = true;
	struct browser_session *sess = session_next(NULL);
	while (sess) {
		if (!first) sb_str(&s, ",");
		first = false;
		sb_str(&s, "{\"session_id\":");
		sb_json_str(&s, sess->session_id);
		sb_str(&s, ",\"current_url\":");
		sb_json_str(&s, sess->current_url[0] ? sess->current_url : "about:blank");
		sb_str(&s, ",\"driver\":");
		sb_json_str(&s, sess->driver[0] ? sess->driver : "");
		sb_str(&s, ",\"subscribers\":");
		sb_u32(&s, sess->subscribers);
		sb_str(&s, ",\"created_at\":0}");
		sess = session_next(sess);
	}
	sb_str(&s, "]}");
	body[s.off] = '\0';
	send_json(conn, 200, "OK", body);
	anx_free(body);
}

static void handle_create_session(struct anx_tcp_conn *conn)
{
	struct browser_session *s = session_create();
	if (!s) {
		send_500(conn, "session limit reached");
		return;
	}
	char body[256];
	sbuf sb = { body, 0, sizeof(body) };
	sb_str(&sb, "{\"session_id\":");
	sb_json_str(&sb, s->session_id);
	sb_str(&sb, ",\"cell_id\":null,\"created_at\":0,");
	sb_str(&sb, "\"browser_engine\":\"anunix-native\",");
	sb_str(&sb, "\"stream_url\":\"/api/v1/sessions/");
	sb_str(&sb, s->session_id);
	sb_str(&sb, "/stream\"}");
	body[sb.off] = '\0';
	send_json(conn, 201, "Created", body);
}

static void handle_delete_session(struct anx_tcp_conn *conn,
				    const char *sid)
{
	struct browser_session *s = session_find(sid);
	if (!s) { send_404(conn); return; }
	session_destroy(s);
	send_204(conn);
}

static void handle_navigate(struct anx_tcp_conn *conn,
			      const char *sid,
			      const char *body,
			      uint32_t body_len __attribute__((unused)))
{
	struct browser_session *s = session_find(sid);
	if (!s) { send_404(conn); return; }

	char url[SESSION_URL_LEN];
	if (!json_str(body, "url", url, sizeof(url))) {
		send_json(conn, 400, "Bad Request",
			   "{\"error\":\"missing_url\",\"message\":\"url required\"}");
		return;
	}

	int ret = session_navigate(s, url);
	if (ret != 0) {
		send_500(conn, "navigation failed");
		return;
	}

	char resp[512];
	sbuf sb = { resp, 0, sizeof(resp) };
	sb_str(&sb, "{\"status\":200,\"url\":");
	sb_json_str(&sb, s->current_url);
	sb_str(&sb, ",\"title\":");
	sb_json_str(&sb, s->title);
	sb_str(&sb, ",\"event_id\":");
	sb_u32(&sb, s->event_seq);
	sb_str(&sb, "}");
	resp[sb.off] = '\0';
	send_json(conn, 200, "OK", resp);
}

static void handle_claim(struct anx_tcp_conn *conn, const char *sid)
{
	struct browser_session *s = session_find(sid);
	if (!s) { send_404(conn); return; }
	anx_strlcpy(s->driver, "local:dev", sizeof(s->driver));
	send_json(conn, 200, "OK", "{\"driver\":\"local:dev\"}");
}

static void handle_release(struct anx_tcp_conn *conn, const char *sid)
{
	struct browser_session *s = session_find(sid);
	if (!s) { send_404(conn); return; }
	anx_memset(s->driver, 0, sizeof(s->driver));
	send_204(conn);
}

/* ── PII endpoints ───────────────────────────────────────────────── */

/* GET /api/v1/sessions/{sid}/content */
static void handle_content(struct anx_tcp_conn *conn, const char *sid)
{
	struct browser_session *s = session_find(sid);
	const char *text;
	uint32_t    tlen;

	if (!s) { send_404(conn); return; }

	text = session_agent_content(s);
	if (!text) {
		send_json(conn, 204, "No Content", "");
		return;
	}

	tlen = (uint32_t)anx_strlen(text);

	/* Build response: {"text":"...", "pii_redacted": bool, "types":"..."} */
	char *resp = (char *)anx_alloc(tlen * 2 + 256);
	if (!resp) { send_500(conn, "oom"); return; }

	sbuf sb = { resp, 0, tlen * 2 + 256 };
	sb_str(&sb, "{\"text\":");
	sb_json_str(&sb, text);
	sb_str(&sb, ",\"pii_redacted\":");
	sb_str(&sb, s->pii_redacted ? "true" : "false");
	sb_str(&sb, ",\"types\":");
	sb_json_str(&sb, s->pii_types);
	sb_str(&sb, "}");
	resp[sb.off] = '\0';

	send_json(conn, 200, "OK", resp);
	anx_free(resp);
}

/* POST /api/v1/sessions/{sid}/pii/bypass */
static void handle_pii_bypass(struct anx_tcp_conn *conn, const char *sid)
{
	struct browser_session *s = session_find(sid);
	if (!s) { send_404(conn); return; }
	session_pii_bypass(s);
	send_json(conn, 200, "OK",
		   "{\"status\":\"bypass_approved\"}");
}

/* GET /api/v1/pii/whitelist */
static void handle_whitelist_list(struct anx_tcp_conn *conn)
{
	char domains[PII_WHITELIST_MAX][PII_DOMAIN_MAX];
	uint32_t n = anx_pii_whitelist_list(domains, PII_WHITELIST_MAX);
	uint32_t i;

	char  buf[PII_WHITELIST_MAX * PII_DOMAIN_MAX + 64];
	sbuf  sb = { buf, 0, sizeof(buf) };

	sb_str(&sb, "{\"domains\":[");
	for (i = 0; i < n; i++) {
		if (i) sb_str(&sb, ",");
		sb_json_str(&sb, domains[i]);
	}
	sb_str(&sb, "]}");
	buf[sb.off] = '\0';

	send_json(conn, 200, "OK", buf);
}

/* POST /api/v1/pii/whitelist — body: {"domain":"example.com"} */
static void handle_whitelist_add(struct anx_tcp_conn *conn,
				  const char *body)
{
	char domain[PII_DOMAIN_MAX];
	if (!json_str(body, "domain", domain, sizeof(domain))) {
		send_json(conn, 400, "Bad Request",
			   "{\"error\":\"missing domain\"}");
		return;
	}
	if (anx_pii_whitelist_add(domain) != 0) {
		send_json(conn, 409, "Conflict",
			   "{\"error\":\"whitelist full or duplicate\"}");
		return;
	}
	kprintf("pii: whitelisted %s\n", domain);
	send_json(conn, 201, "Created",
		   "{\"status\":\"added\"}");
}

/* DELETE /api/v1/pii/whitelist — body: {"domain":"example.com"} */
static void handle_whitelist_remove(struct anx_tcp_conn *conn,
				     const char *body)
{
	char domain[PII_DOMAIN_MAX];
	if (!json_str(body, "domain", domain, sizeof(domain))) {
		send_json(conn, 400, "Bad Request",
			   "{\"error\":\"missing domain\"}");
		return;
	}
	if (anx_pii_whitelist_remove(domain) != 0) {
		send_404(conn);
		return;
	}
	send_204(conn);
}

/* ── Form submission ─────────────────────────────────────────────── */

/*
 * Build a submission URL from the form's action and GET-encode the
 * field values, then navigate the session to that URL.
 * action="" means submit to the current page URL.
 */
static void do_form_submit(struct browser_session *s)
{
	char action[512], method[8], qbuf[1024];
	char nav_url[1024];
	uint32_t off;

	form_submit_action(&s->forms, action, sizeof(action),
			   method, sizeof(method));
	form_collect(&s->forms, qbuf, sizeof(qbuf));

	/* If no action URL, submit back to the current page */
	if (!action[0])
		anx_strlcpy(action, s->current_url, sizeof(action));

	/* Only GET encoding supported for now (POST needs body + HTTP method) */
	off  = 0;
	off += anx_strlcpy(nav_url + off, action,  sizeof(nav_url) - off);
	if (qbuf[0]) {
		off += anx_strlcpy(nav_url + off, "?",    sizeof(nav_url) - off);
		off += anx_strlcpy(nav_url + off, qbuf,   sizeof(nav_url) - off);
	}
	(void)off;
	kprintf("browser: form submit → %s\n", nav_url);
	session_navigate(s, nav_url);
}

/* ── WebSocket stream ────────────────────────────────────────────── */

/*
 * Called when a WebSocket connection is established for a session.
 * Runs a streaming loop: send frames, receive control messages.
 * Returns when the connection is closed.
 */
static void run_ws_stream(struct anx_tcp_conn *conn,
			    struct browser_session *s)
{
	/* JPEG scratch buffer: generous for 1280×800 */
	size_t jpeg_cap = SESSION_FB_W * SESSION_FB_H / 2;
	uint8_t *jpeg_buf = (uint8_t *)anx_alloc(jpeg_cap);
	if (!jpeg_buf) {
		anx_ws_close(conn);
		return;
	}

	/* Base64 scratch (JPEG * 4/3 + JSON overhead) */
	size_t b64_cap = ANX_BASE64_ENC_LEN(jpeg_cap) + 128;
	char *b64_buf = (char *)anx_alloc(b64_cap);
	if (!b64_buf) {
		anx_free(jpeg_buf);
		anx_ws_close(conn);
		return;
	}

	/* JSON frame buffer */
	size_t frame_cap = b64_cap + 128;
	char *frame_buf = (char *)anx_alloc(frame_cap);
	if (!frame_buf) {
		anx_free(jpeg_buf); anx_free(b64_buf);
		anx_ws_close(conn);
		return;
	}

	s->ws_conn = conn;
	s->subscribers++;

	/* Send initial connect event */
	{
		char ev[128];
		sbuf sb = { ev, 0, sizeof(ev) };
		sb_str(&sb, "{\"type\":\"event\",\"kind\":\"connected\",");
		sb_str(&sb, "\"payload\":{\"sid\":");
		sb_json_str(&sb, s->session_id);
		sb_str(&sb, "},\"seq\":0,\"ts\":0}");
		ev[sb.off] = '\0';
		anx_ws_send_text(conn, ev, sb.off);
	}

	uint32_t idle = 0;

	while (true) {
		/* Receive any incoming frames (non-blocking poll, 20ms timeout) */
		struct ws_frame in;
		if (anx_ws_recv_frame(conn, &in, 20)) {
			if (in.opcode == WS_OP_CLOSE) {
				anx_free(in.payload);
				break;
			}
			if (in.opcode == WS_OP_TEXT && in.payload_len > 0) {
				char *msg = (char *)in.payload;

				if (anx_strncmp(msg, "{\"type\":\"navigate\"", 18) == 0) {
					char url[SESSION_URL_LEN];
					if (json_str(msg, "url", url, sizeof(url)))
						session_navigate(s, url);

				} else if (anx_strncmp(msg,
						"{\"type\":\"click\"", 16) == 0) {
					/* Mouse click: hit-test form fields */
					char xstr[16], ystr[16];
					if (json_str(msg, "x", xstr, sizeof(xstr)) &&
					    json_str(msg, "y", ystr, sizeof(ystr))) {
						int32_t cx = 0, cy = 0;
						const char *p;
						for (p = xstr; *p >= '0' && *p <= '9'; p++)
							cx = cx * 10 + (*p - '0');
						for (p = ystr; *p >= '0' && *p <= '9'; p++)
							cy = cy * 10 + (*p - '0');
						int32_t hit = form_click(&s->forms, cx, cy);
						/* Re-render to show focus change */
						if (hit >= 0) {
							/* Submit button click → navigate */
							if (s->forms.fields[hit].type ==
							    FORM_TYPE_SUBMIT)
								do_form_submit(s);
							layout_init(&s->layout, SESSION_FB_W);
							{
								struct layout_image limgs2[SESSION_IMG_MAX];
								uint32_t nli2;
								for (nli2 = 0; nli2 < s->n_imgs; nli2++) {
									limgs2[nli2].url    = s->imgs[nli2].url;
									limgs2[nli2].pixels = s->imgs[nli2].pixels;
									limgs2[nli2].w      = s->imgs[nli2].w;
									limgs2[nli2].h      = s->imgs[nli2].h;
								}
								layout_document(&s->layout, &s->doc,
									s->css_index_valid ? &s->css_index : NULL,
									&s->forms, limgs2, s->n_imgs);
							}
							/* Sync focus state into paint cmds */
							{
								uint32_t ci;
								for (ci = 0; ci < s->layout.n_cmds; ci++) {
									struct paint_cmd *pc = &s->layout.cmds[ci];
									if (pc->field_idx == hit)
										pc->focused = true;
								}
							}
							paint_clear(s->fb, SESSION_FB_W, SESSION_FB_H,
								     SESSION_FB_W * 4, 0x00EFECe6U);
							paint_execute(&s->layout, s->fb,
								       SESSION_FB_W, SESSION_FB_H,
								       SESSION_FB_W * 4,
								       s->scroll_y);
							s->ws_dirty = true;
						}
					}

				} else if (anx_strncmp(msg,
						"{\"type\":\"keydown\"", 18) == 0) {
					/* Keyboard input to focused form field */
					char key[32];
					if (json_str(msg, "key", key, sizeof(key))) {
						bool consumed = form_key(&s->forms, key);
						if (consumed) {
							/* Update paint cmd text and re-render */
							int32_t fidx = s->forms.focused_idx;
							if (fidx >= 0) {
								struct form_field *ff =
									&s->forms.fields[fidx];
								uint32_t ci;
								for (ci = 0; ci < s->layout.n_cmds; ci++) {
									struct paint_cmd *pc = &s->layout.cmds[ci];
									if (pc->field_idx == fidx) {
										anx_strlcpy(pc->text, ff->value,
											     PAINT_MAX_TEXT);
										pc->cursor_pos = ff->cursor_pos;
									}
								}
							}
							paint_clear(s->fb, SESSION_FB_W, SESSION_FB_H,
								     SESSION_FB_W * 4, 0x00EFECe6U);
							paint_execute(&s->layout, s->fb,
								       SESSION_FB_W, SESSION_FB_H,
								       SESSION_FB_W * 4,
								       s->scroll_y);
							s->ws_dirty = true;
						}
						/* Enter in a text/submit field triggers submission */
						if (!consumed &&
						    anx_strcmp(key, "Enter") == 0 &&
						    s->forms.focused_idx >= 0)
							do_form_submit(s);
					}

				} else if (anx_strncmp(msg,
						"{\"type\":\"scroll\"", 17) == 0) {
					char dystr[16];
					if (json_str(msg, "dy", dystr, sizeof(dystr))) {
						int32_t dy = 0;
						bool neg = false;
						const char *p = dystr;
						if (*p == '-') { neg = true; p++; }
						while (*p >= '0' && *p <= '9')
							dy = dy * 10 + (*p++ - '0');
						if (neg) dy = -dy;
						session_scroll(s, dy);
					}

				} else if (anx_strncmp(msg,
						"{\"type\":\"pii_response\"", 22) == 0) {
					char action[32];
					if (json_str(msg, "action",
						      action, sizeof(action))) {
						if (anx_strcmp(action,
							"bypass_once") == 0 ||
						    anx_strcmp(action,
							"bypass_always") == 0) {
							if (anx_strcmp(action,
								"bypass_always") == 0) {
								/* Domain whitelist */
								char dom[PII_DOMAIN_MAX];
								anx_strlcpy(dom,
									s->current_url,
									sizeof(dom));
								anx_pii_whitelist_add(dom);
							}
							session_pii_bypass(s);
						}
						/* "redact_once" → no action needed,
						   redacted content already sent */
					}
				}
			}
			anx_free(in.payload);
		}

		/* Emit PII warning event once per page when PII is detected */
		if (s->pii_checked && s->pii_redacted && !s->pii_event_sent) {
			char ev[512];
			sbuf esb = { ev, 0, sizeof(ev) };
			char host[256];
			/* extract domain from current URL for the warning */
			{
				const char *p = s->current_url;
				if (anx_strncmp(p, "https://", 8) == 0) p += 8;
				else if (anx_strncmp(p, "http://", 7) == 0) p += 7;
				uint32_t hlen = 0;
				while (p[hlen] && p[hlen] != '/' &&
				       p[hlen] != ':') hlen++;
				if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
				anx_memcpy(host, p, hlen);
				host[hlen] = '\0';
			}
			sb_str(&esb, "{\"type\":\"event\",\"kind\":\"pii_warning\","
				       "\"payload\":{\"domain\":");
			sb_json_str(&esb, host);
			sb_str(&esb, ",\"types\":");
			sb_json_str(&esb, s->pii_types);
			sb_str(&esb, ",\"sid\":");
			sb_json_str(&esb, s->session_id);
			sb_str(&esb, "},\"seq\":");
			{
				char seq[12];
				sbuf ns = { seq, 0, sizeof(seq) };
				sb_u32(&ns, s->event_seq++);
				seq[ns.off] = '\0';
				sb_str(&esb, seq);
			}
			sb_str(&esb, ",\"ts\":0}");
			ev[esb.off] = '\0';
			anx_ws_send_text(conn, ev, esb.off);
			s->pii_event_sent = true;
		}

		/* Send frame if dirty or every ~10 idle polls (200ms) */
		idle++;
		if (s->ws_dirty || idle >= 10) {
			idle = 0;
			s->ws_dirty = false;

			size_t jpeg_len = session_snapshot_jpeg(s, jpeg_buf, jpeg_cap);
			if (jpeg_len == 0) continue;

			size_t b64_len = anx_base64_encode(jpeg_buf, jpeg_len,
							    b64_buf, b64_cap);

			sbuf sb = { frame_buf, 0, (uint32_t)frame_cap };
			sb_str(&sb, "{\"type\":\"frame\",\"mime\":\"image/jpeg\",\"data_b64\":\"");
			/* Append base64 directly */
			if (sb.off + b64_len + 3 < frame_cap) {
				anx_memcpy(frame_buf + sb.off, b64_buf, b64_len);
				sb.off += (uint32_t)b64_len;
			}
			sb_str(&sb, "\"}");
			frame_buf[sb.off] = '\0';

			if (anx_ws_send_text(conn, frame_buf, sb.off) != 0)
				break;
		}
	}

	s->subscribers--;
	if (s->subscribers == 0) {
		s->ws_conn = NULL;
	}
	anx_free(jpeg_buf);
	anx_free(b64_buf);
	anx_free(frame_buf);
	anx_ws_close(conn);
}

/* ── Request dispatcher ──────────────────────────────────────────── */

#define REQ_BUF_SZ  8192

static void browser_handle_request(struct anx_tcp_conn *conn)
{
	char    *req = (char *)anx_alloc(REQ_BUF_SZ);
	uint32_t total = 0;
	int      n;
	bool     is_ws_req;

	if (!req) { anx_tcp_srv_close(conn); return; }

	/* Read until headers complete */
	while (total < REQ_BUF_SZ - 1) {
		n = anx_tcp_srv_recv(conn, req + total,
				      REQ_BUF_SZ - 1 - total, 3000);
		if (n <= 0) break;
		total += (uint32_t)n;
		req[total] = '\0';
		/* Check for end of headers */
		uint32_t i;
		for (i = 3; i < total; i++) {
			if (req[i-3]=='\r' && req[i-2]=='\n' &&
			    req[i-1]=='\r' && req[i]=='\n') goto headers_done;
		}
	}
headers_done:
	req[total] = '\0';
	if (total == 0) { anx_free(req); anx_tcp_srv_close(conn); return; }

	/* Read body if Content-Length present */
	{
		const char *cl = NULL;
		const char *p = req;
		while (*p) {
			if (anx_strncmp(p, "\r\ncontent-length:", 17) == 0 ||
			    anx_strncmp(p, "\r\nContent-Length:", 17) == 0) {
				cl = p + 17;
				while (*cl == ' ') cl++;
				break;
			}
			p++;
		}
		if (cl) {
			uint32_t clen = 0;
			while (*cl >= '0' && *cl <= '9') {
				clen = clen * 10 + (uint32_t)(*cl - '0');
				cl++;
			}
			/* Find body start */
			uint32_t bi;
			for (bi = 3; bi < total; bi++) {
				if (req[bi-3]=='\r' && req[bi-2]=='\n' &&
				    req[bi-1]=='\r' && req[bi]=='\n') {
					bi++;
					uint32_t have = total - bi;
					while (have < clen && total < REQ_BUF_SZ - 1) {
						n = anx_tcp_srv_recv(conn, req + total,
								      REQ_BUF_SZ-1-total, 2000);
						if (n <= 0) break;
						total += (uint32_t)n;
						have  += (uint32_t)n;
					}
					req[total] = '\0';
					break;
				}
			}
		}
	}

	/* CORS preflight */
	if (anx_strncmp(req, "OPTIONS ", 8) == 0) {
		const char *r =
			"HTTP/1.1 204 No Content\r\n"
			"Access-Control-Allow-Origin: *\r\n"
			"Access-Control-Allow-Methods: GET,POST,DELETE,OPTIONS\r\n"
			"Access-Control-Allow-Headers: Content-Type\r\n"
			"Connection: close\r\n\r\n";
		anx_tcp_srv_send(conn, r, (uint32_t)anx_strlen(r));
		anx_tcp_srv_close(conn);
		anx_free(req);
		return;
	}

	/* Check for WebSocket upgrade */
	is_ws_req = false;
	{
		const char *p = req;
		while (*p) {
			if (anx_strncmp(p, "Upgrade: websocket", 18) == 0 ||
			    anx_strncmp(p, "upgrade: websocket", 18) == 0) {
				is_ws_req = true;
				break;
			}
			p++;
		}
	}

	char path[256];
	extract_path(req, path, sizeof(path));

	bool is_get    = (anx_strncmp(req, "GET ", 4) == 0);
	bool is_post   = (anx_strncmp(req, "POST ", 5) == 0);
	bool is_delete = (anx_strncmp(req, "DELETE ", 7) == 0);

	/* WebSocket upgrade for /stream */
	if (is_ws_req && is_get) {
		char sid[SESSION_ID_LEN];
		if (extract_sid(path, sid, sizeof(sid)) &&
		    anx_strncmp(path + anx_strlen("/api/v1/sessions/") + anx_strlen(sid),
			         "/stream", 7) == 0) {
			struct browser_session *s = session_find(sid);
			if (!s) { send_404(conn); anx_free(req); return; }
			if (anx_ws_upgrade(conn, req, total)) {
				anx_free(req);
				run_ws_stream(conn, s);
				return;
			}
		}
	}

	/* ── REST routing ── */
	/* Shared body extraction for POST/DELETE */
	const char *body = "";
	uint32_t    body_len = 0;
	if (is_post || is_delete) {
		uint32_t i;
		for (i = 3; i < total; i++) {
			if (req[i-3]=='\r' && req[i-2]=='\n' &&
			    req[i-1]=='\r' && req[i]=='\n') {
				body     = req + i + 1;
				body_len = total - i - 1;
				break;
			}
		}
	}

	if (is_get && anx_strcmp(path, "/api/v1/health") == 0) {
		handle_health(conn);
	} else if (is_get && anx_strcmp(path, "/api/v1/sessions") == 0) {
		handle_list_sessions(conn);
	} else if (is_post && anx_strcmp(path, "/api/v1/sessions") == 0) {
		handle_create_session(conn);

	/* PII whitelist endpoints (no {sid} prefix) */
	} else if (is_get &&
		    anx_strcmp(path, "/api/v1/pii/whitelist") == 0) {
		handle_whitelist_list(conn);
	} else if (is_post &&
		    anx_strcmp(path, "/api/v1/pii/whitelist") == 0) {
		handle_whitelist_add(conn, body);
	} else if (is_delete &&
		    anx_strcmp(path, "/api/v1/pii/whitelist") == 0) {
		handle_whitelist_remove(conn, body);

	} else {
		char sid[SESSION_ID_LEN];
		if (extract_sid(path, sid, sizeof(sid))) {
			size_t base_len = anx_strlen("/api/v1/sessions/")
					+ anx_strlen(sid);
			const char *suffix = path + base_len;

			if (is_delete && anx_strcmp(suffix, "") == 0) {
				handle_delete_session(conn, sid);
			} else if (is_get &&
				    anx_strcmp(suffix, "/content") == 0) {
				handle_content(conn, sid);
			} else if (is_post &&
				    anx_strcmp(suffix, "/pii/bypass") == 0) {
				handle_pii_bypass(conn, sid);
			} else if (is_post &&
				    anx_strcmp(suffix, "/navigate") == 0) {
				handle_navigate(conn, sid, body, body_len);
			} else if (is_post &&
				    anx_strcmp(suffix, "/claim") == 0) {
				handle_claim(conn, sid);
			} else if (is_post &&
				    anx_strcmp(suffix, "/release") == 0) {
				handle_release(conn, sid);
			} else {
				send_404(conn);
			}
		} else {
			send_404(conn);
		}
	}

	anx_free(req);
}

/* ── Accept callback & poll ──────────────────────────────────────── */

static void browser_accept(struct anx_tcp_conn *conn, void *arg)
{
	(void)arg;
	pending_rest = conn;
}

int anx_browser_init(uint16_t port)
{
	int ret;

	session_manager_init();
	anx_pii_whitelist_init();
	anx_pii_init(NULL);   /* auto-config: native if model client ready */
	ret = anx_tcp_listen(port, browser_accept, NULL);
	if (ret != 0) {
		kprintf("browser: failed to listen on port %u\n",
			(uint32_t)port);
		return ret;
	}
	browser_ready = true;
	kprintf("browser: ANX-Browser Protocol on port %u (native engine)\n",
		(uint32_t)port);
	return 0;
}

void anx_browser_poll(void)
{
	if (!browser_ready) return;
	if (pending_rest) {
		struct anx_tcp_conn *conn = pending_rest;
		pending_rest = NULL;
		browser_handle_request(conn);
	}
}
