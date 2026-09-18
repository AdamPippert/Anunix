/*
 * terminal.c — Graphical shell terminal surface.
 *
 * Canvas surface that wraps anx_shell_execute, captures kprintf output,
 * and renders a scrollable text grid. Key events arrive via the WM
 * dispatch loop through surf->on_event.
 *
 * Output and the editable prompt share one top-down viewport. Long lines
 * reflow on resize; PageUp/PageDown scroll retained output. Up/Down use
 * the common shell history with a draft cursor owned by each terminal.
 *
 * Multiple terminals can be opened — each gets its own static slot.
 */

#include <anx/types.h>
#include <anx/wm.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/gui.h>
#include <anx/theme.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/shell.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define TERM_MAX         4	/* max simultaneous terminals */
#define TERM_SCROLLBACK  200	/* scrollback line slots */
#define TERM_LINE_MAX    128	/* max chars per scrollback line */
#define TERM_INPUT_MAX   256	/* max command input length */
#define TERM_CMD_BUF     8192	/* kprintf capture buffer */
#define TERM_TOP         4	/* space above the scrollback; the WM draws the title bar */
#define TERM_PAD         6	/* inner padding */

/* ------------------------------------------------------------------ */
/* Per-terminal state                                                  */
/* ------------------------------------------------------------------ */

struct anx_terminal {
	struct anx_surface *surf;
	uint32_t           *pixels;
	uint32_t            pix_w;
	uint32_t            pix_h;

	/* Text grid dimensions (derived from surface size) */
	uint32_t            cols;
	uint32_t            rows;

	/* Bounded logical lines; wrapping is computed at draw time. */
	char     lines[TERM_SCROLLBACK][TERM_LINE_MAX];
	uint32_t line_count, line_head;
	uint32_t scroll_rows;

	/* Input */
	char     input[TERM_INPUT_MAX];
	uint32_t input_len;
	struct anx_shell_history_cursor recall;

	/* Command output capture */
	char     cmd_buf[TERM_CMD_BUF];

	bool     active;
};

static struct anx_terminal g_terms[TERM_MAX];

/* ------------------------------------------------------------------ */
/* Pixel drawing into surface canvas                                   */
/* ------------------------------------------------------------------ */

static void term_fill(struct anx_terminal *t, uint32_t x, uint32_t y,
		       uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t r, c;

	for (r = y; r < y + h && r < t->pix_h; r++)
		for (c = x; c < x + w && c < t->pix_w; c++)
			t->pixels[r * t->pix_w + c] = color;
}

static void term_char(struct anx_terminal *t, uint32_t x, uint32_t y,
		       char ch, uint32_t fg, uint32_t bg)
{
	anx_font_blit_char(t->pixels, t->pix_w, t->pix_h, x, y, ch, fg, bg);
}

static void term_str(struct anx_terminal *t, uint32_t x, uint32_t y,
		      const char *s, uint32_t fg, uint32_t bg)
{
	for (; *s && x + ANX_FONT_WIDTH <= t->pix_w; s++, x += ANX_FONT_WIDTH)
		term_char(t, x, y, *s, fg, bg);
}

/* ------------------------------------------------------------------ */
/* Scrollback helpers                                                  */
/* ------------------------------------------------------------------ */

static void term_append_line(struct anx_terminal *t, const char *line,
			       uint32_t len)
{
	uint32_t slot = (t->line_head + t->line_count) % TERM_SCROLLBACK;

	anx_memcpy(t->lines[slot], line, len);
	t->lines[slot][len] = '\0';
	if (t->line_count < TERM_SCROLLBACK)
		t->line_count++;
	else
		t->line_head = (t->line_head + 1) % TERM_SCROLLBACK;
}

static void term_append_text(struct anx_terminal *t, const char *text)
{
	while (*text) {
		uint32_t len = 0;

		while (text[len] && text[len] != '\n' && len < TERM_LINE_MAX - 1)
			len++;
		term_append_line(t, text, len);
		text += len;
		if (*text == '\n') text++;
	}
}

static void term_set_grid(struct anx_terminal *t)
{
	t->cols = t->pix_w >= TERM_PAD * 2 + ANX_FONT_WIDTH
		? (t->pix_w - TERM_PAD * 2) / ANX_FONT_WIDTH : 1;
	t->rows = t->pix_h >= TERM_TOP + TERM_PAD + ANX_FONT_HEIGHT
		? (t->pix_h - TERM_TOP - TERM_PAD) / ANX_FONT_HEIGHT : 1;
}

/* Draw wrapped rows that intersect the current viewport. */
static void term_draw_rows(struct anx_terminal *t, const char *text,
			   uint32_t first, uint32_t *row, uint32_t fg, uint32_t bg)
{
	uint32_t len = (uint32_t)anx_strlen(text), pos = 0;

	do {
		uint32_t n = len - pos;
		char line[TERM_INPUT_MAX + 5];

		if (n > t->cols) n = t->cols;
		if (*row >= first && *row - first < t->rows) {
			anx_memcpy(line, text + pos, n);
			line[n] = '\0';
			term_str(t, TERM_PAD, TERM_TOP + (*row - first) * ANX_FONT_HEIGHT,
				 line, fg, bg);
		}
		(*row)++;
		pos += n;
	} while (pos < len);
}

/* ------------------------------------------------------------------ */
/* Render                                                              */
/* ------------------------------------------------------------------ */

static void term_render(struct anx_terminal *t)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg = theme->palette.background;
	uint32_t fg = theme->palette.text_primary;
	uint32_t i, output = 0, first, row = 0, cursor_row;
	char prompt[TERM_INPUT_MAX + 5];

	if (!t->active || !t->surf || !t->pixels)
		return;
	term_fill(t, 0, 0, t->pix_w, t->pix_h, bg);
	for (i = 0; i < t->line_count; i++) {
		uint32_t slot = (t->line_head + i) % TERM_SCROLLBACK;
		uint32_t len = (uint32_t)anx_strlen(t->lines[slot]);

		output += len ? (len + t->cols - 1) / t->cols : 1;
	}
	cursor_row = output + (t->input_len + 5) / t->cols;
	first = cursor_row + 1 > t->rows ? cursor_row + 1 - t->rows : 0;
	if (t->scroll_rows > first) t->scroll_rows = first;
	first -= t->scroll_rows;
	for (i = 0; i < t->line_count; i++) {
		uint32_t slot = (t->line_head + i) % TERM_SCROLLBACK;

		term_draw_rows(t, t->lines[slot], first, &row, fg, bg);
	}
	anx_snprintf(prompt, sizeof(prompt), "anx> %s", t->input);
	term_draw_rows(t, prompt, first, &row, fg, bg);
	if (cursor_row >= first && cursor_row - first < t->rows)
		term_fill(t, TERM_PAD + ((t->input_len + 5) % t->cols) * ANX_FONT_WIDTH,
			  TERM_TOP + (cursor_row - first) * ANX_FONT_HEIGHT,
			  2, ANX_FONT_HEIGHT, theme->palette.accent);
	if (t->surf->state == ANX_SURF_VISIBLE)
		anx_iface_surface_commit(t->surf);
}

/* ------------------------------------------------------------------ */
/* Execute a command and append output to scrollback                   */
/* ------------------------------------------------------------------ */

static void term_exec(struct anx_terminal *t)
{
	uint32_t n;
	char command[TERM_INPUT_MAX], echo[TERM_INPUT_MAX + 5];
	struct anx_capture_state saved;

	anx_strlcpy(command, t->input, sizeof(command));
	t->input[0] = '\0';
	t->input_len = 0;
	t->scroll_rows = 0;
	anx_shell_history_reset(&t->recall);
	anx_shell_history_record(command);
	if (!anx_strcmp(command, "clear")) {
		t->line_count = 0;
		t->line_head = 0;
		return;
	}
	if (!anx_strcmp(command, "exit") || !anx_strcmp(command, "quit")) {
		anx_wm_window_close(t->surf);
		return;
	}
	anx_snprintf(echo, sizeof(echo), "anx> %s", command);
	term_append_text(t, echo);
	if (!command[0])
		return;
	anx_kprintf_capture_save(&saved);
	anx_kprintf_capture_start(t->cmd_buf, TERM_CMD_BUF);
	anx_shell_execute(command);
	n = anx_kprintf_capture_stop();
	anx_kprintf_capture_restore(&saved);
	if (n > 0)
		term_append_text(t, t->cmd_buf);
}

/* ------------------------------------------------------------------ */
/* Key event handler (called by WM dispatch loop)                      */
/* ------------------------------------------------------------------ */

static void term_on_event(struct anx_surface *surf,
			   const struct anx_event *ev)
{
	struct anx_terminal *t = NULL;
	uint32_t i;

	/* Find which terminal owns this surface */
	for (i = 0; i < TERM_MAX; i++) {
		if (g_terms[i].active && g_terms[i].surf == surf) {
			t = &g_terms[i];
			break;
		}
	}
	if (!t)
		return;

	if (ev->type == ANX_EVENT_KEY_DOWN) {
		uint32_t key  = ev->data.key.keycode;
		uint32_t mods = ev->data.key.modifiers;
		uint32_t ucp  = ev->data.key.unicode;

		(void)mods;
		if (key != ANX_KEY_PAGEUP && key != ANX_KEY_PAGEDOWN)
			t->scroll_rows = 0;

		switch (key) {
		case ANX_KEY_ENTER:
			term_exec(t);
			break;

		case ANX_KEY_BACKSPACE:
			if (t->input_len > 0) {
				t->input_len--;
				t->input[t->input_len] = '\0';
			}
			anx_shell_history_reset(&t->recall);
			break;

		case ANX_KEY_UP:
		case ANX_KEY_DOWN: {
			int n = anx_shell_history_move(&t->recall,
				key == ANX_KEY_UP ? -1 : 1, t->input, sizeof(t->input));

			if (n >= 0) t->input_len = (uint32_t)n;
			break;
		}
		case ANX_KEY_PAGEUP:
			t->scroll_rows += t->rows;
			break;
		case ANX_KEY_PAGEDOWN:
			t->scroll_rows = t->scroll_rows > t->rows ? t->scroll_rows - t->rows : 0;
			break;

		case ANX_KEY_ESC:
			/* Close the terminal */
			anx_wm_window_close(surf);
			return;

		default:
			/* Append printable ASCII */
			if (ucp >= 0x20 && ucp < 0x7F &&
			    t->input_len < TERM_INPUT_MAX - 1) {
				t->input[t->input_len++] = (char)ucp;
				t->input[t->input_len]   = '\0';
				anx_shell_history_reset(&t->recall);
			}
			break;
		}

		term_render(t);
	}
}

/* ------------------------------------------------------------------ */
/* Launch                                                              */
/* ------------------------------------------------------------------ */

/* Free the slot however the window went away (Esc, Meta+Q, close button). */
static void term_on_destroy(struct anx_surface *surf)
{
	uint32_t i;

	anx_wm_canvas_free(surf);
	for (i = 0; i < TERM_MAX; i++) {
		if (g_terms[i].surf == surf) {
			g_terms[i].surf   = NULL;
			g_terms[i].pixels = NULL;
			g_terms[i].active = false;
		}
	}
}

/* The WM tiled or resized the window: redraw at the new size. */
static void term_on_resize(struct anx_surface *surf)
{
	uint32_t i, *px;

	for (i = 0; i < TERM_MAX; i++) {
		struct anx_terminal *t = &g_terms[i];

		if (!t->active || t->surf != surf)
			continue;
		px = anx_wm_canvas_realloc(surf, surf->width, surf->height);
		if (!px)
			return;	/* old buffer stays, shown unscaled */
		t->pixels = px;
		t->pix_w  = surf->width;
		t->pix_h  = surf->height;
		term_set_grid(t);
		term_render(t);
		return;
	}
}

void anx_wm_native_terminals_redraw(void)
{
	uint32_t i;

	for (i = 0; i < TERM_MAX; i++)
		if (g_terms[i].active)
			term_render(&g_terms[i]);
}

void anx_wm_launch_terminal(void)
{
	const struct anx_fb_info *fb;
	struct anx_content_node  *cn;
	struct anx_terminal      *t = NULL;
	uint32_t i, w, h, buf_size, top, bottom;

	fb = anx_fb_get_info();
	if (!fb || !fb->available) {
		kprintf("[terminal] no framebuffer\n");
		return;
	}

	/* Find a free terminal slot */
	for (i = 0; i < TERM_MAX; i++) {
		if (!g_terms[i].active) {
			t = &g_terms[i];
			break;
		}
	}
	if (!t) {
		kprintf("[terminal] max terminals open\n");
		return;
	}

	/* Size: most of the screen below the menubar */
	w = fb->width  * 4 / 5;
	top = ANX_WM_MENUBAR_H + ANX_WM_DECOR_H + anx_wm_tiling.border_w + 10;
	bottom = ANX_WM_TASKBAR_H + anx_wm_tiling.border_w + 10;
	h = fb->height > top + bottom ? fb->height - top - bottom : fb->height;
	anx_wm_window_fit(&w, &h);

	buf_size  = w * h * 4;
	t->pixels = anx_alloc(buf_size);
	if (!t->pixels) {
		/* Say so: a silent return looked like a dead Meta+Enter. */
		kprintf("[terminal] no memory for %ux%u window\n", w, h);
		anx_wm_notify("Terminal: not enough memory");
		return;
	}

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(t->pixels);
		t->pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = t->pixels;
	cn->data_len = buf_size;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     (int32_t)((fb->width - w) / 2),
				     (int32_t)top,
				     w, h, &t->surf) != ANX_OK) {
		anx_free(cn);
		anx_free(t->pixels);
		t->pixels = NULL;
		return;
	}

	t->pix_w      = w;
	t->pix_h      = h;
	term_set_grid(t);
	t->line_count = 0;
	t->line_head = 0;
	t->scroll_rows = 0;
	anx_shell_history_reset(&t->recall);
	t->input[0]   = '\0';
	t->input_len  = 0;
	t->active     = true;

	/* Register event handler */
	t->surf->on_event   = term_on_event;
	t->surf->on_destroy = term_on_destroy;
	t->surf->on_resize  = term_on_resize;
	/* A title gives it the WM's title bar and window buttons. */
	anx_iface_surface_set_title(t->surf, "ansh");

	/* Initial welcome lines */
	term_append_text(t, "Anunix Shell (type 'help' for commands)");
	term_append_text(t, "Up/Down: history  |  PgUp/PgDn: scroll");
	term_append_line(t, "", 0);

	term_render(t);
	if (anx_iface_surface_map(t->surf) != ANX_OK ||
	    anx_wm_window_open(t->surf) != ANX_OK) {
		anx_iface_surface_destroy(t->surf);
		anx_wm_notify("Terminal: cannot open window");
		return;
	}
	term_render(t);
	kprintf("[terminal] opened %ux%u cols=%u rows=%u\n",
		w, h, t->cols, t->rows);
}
