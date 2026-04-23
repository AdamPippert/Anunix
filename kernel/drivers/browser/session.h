/*
 * session.h — Browser session lifecycle.
 *
 * Each session is an independent browser "tab": it has its own
 * off-screen framebuffer, DOM tree, layout context, and URL state.
 * Sessions are addressed by a short hex ID ("sess_xxxxxxxx").
 */

#ifndef ANX_BROWSER_SESSION_H
#define ANX_BROWSER_SESSION_H

#include <anx/types.h>
#include <anx/net.h>
#include "html/dom.h"
#include "layout/layout.h"

#define SESSION_MAX       4
#define SESSION_ID_LEN   16
#define SESSION_URL_LEN 512

/* Frame buffer dimensions for off-screen rendering */
#define SESSION_FB_W    1280
#define SESSION_FB_H     800

struct browser_session {
	bool     active;
	char     session_id[SESSION_ID_LEN];
	char     current_url[SESSION_URL_LEN];
	char     title[256];
	char     driver[64];      /* empty = no driver claimed */
	uint32_t subscribers;
	uint64_t created_at;      /* unix timestamp */
	uint32_t event_seq;

	/* Off-screen framebuffer (XRGB8888) */
	uint32_t *fb;             /* SESSION_FB_W * SESSION_FB_H * 4 bytes */

	/* Parsed document state */
	struct dom_doc    doc;
	struct layout_ctx layout;

	/* WebSocket streaming connection (NULL when no subscriber) */
	struct anx_tcp_conn *ws_conn;
	bool                 ws_dirty;  /* true when a new frame is ready */
};

/* ── Session manager ─────────────────────────────────────────────── */

void session_manager_init(void);

/*
 * Create a new session.  Returns pointer to session on success, NULL
 * if at capacity.
 */
struct browser_session *session_create(void);

/* Find session by ID; returns NULL if not found. */
struct browser_session *session_find(const char *sid);

/* Destroy a session (free framebuffer, close WebSocket). */
void session_destroy(struct browser_session *s);

/* Count active sessions. */
uint32_t session_count(void);

/* Iterate active sessions; pass NULL to start, returns NULL when done. */
struct browser_session *session_next(const struct browser_session *prev);

/*
 * Navigate session to URL.
 * Fetches the page over HTTP, tokenizes, builds DOM, lays out, and
 * marks the framebuffer dirty for the next stream frame.
 * Returns 0 on success, non-zero on network/parse error.
 */
int session_navigate(struct browser_session *s, const char *url);

/*
 * Encode the session framebuffer as JPEG and write it into out_buf.
 * Returns number of bytes written.
 */
size_t session_snapshot_jpeg(struct browser_session *s,
			       uint8_t *out_buf, size_t out_cap);

#endif /* ANX_BROWSER_SESSION_H */
