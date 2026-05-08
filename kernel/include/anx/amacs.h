/*
 * anx/amacs.h — Object-Native Editor (RFC-0023).
 *
 * Three layers exposed in this header:
 *   - eLISP value model + interpreter entry points (elisp.c)
 *   - Gap-buffer engine (buffer.c)
 *   - Editor cell dispatch hook (cell.c)
 */

#ifndef ANX_AMACS_H
#define ANX_AMACS_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Buffer engine                                                       */
/* ------------------------------------------------------------------ */

#define ANX_ED_BUF_INITIAL	4096
#define ANX_ED_BUF_MAX		(8 * 1024 * 1024)

struct anx_ed_marker;
struct anx_ed_interval;

struct anx_ed_buffer {
	char     *data;
	uint32_t  size;		/* total allocation */
	uint32_t  gap_start;
	uint32_t  gap_end;
	uint32_t  point;	/* logical 0-indexed position */
	anx_oid_t source_oid;	/* optional source OID, zero if synthetic */
	bool      dirty;
	struct anx_ed_marker   *markers;	/* intrusive list */
	struct anx_ed_interval *intervals;	/* sorted, non-overlapping */
};

/*
 * A marker is a buffer position that survives edits.  Insert before it
 * shifts it forward; delete spanning it pulls it back to the deletion
 * start.  insertion_type=false (default) means an insert exactly at the
 * marker leaves it in place; true makes it move forward with the insert.
 *
 * A marker is "detached" when its buffer has been freed (buf=NULL); a
 * detached marker reads back as nil from marker-position.
 */
struct anx_ed_marker {
	struct anx_ed_buffer *buf;	/* NULL when detached */
	uint32_t              pos;
	bool                  insertion_type;
	uint32_t              rc;
	struct anx_ed_marker *next;	/* in buf->markers */
};

struct anx_ed_marker *anx_ed_marker_new(struct anx_ed_buffer *buf,
					uint32_t pos);
void     anx_ed_marker_retain(struct anx_ed_marker *m);
void     anx_ed_marker_release(struct anx_ed_marker *m);
void     anx_ed_marker_set(struct anx_ed_marker *m,
			   struct anx_ed_buffer *buf, uint32_t pos);
uint32_t anx_ed_marker_position(const struct anx_ed_marker *m);
bool     anx_ed_marker_attached(const struct anx_ed_marker *m);

int  anx_ed_buf_create(struct anx_ed_buffer **out);
int  anx_ed_buf_create_from_bytes(const char *bytes, uint32_t len,
				  struct anx_ed_buffer **out);
void anx_ed_buf_free(struct anx_ed_buffer *buf);
uint32_t anx_ed_buf_length(const struct anx_ed_buffer *buf);
int  anx_ed_buf_goto(struct anx_ed_buffer *buf, uint32_t pos);
int  anx_ed_buf_insert(struct anx_ed_buffer *buf, const char *s, uint32_t n);
int  anx_ed_buf_delete(struct anx_ed_buffer *buf, uint32_t n);
int  anx_ed_buf_text(const struct anx_ed_buffer *buf, char *out,
		     uint32_t out_size, uint32_t *written);
int  anx_ed_buf_search(const struct anx_ed_buffer *buf,
		       const char *needle, uint32_t *match_pos);
int  anx_ed_buf_replace_all(struct anx_ed_buffer *buf,
			    const char *needle, const char *replacement,
			    uint32_t *count);

/* ------------------------------------------------------------------ */
/* Text properties (Phase 2.1)                                         */
/* ------------------------------------------------------------------ */

/*
 * A property is a (name, value) string pair attached to a half-open
 * range [start, end).  Properties move with the text: insert before a
 * range shifts it forward; delete spanning a range trims or removes it.
 *
 * v1 stores values as strings.  Symbol/integer values are not yet
 * supported — fontification only needs string-named faces.
 */
struct anx_ed_textprop {
	char *name;
	char *value;
	struct anx_ed_textprop *next;
};

struct anx_ed_interval {
	uint32_t start;
	uint32_t end;	/* exclusive */
	struct anx_ed_textprop *props;
	struct anx_ed_interval *next;
};

/* Set NAME=VALUE on every position in [start, end).  Splits intervals at
 * the boundary as needed.  Overwrites any prior value. */
int  anx_ed_buf_put_property(struct anx_ed_buffer *buf,
			     uint32_t start, uint32_t end,
			     const char *name, const char *value);

/* Lookup the property NAME at POS.  Returns the stored value pointer
 * (owned by the buffer) or NULL. */
const char *anx_ed_buf_get_property(const struct anx_ed_buffer *buf,
				    uint32_t pos, const char *name);

/* Remove NAME from every interval intersecting [start, end). */
int  anx_ed_buf_remove_property(struct anx_ed_buffer *buf,
				uint32_t start, uint32_t end,
				const char *name);

/* Drop all property intervals.  Used by fontifiers before re-applying. */
void anx_ed_buf_clear_properties(struct anx_ed_buffer *buf);

/* ------------------------------------------------------------------ */
/* Faces (Phase 2.2)                                                   */
/* ------------------------------------------------------------------ */

#define ANX_ED_MAX_FACES	32

struct anx_ed_face {
	char     *name;		/* owned; NULL when slot empty */
	uint32_t  fg;		/* 0x00RRGGBB */
	uint32_t  bg;		/* 0x00RRGGBB; 0xff000000 = "use default bg" */
};

#define ANX_ED_FACE_INHERIT_BG	0xff000000u

/* Install built-in faces (default, region, comment, string, keyword,
 * org-level-1..6, org-todo, org-done, link, etc.).  Idempotent. */
void anx_ed_face_init_defaults(void);

/* Add or update a face by name.  Returns ANX_OK on success, ANX_ENOMEM
 * when the registry is full or strdup fails. */
int  anx_ed_face_define(const char *name, uint32_t fg, uint32_t bg);

/* Lookup a face by name; NULL if not registered. */
const struct anx_ed_face *anx_ed_face_lookup(const char *name);

/* ------------------------------------------------------------------ */
/* Keymap (Phase 2.7)                                                  */
/* ------------------------------------------------------------------ */

/*
 * v1 keymap is a flat list of single-chord global bindings.  Multi-key
 * sequences (C-x C-s style) and per-mode keymaps arrive in 2.8.
 *
 * Bindings store the function as a symbol name; at dispatch time, the
 * editor evaluates `(funcall 'NAME)` in the active session.
 */
#define ANX_ED_MAX_KEYBINDINGS	64
#define ANX_ED_KEY_FN_MAX	64

/* Register a binding.  Returns ANX_OK or an error code.  Re-binding the
 * same (mods, key) overwrites the existing entry. */
int  anx_ed_keymap_define(uint32_t mods, uint32_t key, const char *fn_name);

/* Lookup; returns the bound function name (owned by the keymap) or NULL. */
const char *anx_ed_keymap_lookup(uint32_t mods, uint32_t key);

/* Parse a textual key descriptor like "C-c", "M-x", "RET", "<f5>" into
 * (mods, key).  Returns ANX_OK on success. */
int  anx_ed_keymap_parse(const char *desc, uint32_t *mods_out, uint32_t *key_out);

/* ------------------------------------------------------------------ */
/* eLISP value model                                                   */
/* ------------------------------------------------------------------ */

enum anx_lv_tag {
	ANX_LV_NIL = 0,	/* () / false  */
	ANX_LV_T,	/* canonical truthy symbol */
	ANX_LV_INT,
	ANX_LV_STR,
	ANX_LV_SYM,
	ANX_LV_CONS,
	ANX_LV_FN,	/* lambda */
	ANX_LV_BUILTIN,
	ANX_LV_BUF,	/* buffer handle (small int into per-cell table) */
	ANX_LV_MARKER,	/* refcounted pointer to anx_ed_marker */
};

struct anx_lv;
struct anx_lv_env;

typedef struct anx_lv *(*anx_lv_builtin_fn)(struct anx_lv *args,
					    struct anx_lv_env *env,
					    void *ctx);

struct anx_lv_cons {
	struct anx_lv *car;
	struct anx_lv *cdr;
};

struct anx_lv_fn {
	struct anx_lv     *params;	/* list of symbol */
	struct anx_lv     *body;	/* list of forms */
	struct anx_lv_env *closure;
	bool               macro;	/* true: receive args unevaluated,
					 * eval the returned form */
};

struct anx_lv_builtin {
	const char        *name;
	anx_lv_builtin_fn  fn;
	uint8_t            min_args;
	uint8_t            max_args;	/* 0xff = unlimited */
};

struct anx_lv {
	enum anx_lv_tag tag;
	uint32_t        rc;
	union {
		int64_t                       i;
		struct {
			char    *bytes;
			uint32_t len;
		} s;					/* STR / SYM */
		struct anx_lv_cons            cons;
		struct anx_lv_fn              fn;
		const struct anx_lv_builtin  *builtin;
		uint32_t                      buf_handle;	/* BUF */
		struct anx_ed_marker         *marker;		/* MARKER */
	} u;
};

struct anx_lv_env {
	struct anx_lv     *bindings;	/* assoc list ((sym . val) ...) */
	struct anx_lv_env *parent;
	uint32_t           rc;
};

/* Interpreter session (per cell invocation) */
#define ANX_ED_MAX_BUFFERS	16

struct anx_ed_session {
	struct anx_lv_env *root_env;
	struct anx_ed_buffer *buffers[ANX_ED_MAX_BUFFERS];
	/* In-band error signaling — set by (error …), checked at the top of
	 * every eval, cleared by condition-case.  No setjmp needed. */
	bool             error_pending;
	struct anx_lv   *error_data;
};

int  anx_ed_session_create(struct anx_ed_session **out);
void anx_ed_session_free(struct anx_ed_session *sess);

/*
 * Read one expression from src (NUL-terminated), eval it in the
 * session's root env, and write a printed representation of the
 * result into out (NUL-terminated, truncated to out_size).
 *
 * If sequence is true, multiple top-level forms are read until end of
 * input; the printed value of the last form is returned.
 */
int anx_ed_eval(struct anx_ed_session *sess, const char *src,
		bool sequence, char *out, uint32_t out_size);

/* Allocate a fresh value of the given tag.  Caller owns one ref. */
struct anx_lv *anx_lv_new(enum anx_lv_tag tag);
struct anx_lv *anx_lv_int(int64_t v);
struct anx_lv *anx_lv_str(const char *s, uint32_t n);
struct anx_lv *anx_lv_sym(const char *s, uint32_t n);
struct anx_lv *anx_lv_cons(struct anx_lv *car, struct anx_lv *cdr);
struct anx_lv *anx_lv_t(void);		/* T constant (refcount-managed) */
struct anx_lv *anx_lv_nil(void);	/* nil constant */

void anx_lv_retain(struct anx_lv *v);
void anx_lv_release(struct anx_lv *v);

/* ------------------------------------------------------------------ */
/* Editor cell dispatch                                                */
/* ------------------------------------------------------------------ */

/*
 * Intents (v1):
 *   editor-eval        in[0] = source-text BYTE_DATA OID
 *                      in[1] = elisp-form BYTE_DATA OID
 *                      out   = result BYTE_DATA OID (printed final value
 *                              or final buffer contents if last form
 *                              evaluated to a buffer handle)
 */
int anx_ed_cell_dispatch(const char *intent,
			 const anx_oid_t *in_oids, uint32_t in_count,
			 anx_oid_t *out_oid_out);

/* ------------------------------------------------------------------ */
/* Interactive editor (UI takeover of the terminal surface)            */
/* ------------------------------------------------------------------ */

#define ANX_ED_NS_MAX		32
#define ANX_ED_PATH_MAX		128
#define ANX_ED_MINIBUF_MAX	256

/*
 * Open the editor on a state object identified by [ns_name]:[path].
 * Resolves via anx_ns_resolve(); if the path doesn't exist yet, the
 * editor opens an empty buffer and creates+binds the object on first
 * save.  Default ns_name is "posix" — matches what external programs
 * use to read/write the same bytes as a file.
 *
 * Takes over the terminal surface; subsequent key events are routed
 * through anx_ed_key_event() until anx_ed_close() is called.
 */
int  anx_ed_open_path(const char *ns_name, const char *path);

/* True when the editor has the terminal surface. */
bool anx_ed_active(void);

/* Editor mainline hooks called by the WM terminal. */
void anx_ed_key_event(uint32_t key, uint32_t mods, uint32_t unicode);
void anx_ed_paint(uint32_t *pixels, uint32_t width, uint32_t height);

/* Release the surface and free the buffer.  Safe if not active. */
void anx_ed_close(void);

/* C-x C-s — serialize the buffer and write a new SO version.
 * Returns ANX_OK on success.  Available for tests; key dispatch
 * routes through this same path. */
int  anx_ed_save(void);

/*
 * Editing primitives (used by both the keymap and eLISP bindings).
 * They operate on the currently-active editor buffer.
 *
 * Returns ANX_OK or ANX_ENOENT when no editor is active.
 */
int  anx_ed_cmd_forward_char(void);
int  anx_ed_cmd_backward_char(void);
int  anx_ed_cmd_next_line(void);
int  anx_ed_cmd_previous_line(void);
int  anx_ed_cmd_beginning_of_line(void);
int  anx_ed_cmd_end_of_line(void);
int  anx_ed_cmd_beginning_of_buffer(void);
int  anx_ed_cmd_end_of_buffer(void);
int  anx_ed_cmd_insert_char(uint32_t codepoint);
int  anx_ed_cmd_insert_string(const char *s, uint32_t n);
int  anx_ed_cmd_newline(void);
int  anx_ed_cmd_delete_backward_char(void);
int  anx_ed_cmd_delete_forward_char(void);
int  anx_ed_cmd_kill_line(void);

/* Active editor buffer accessor (NULL if no editor active). */
struct anx_ed_buffer *anx_ed_active_buffer(void);

/* Active buffer name — the [ns]:[path] of the open object, or "" when
 * no buffer is active. */
const char *anx_ed_active_buffer_name(void);

/* Active eLISP session (NULL if no editor active).  Used by the
 * minibuffer M-: eval path and by tests for editing primitives. */
struct anx_ed_session *anx_ed_active_session(void);

/* Reset and request a paint of the terminal surface; called after a
 * command mutates the buffer. */
void anx_ed_request_redraw(void);

/* ------------------------------------------------------------------ */
/* eLISP customization surface                                         */
/* ------------------------------------------------------------------ */

/* Run a hook by name in the active session.  No-op if not active or
 * the hook is unbound. */
int  anx_ed_run_hook(const char *hook_name);

/* Install hook + editing-primitive builtins into a fresh session.
 * Called automatically by anx_ed_session_create(); also exposed for
 * test setup. */
int  anx_ed_session_install_extensions(struct anx_ed_session *sess);

/* Load and evaluate ~/.amacs.el from the POSIX namespace if it
 * exists.  Silently no-ops if the path is missing. */
int  anx_ed_load_init(void);

#endif /* ANX_AMACS_H */
