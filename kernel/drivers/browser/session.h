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
#include "css/css_parser.h"
#include "css/css_selector.h"
#include "forms/forms.h"
#include "pii/pii_filter.h"
#include "js/js_engine.h"

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

	/* CSS engine: author stylesheet for the current page */
	struct css_sheet          css_sheet;
	struct css_selector_index css_index;
	bool                      css_index_valid;

	/* Form element state */
	struct form_state forms;

	/* JavaScript engine */
	struct js_engine  js;
	struct js_heap    js_heap;

	/* Decoded image cache — populated after rq_fetch_images(), cleared on navigate */
#define SESSION_IMG_MAX 32
	struct {
		char      url[512];   /* src attribute, matched by layout engine */
		uint32_t *pixels;     /* XRGB8888; heap-allocated by webp_decode */
		uint32_t  w, h;
	} imgs[SESSION_IMG_MAX];
	uint32_t n_imgs;

	/* Scroll state — reset to 0 on each navigate */
	int32_t  scroll_y;    /* current vertical offset in pixels */
	int32_t  scroll_max;  /* max scroll_y = content_h - SESSION_FB_H (or 0) */

	/* WebSocket streaming connection (NULL when no subscriber) */
	struct anx_tcp_conn *ws_conn;
	bool                 ws_dirty;  /* true when a new frame is ready */

	/*
	 * PII filter state.
	 * page_text     — plain text extracted from DOM after navigate.
	 * pii_redacted  — redacted version (heap-alloc, NULL if no PII found).
	 * pii_types     — comma-separated PII categories found.
	 * pii_bypass    — true when the user has approved one bypass.
	 */
	char *page_text;           /* heap-allocated; freed on next navigate */
	char *pii_redacted;        /* heap-allocated; freed on next navigate */
	char  pii_types[256];
	bool  pii_checked;         /* true once PII filter has run on current page */
	bool  pii_bypass;          /* user approved one-time bypass */
	bool  pii_event_sent;      /* true after pii_warning WS event was emitted */
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
 * Scroll session by dy pixels (positive = down). Clamps to [0, scroll_max],
 * re-renders the framebuffer, and marks ws_dirty.
 */
void session_scroll(struct browser_session *s, int32_t dy);

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

/*
 * Return the appropriate page text for an agent consumer.
 * If PII was detected and no bypass is active, returns the redacted
 * version.  If bypass is active (user approved), returns original and
 * clears the one-shot bypass flag.
 * Returns NULL when no page text is available.
 * The returned pointer is owned by the session — do not free it.
 */
const char *session_agent_content(struct browser_session *s);

/*
 * Approve a one-time PII bypass for this session.
 * The next call to session_agent_content() will return the original text.
 */
void session_pii_bypass(struct browser_session *s);

#endif /* ANX_BROWSER_SESSION_H */
