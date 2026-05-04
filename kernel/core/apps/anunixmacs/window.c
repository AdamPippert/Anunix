/*
 * window.c — Interactive editor UI for anunixmacs (RFC-0023).
 *
 * Takes over the terminal surface when active.  wm_terminal.c calls
 * anx_ed_paint() to render and anx_ed_key_event() to deliver keys; the
 * editor renders into the surface's pixel buffer using the public font
 * API.  The buffer lives in a gap buffer (buffer.c); save serializes
 * the buffer into a fresh State Object version via anx_so_replace_payload
 * so external POSIX programs see the new bytes immediately.
 */

#include <anx/anunixmacs.h>
#include <anx/types.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/font.h>
#include <anx/input.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/wm.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define FONT_W		ANX_FONT_WIDTH
#define FONT_H		ANX_FONT_HEIGHT
#define LINE_H		(FONT_H + 4)
#define PAD_X		4
#define PAD_Y		2

/* ------------------------------------------------------------------ */
/* Colors                                                              */
/* ------------------------------------------------------------------ */

#define COL_BG		0x00181818u
#define COL_FG		0x00e0e0e0u
#define COL_DIM		0x00808080u
#define COL_MODE_BG	0x00303040u
#define COL_MODE_FG	0x00ffffffu
#define COL_MINI_BG	0x00101418u
#define COL_MINI_FG	0x00cccccccu
#define COL_CUR_BG	0x00ffaa00u
#define COL_CUR_FG	0x00000000u

/* ------------------------------------------------------------------ */
/* Mode                                                                */
/* ------------------------------------------------------------------ */

enum input_mode {
	MODE_EDIT,
	MODE_MINIBUF,	/* M-: eval prompt active */
};

/* ------------------------------------------------------------------ */
/* Editor state (singleton)                                            */
/* ------------------------------------------------------------------ */

static struct {
	bool                   active;
	enum input_mode        mode;
	struct anx_ed_buffer  *buf;
	struct anx_ed_session *sess;

	char                   ns[ANX_ED_NS_MAX];
	char                   path[ANX_ED_PATH_MAX];
	bool                   has_oid;
	anx_oid_t              oid;

	uint32_t               scroll_line;	/* first visible logical line */

	/* C-x prefix state */
	bool                   ctrl_x_pending;

	/* Minibuffer */
	char                   mini[ANX_ED_MINIBUF_MAX];
	uint32_t               mini_len;
	char                   echo[ANX_ED_MINIBUF_MAX];	/* status line */
} g_ed;

/* ------------------------------------------------------------------ */
/* Buffer iteration helpers (gap-aware)                                */
/* ------------------------------------------------------------------ */

static inline char buf_char_at(const struct anx_ed_buffer *b, uint32_t i)
{
	if (i < b->gap_start) return b->data[i];
	return b->data[i + (b->gap_end - b->gap_start)];
}

/* Convert byte offset to (line, col).  Lines are 0-indexed. */
static void offset_to_line_col(const struct anx_ed_buffer *b, uint32_t off,
			       uint32_t *line_out, uint32_t *col_out)
{
	uint32_t len = anx_ed_buf_length(b);
	uint32_t line = 0, col = 0, i;
	if (off > len) off = len;
	for (i = 0; i < off; i++) {
		if (buf_char_at(b, i) == '\n') { line++; col = 0; }
		else col++;
	}
	if (line_out) *line_out = line;
	if (col_out)  *col_out  = col;
}

static uint32_t line_start_offset(const struct anx_ed_buffer *b, uint32_t target_line)
{
	uint32_t len = anx_ed_buf_length(b);
	uint32_t line = 0, i;
	if (target_line == 0) return 0;
	for (i = 0; i < len; i++) {
		if (buf_char_at(b, i) == '\n') {
			line++;
			if (line == target_line) return i + 1;
		}
	}
	return len;
}

static uint32_t line_end_offset(const struct anx_ed_buffer *b, uint32_t off)
{
	uint32_t len = anx_ed_buf_length(b);
	uint32_t i;
	for (i = off; i < len; i++)
		if (buf_char_at(b, i) == '\n') return i;
	return len;
}

/* ------------------------------------------------------------------ */
/* Public state                                                        */
/* ------------------------------------------------------------------ */

bool anx_ed_active(void)                          { return g_ed.active; }
struct anx_ed_buffer  *anx_ed_active_buffer(void) { return g_ed.active ? g_ed.buf  : NULL; }
struct anx_ed_session *anx_ed_active_session(void){ return g_ed.active ? g_ed.sess : NULL; }

const char *anx_ed_active_buffer_name(void)
{
	return g_ed.active ? g_ed.path : "";
}

int anx_ed_run_hook(const char *hook_name)
{
	char form[64];
	char out[64];
	if (!g_ed.active || !g_ed.sess || !hook_name) return ANX_ENOENT;
	anx_snprintf(form, sizeof(form), "(run-hooks '%s)", hook_name);
	return anx_ed_eval(g_ed.sess, form, false, out, sizeof(out));
}

int anx_ed_session_install_extensions(struct anx_ed_session *sess)
{
	/* Extensions are installed by anx_ed_session_create() automatically. */
	(void)sess;
	return ANX_OK;
}

int anx_ed_load_init(void)
{
	anx_oid_t oid;
	struct anx_state_object *obj;
	if (!g_ed.active || !g_ed.sess) return ANX_ENOENT;
	if (anx_ns_resolve("posix", "/.anunixmacs.el", &oid) != ANX_OK)
		return ANX_ENOENT;
	obj = anx_objstore_lookup(&oid);
	if (!obj) return ANX_ENOENT;
	if (obj->payload && obj->payload_size > 0) {
		char out[64];
		char *src = (char *)anx_alloc((uint32_t)obj->payload_size + 1);
		if (src) {
			anx_memcpy(src, obj->payload, obj->payload_size);
			src[obj->payload_size] = '\0';
			anx_ed_eval(g_ed.sess, src, true, out, sizeof(out));
			anx_free(src);
		}
	}
	anx_objstore_release(obj);
	return ANX_OK;
}

void anx_ed_request_redraw(void)
{
	/* The terminal surface paints us via anx_ed_paint(). */
	anx_wm_terminal_redraw();
}

/* ------------------------------------------------------------------ */
/* File I/O                                                            */
/* ------------------------------------------------------------------ */

static int load_object_into_buffer(const anx_oid_t *oid)
{
	struct anx_state_object *obj;
	int rc;
	obj = anx_objstore_lookup(oid);
	if (!obj) return ANX_ENOENT;
	if (obj->payload && obj->payload_size > 0) {
		rc = anx_ed_buf_insert(g_ed.buf, (const char *)obj->payload,
				       (uint32_t)obj->payload_size);
		if (rc != ANX_OK) {
			anx_objstore_release(obj);
			return rc;
		}
	}
	anx_objstore_release(obj);
	g_ed.buf->point = 0;
	g_ed.buf->dirty = false;
	return ANX_OK;
}

static int serialize_buffer(char **out, uint32_t *len_out)
{
	uint32_t len = anx_ed_buf_length(g_ed.buf);
	char *buf = (char *)anx_alloc(len + 1);
	uint32_t w = 0;
	if (!buf) return ANX_ENOMEM;
	if (len > 0) anx_ed_buf_text(g_ed.buf, buf, len + 1, &w);
	else buf[0] = '\0';
	*out = buf;
	*len_out = w;
	return ANX_OK;
}

int anx_ed_save(void)
{
	char    *bytes;
	uint32_t blen;
	int      rc;

	if (!g_ed.active) return ANX_ENOENT;
	rc = serialize_buffer(&bytes, &blen);
	if (rc != ANX_OK) return rc;

	if (g_ed.has_oid) {
		struct anx_object_handle h;
		rc = anx_so_open(&g_ed.oid, ANX_OPEN_WRITE, &h);
		if (rc == ANX_OK) {
			rc = anx_so_replace_payload(&h, bytes, blen);
			anx_so_close(&h);
		}
	} else {
		struct anx_so_create_params p;
		struct anx_state_object    *obj;
		anx_memset(&p, 0, sizeof(p));
		p.object_type  = ANX_OBJ_BYTE_DATA;
		p.payload      = bytes;
		p.payload_size = blen;
		rc = anx_so_create(&p, &obj);
		if (rc == ANX_OK) {
			g_ed.oid     = obj->oid;
			g_ed.has_oid = true;
			anx_ns_bind(g_ed.ns, g_ed.path, &obj->oid);
			anx_objstore_release(obj);
		}
	}
	anx_free(bytes);
	if (rc == ANX_OK) {
		g_ed.buf->dirty = false;
		anx_strlcpy(g_ed.echo, "saved", sizeof(g_ed.echo));
		anx_ed_run_hook("after-save-hook");
	} else {
		anx_strlcpy(g_ed.echo, "save failed", sizeof(g_ed.echo));
	}
	return rc;
}

int anx_ed_open_path(const char *ns_name, const char *path)
{
	int rc;
	if (!path) return ANX_EINVAL;
	if (g_ed.active) anx_ed_close();

	anx_memset(&g_ed, 0, sizeof(g_ed));
	g_ed.mode = MODE_EDIT;
	anx_strlcpy(g_ed.ns,   ns_name && *ns_name ? ns_name : "posix",
		    sizeof(g_ed.ns));
	anx_strlcpy(g_ed.path, path, sizeof(g_ed.path));

	rc = anx_ed_buf_create(&g_ed.buf);
	if (rc != ANX_OK) return rc;
	rc = anx_ed_session_create(&g_ed.sess);
	if (rc != ANX_OK) {
		anx_ed_buf_free(g_ed.buf);
		g_ed.buf = NULL;
		return rc;
	}

	if (anx_ns_resolve(g_ed.ns, g_ed.path, &g_ed.oid) == ANX_OK) {
		g_ed.has_oid = true;
		(void)load_object_into_buffer(&g_ed.oid);
	}

	g_ed.active = true;
	anx_strlcpy(g_ed.echo, "find-file", sizeof(g_ed.echo));

	/* Show the surface and trigger a paint. */
	anx_wm_terminal_open();
	(void)anx_ed_load_init();
	anx_ed_run_hook("find-file-hook");
	anx_ed_request_redraw();
	return ANX_OK;
}

void anx_ed_close(void)
{
	if (!g_ed.active) return;
	if (g_ed.buf)  { anx_ed_buf_free(g_ed.buf);  g_ed.buf  = NULL; }
	if (g_ed.sess) { anx_ed_session_free(g_ed.sess); g_ed.sess = NULL; }
	g_ed.active = false;
	anx_ed_request_redraw();
}

/* ------------------------------------------------------------------ */
/* Editing commands (operate on g_ed.buf)                              */
/* ------------------------------------------------------------------ */

int anx_ed_cmd_forward_char(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	if (!g_ed.active || !b) return ANX_ENOENT;
	if (b->point < anx_ed_buf_length(b)) b->point++;
	return ANX_OK;
}

int anx_ed_cmd_backward_char(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	if (!g_ed.active || !b) return ANX_ENOENT;
	if (b->point > 0) b->point--;
	return ANX_OK;
}

int anx_ed_cmd_beginning_of_line(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	if (!g_ed.active || !b) return ANX_ENOENT;
	while (b->point > 0 && buf_char_at(b, b->point - 1) != '\n')
		b->point--;
	return ANX_OK;
}

int anx_ed_cmd_end_of_line(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	uint32_t len;
	if (!g_ed.active || !b) return ANX_ENOENT;
	len = anx_ed_buf_length(b);
	while (b->point < len && buf_char_at(b, b->point) != '\n')
		b->point++;
	return ANX_OK;
}

int anx_ed_cmd_next_line(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	uint32_t line, col, len;
	if (!g_ed.active || !b) return ANX_ENOENT;
	offset_to_line_col(b, b->point, &line, &col);
	len = anx_ed_buf_length(b);
	/* Advance to start of next line */
	while (b->point < len && buf_char_at(b, b->point) != '\n')
		b->point++;
	if (b->point < len) b->point++;	/* consume \n */
	/* Walk col chars or stop at \n */
	{
		uint32_t i = 0;
		while (i < col && b->point < len &&
		       buf_char_at(b, b->point) != '\n') {
			b->point++; i++;
		}
	}
	(void)line;
	return ANX_OK;
}

int anx_ed_cmd_previous_line(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	uint32_t line, col;
	uint32_t prev_start;
	if (!g_ed.active || !b) return ANX_ENOENT;
	offset_to_line_col(b, b->point, &line, &col);
	if (line == 0) return ANX_OK;
	prev_start = line_start_offset(b, line - 1);
	b->point = prev_start;
	{
		uint32_t i = 0;
		uint32_t len = anx_ed_buf_length(b);
		while (i < col && b->point < len &&
		       buf_char_at(b, b->point) != '\n') {
			b->point++; i++;
		}
	}
	return ANX_OK;
}

int anx_ed_cmd_beginning_of_buffer(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	if (!g_ed.active || !b) return ANX_ENOENT;
	b->point = 0;
	return ANX_OK;
}

int anx_ed_cmd_end_of_buffer(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	if (!g_ed.active || !b) return ANX_ENOENT;
	b->point = anx_ed_buf_length(b);
	return ANX_OK;
}

int anx_ed_cmd_insert_char(uint32_t cp)
{
	char c;
	if (!g_ed.active || !g_ed.buf) return ANX_ENOENT;
	if (cp == 0 || cp > 0x7e) return ANX_EINVAL;	/* ASCII v1 */
	c = (char)cp;
	return anx_ed_buf_insert(g_ed.buf, &c, 1);
}

int anx_ed_cmd_insert_string(const char *s, uint32_t n)
{
	if (!g_ed.active || !g_ed.buf || !s) return ANX_ENOENT;
	return anx_ed_buf_insert(g_ed.buf, s, n);
}

int anx_ed_cmd_newline(void)
{
	const char nl = '\n';
	if (!g_ed.active || !g_ed.buf) return ANX_ENOENT;
	return anx_ed_buf_insert(g_ed.buf, &nl, 1);
}

int anx_ed_cmd_delete_backward_char(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	if (!g_ed.active || !b) return ANX_ENOENT;
	if (b->point == 0) return ANX_OK;
	b->point--;
	return anx_ed_buf_delete(b, 1);
}

int anx_ed_cmd_delete_forward_char(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	if (!g_ed.active || !b) return ANX_ENOENT;
	if (b->point >= anx_ed_buf_length(b)) return ANX_OK;
	return anx_ed_buf_delete(b, 1);
}

int anx_ed_cmd_kill_line(void)
{
	struct anx_ed_buffer *b = g_ed.buf;
	uint32_t end, n;
	if (!g_ed.active || !b) return ANX_ENOENT;
	end = line_end_offset(b, b->point);
	if (end == b->point) {
		/* On EOL: kill the newline itself */
		if (end < anx_ed_buf_length(b)) n = 1;
		else return ANX_OK;
	} else {
		n = end - b->point;
	}
	return anx_ed_buf_delete(b, n);
}

/* ------------------------------------------------------------------ */
/* Drawing helpers (write into a caller-provided pixel buffer)         */
/* ------------------------------------------------------------------ */

static void px_fill(uint32_t *px, uint32_t W, uint32_t H,
		    uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		    uint32_t color)
{
	uint32_t r, c;
	if (x >= W || y >= H) return;
	if (x + w > W) w = W - x;
	if (y + h > H) h = H - y;
	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++)
			px[(y + r) * W + (x + c)] = color;
}

static void px_putc(uint32_t *px, uint32_t W, uint32_t H,
		    uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
	if (x + FONT_W > W || y + FONT_H > H) return;
	anx_font_blit_char(px, W, H, x, y, c, fg, bg);
}

static void px_puts(uint32_t *px, uint32_t W, uint32_t H,
		    uint32_t x, uint32_t y, const char *s,
		    uint32_t fg, uint32_t bg)
{
	while (*s) {
		px_putc(px, W, H, x, y, *s, fg, bg);
		x += FONT_W;
		if (x + FONT_W > W) return;
		s++;
	}
}

/* ------------------------------------------------------------------ */
/* Render                                                              */
/* ------------------------------------------------------------------ */

static void render_modeline(uint32_t *px, uint32_t W, uint32_t H,
			    uint32_t y, uint32_t cur_line, uint32_t cur_col)
{
	char buf[ANX_ED_PATH_MAX + 64];
	bool dirty = g_ed.buf && g_ed.buf->dirty;
	px_fill(px, W, H, 0, y, W, LINE_H, COL_MODE_BG);
	anx_snprintf(buf, sizeof(buf), " -- %s:%s%s  L%u C%u  (anunixmacs)",
		     g_ed.ns, g_ed.path,
		     dirty ? " [+]" : "",
		     cur_line + 1, cur_col + 1);
	px_puts(px, W, H, PAD_X, y + PAD_Y, buf, COL_MODE_FG, COL_MODE_BG);
}

static void render_minibuffer(uint32_t *px, uint32_t W, uint32_t H,
			      uint32_t y)
{
	px_fill(px, W, H, 0, y, W, LINE_H, COL_MINI_BG);
	if (g_ed.mode == MODE_MINIBUF) {
		px_puts(px, W, H, PAD_X, y + PAD_Y, "M-: ",
			COL_MINI_FG, COL_MINI_BG);
		px_puts(px, W, H, PAD_X + 4 * FONT_W, y + PAD_Y,
			g_ed.mini, COL_FG, COL_MINI_BG);
		/* Cursor at end of minibuffer */
		px_fill(px, W, H,
			PAD_X + (4 + g_ed.mini_len) * FONT_W,
			y + PAD_Y, FONT_W, FONT_H, COL_CUR_BG);
	} else if (g_ed.echo[0]) {
		px_puts(px, W, H, PAD_X, y + PAD_Y, g_ed.echo,
			COL_DIM, COL_MINI_BG);
	}
}

void anx_ed_paint(uint32_t *px, uint32_t W, uint32_t H)
{
	uint32_t mode_y, mini_y, text_h, vis_lines;
	uint32_t cur_line, cur_col;
	uint32_t i, sy;

	if (!g_ed.active || !g_ed.buf || !px || W == 0 || H == 0) return;

	/* Layout: text [mode] [minibuffer] */
	if (H < 3 * LINE_H) return;
	mini_y = H - LINE_H;
	mode_y = mini_y - LINE_H;
	text_h = mode_y;
	vis_lines = text_h / LINE_H;

	offset_to_line_col(g_ed.buf, g_ed.buf->point, &cur_line, &cur_col);

	/* Adjust scroll so the cursor line is visible. */
	if (cur_line < g_ed.scroll_line) g_ed.scroll_line = cur_line;
	else if (cur_line >= g_ed.scroll_line + vis_lines)
		g_ed.scroll_line = cur_line - vis_lines + 1;

	/* Fill text area bg */
	px_fill(px, W, H, 0, 0, W, text_h, COL_BG);

	/* Walk buffer drawing visible lines */
	sy = PAD_Y;
	{
		uint32_t off = line_start_offset(g_ed.buf, g_ed.scroll_line);
		uint32_t len = anx_ed_buf_length(g_ed.buf);
		uint32_t row = 0;
		uint32_t col_x = PAD_X;

		i = off;
		while (row < vis_lines) {
			if (i >= len) {
				if (row > 0 || g_ed.scroll_line > 0) {
					px_puts(px, W, H, PAD_X, sy + row * LINE_H,
						"~", COL_DIM, COL_BG);
				}
				row++;
				col_x = PAD_X;
				continue;
			}
			char c = buf_char_at(g_ed.buf, i);
			if (c == '\n') {
				if (i == g_ed.buf->point) {
					px_fill(px, W, H, col_x,
						sy + row * LINE_H,
						FONT_W, FONT_H, COL_CUR_BG);
				}
				row++;
				col_x = PAD_X;
				i++;
				continue;
			}
			if (i == g_ed.buf->point) {
				px_fill(px, W, H, col_x, sy + row * LINE_H,
					FONT_W, FONT_H, COL_CUR_BG);
				if (c >= 0x20 && c < 0x7f)
					px_putc(px, W, H, col_x,
						sy + row * LINE_H, c,
						COL_CUR_FG, COL_CUR_BG);
			} else if (c >= 0x20 && c < 0x7f) {
				px_putc(px, W, H, col_x, sy + row * LINE_H,
					c, COL_FG, COL_BG);
			}
			col_x += FONT_W;
			if (col_x + FONT_W > W) {
				row++;
				col_x = PAD_X;
			}
			i++;
		}

		/* Cursor at end-of-buffer */
		if (g_ed.buf->point >= len) {
			uint32_t l, c0;
			offset_to_line_col(g_ed.buf, len, &l, &c0);
			if (l >= g_ed.scroll_line && l < g_ed.scroll_line + vis_lines) {
				uint32_t r = l - g_ed.scroll_line;
				uint32_t cx = PAD_X + c0 * FONT_W;
				if (cx + FONT_W <= W)
					px_fill(px, W, H, cx, sy + r * LINE_H,
						FONT_W, FONT_H, COL_CUR_BG);
			}
		}
	}

	render_modeline(px, W, H, mode_y, cur_line, cur_col);
	render_minibuffer(px, W, H, mini_y);
}

/* ------------------------------------------------------------------ */
/* Minibuffer eval                                                     */
/* ------------------------------------------------------------------ */

static void minibuf_enter(void)
{
	g_ed.mode = MODE_MINIBUF;
	g_ed.mini[0] = '\0';
	g_ed.mini_len = 0;
	g_ed.echo[0] = '\0';
}

static void minibuf_cancel(void)
{
	g_ed.mode = MODE_EDIT;
	g_ed.mini_len = 0;
	g_ed.mini[0] = '\0';
	anx_strlcpy(g_ed.echo, "quit", sizeof(g_ed.echo));
}

static void minibuf_submit(void)
{
	char out[ANX_ED_MINIBUF_MAX];
	if (!g_ed.sess) { g_ed.mode = MODE_EDIT; return; }
	(void)anx_ed_eval(g_ed.sess, g_ed.mini, true, out, sizeof(out));
	anx_strlcpy(g_ed.echo, out, sizeof(g_ed.echo));
	g_ed.mode = MODE_EDIT;
	g_ed.mini_len = 0;
	g_ed.mini[0] = '\0';
}

static void minibuf_key(uint32_t key, uint32_t mods, uint32_t unicode)
{
	if (key == ANX_KEY_ESC ||
	    ((mods & ANX_MOD_CTRL) && key == ANX_KEY_G)) {
		minibuf_cancel();
		return;
	}
	if (key == ANX_KEY_ENTER) {
		minibuf_submit();
		return;
	}
	if (key == ANX_KEY_BACKSPACE) {
		if (g_ed.mini_len > 0)
			g_ed.mini[--g_ed.mini_len] = '\0';
		return;
	}
	if (unicode >= 0x20 && unicode < 0x7f &&
	    g_ed.mini_len + 1 < sizeof(g_ed.mini)) {
		g_ed.mini[g_ed.mini_len++] = (char)unicode;
		g_ed.mini[g_ed.mini_len] = '\0';
	}
}

/* ------------------------------------------------------------------ */
/* Key dispatch                                                        */
/* ------------------------------------------------------------------ */

static void clear_echo(void)
{
	g_ed.echo[0] = '\0';
}

void anx_ed_key_event(uint32_t key, uint32_t mods, uint32_t unicode)
{
	if (!g_ed.active) return;

	if (g_ed.mode == MODE_MINIBUF) {
		minibuf_key(key, mods, unicode);
		anx_ed_request_redraw();
		return;
	}

	/* C-x prefix: collect next chord. */
	if (g_ed.ctrl_x_pending) {
		g_ed.ctrl_x_pending = false;
		if (mods & ANX_MOD_CTRL) {
			if (key == ANX_KEY_S) { anx_ed_save();
				anx_ed_request_redraw(); return; }
			if (key == ANX_KEY_C) { anx_ed_close(); return; }
		}
		anx_strlcpy(g_ed.echo, "C-x undefined", sizeof(g_ed.echo));
		anx_ed_request_redraw();
		return;
	}

	clear_echo();

	/* M-: opens minibuffer */
	if ((mods & ANX_MOD_ALT) && unicode == ':') {
		minibuf_enter();
		anx_ed_request_redraw();
		return;
	}

	if (mods & ANX_MOD_CTRL) {
		switch (key) {
		case ANX_KEY_X: g_ed.ctrl_x_pending = true; goto done;
		case ANX_KEY_F: anx_ed_cmd_forward_char();        goto done;
		case ANX_KEY_B: anx_ed_cmd_backward_char();       goto done;
		case ANX_KEY_N: anx_ed_cmd_next_line();           goto done;
		case ANX_KEY_P: anx_ed_cmd_previous_line();       goto done;
		case ANX_KEY_A: anx_ed_cmd_beginning_of_line();   goto done;
		case ANX_KEY_E: anx_ed_cmd_end_of_line();         goto done;
		case ANX_KEY_D: anx_ed_cmd_delete_forward_char(); goto done;
		case ANX_KEY_K: anx_ed_cmd_kill_line();           goto done;
		case ANX_KEY_G: clear_echo();                     goto done;
		}
	}

	if (mods & ANX_MOD_ALT) {
		switch (key) {
		case ANX_KEY_V:
			/* M-v: page up */
			{
				uint32_t i;
				for (i = 0; i < 10; i++) anx_ed_cmd_previous_line();
			}
			goto done;
		}
		if (unicode == '<') { anx_ed_cmd_beginning_of_buffer(); goto done; }
		if (unicode == '>') { anx_ed_cmd_end_of_buffer();       goto done; }
	}

	switch (key) {
	case ANX_KEY_LEFT:      anx_ed_cmd_backward_char();        goto done;
	case ANX_KEY_RIGHT:     anx_ed_cmd_forward_char();         goto done;
	case ANX_KEY_UP:        anx_ed_cmd_previous_line();        goto done;
	case ANX_KEY_DOWN:      anx_ed_cmd_next_line();            goto done;
	case ANX_KEY_HOME:      anx_ed_cmd_beginning_of_line();    goto done;
	case ANX_KEY_END:       anx_ed_cmd_end_of_line();          goto done;
	case ANX_KEY_BACKSPACE: anx_ed_cmd_delete_backward_char(); goto done;
	case ANX_KEY_DELETE:    anx_ed_cmd_delete_forward_char();  goto done;
	case ANX_KEY_ENTER:     anx_ed_cmd_newline();              goto done;
	case ANX_KEY_TAB:       anx_ed_cmd_insert_string("\t", 1); goto done;
	case ANX_KEY_PAGEUP:
		{ uint32_t i; for (i = 0; i < 10; i++) anx_ed_cmd_previous_line(); }
		goto done;
	case ANX_KEY_PAGEDOWN:
		{ uint32_t i; for (i = 0; i < 10; i++) anx_ed_cmd_next_line(); }
		goto done;
	}

	/* Printable insert */
	if (unicode >= 0x20 && unicode < 0x7f && !(mods & ANX_MOD_CTRL))
		anx_ed_cmd_insert_char(unicode);

done:
	anx_ed_request_redraw();
}
