/*
 * terminal.c — Graphical shell terminal surface.
 *
 * Canvas surface that wraps anx_shell_execute, captures kprintf output,
 * and renders a scrollable text grid. Key events arrive via the WM
 * dispatch loop through surf->on_event.
 *
 * Layout:
 *   ┌──────────────────────────────────────────────────┐
 *   │ ansh                                             │  ← title bar
 *   ├──────────────────────────────────────────────────┤
 *   │ scrollback lines ...                             │
 *   │ ...                                              │
 *   ├──────────────────────────────────────────────────┤
 *   │ anx> command_                                    │  ← input line
 *   └──────────────────────────────────────────────────┘
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
#define TERM_TITLE_H     28	/* title bar height in pixels */
#define TERM_PAD         6	/* inner padding */
#define TERM_INPUT_H     (ANX_FONT_HEIGHT + TERM_PAD * 2 + 2)

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

	/* Scrollback ring: lines[write_idx % TERM_SCROLLBACK] is next write slot */
	char     lines[TERM_SCROLLBACK][TERM_LINE_MAX];
	uint32_t line_total;	/* total lines ever written (not capped) */

	/* Input */
	char     input[TERM_INPUT_MAX];
	uint32_t input_len;

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
	const uint16_t *glyph = anx_font_glyph(ch);
	uint32_t r, c;

	for (r = 0; r < ANX_FONT_HEIGHT; r++) {
		uint32_t py = y + r;

		if (py >= t->pix_h)
			break;
		for (c = 0; c < ANX_FONT_WIDTH; c++) {
			uint32_t px = x + c;

			if (px >= t->pix_w)
				break;
			t->pixels[py * t->pix_w + px] =
				(glyph[r] & (0x800u >> c)) ? fg : bg;
		}
	}
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
	uint32_t slot = t->line_total % TERM_SCROLLBACK;
	uint32_t copy = (len < TERM_LINE_MAX - 1) ? len : TERM_LINE_MAX - 1;

	anx_memcpy(t->lines[slot], line, copy);
	t->lines[slot][copy] = '\0';
	t->line_total++;
}

/* Split a multi-line string and append each line, wrapping at cols. */
static void term_append_text(struct anx_terminal *t, const char *text)
{
	const char *p = text;
	char wrap_buf[TERM_LINE_MAX];

	while (*p) {
		const char *nl = p;
		uint32_t    len;

		while (*nl && *nl != '\n')
			nl++;
		len = (uint32_t)(nl - p);

		/* Word-wrap long lines */
		if (len == 0) {
			term_append_line(t, "", 0);
		} else {
			uint32_t off = 0;

			while (off < len) {
				uint32_t chunk = len - off;

				if (chunk > t->cols)
					chunk = t->cols;
				anx_memcpy(wrap_buf, p + off, chunk);
				wrap_buf[chunk] = '\0';
				term_append_line(t, wrap_buf, chunk);
				off += chunk;
			}
		}

		p = nl;
		if (*p == '\n')
			p++;
	}
}

/* ------------------------------------------------------------------ */
/* Render                                                              */
/* ------------------------------------------------------------------ */

static void term_render(struct anx_terminal *t)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg     = theme->palette.background;
	uint32_t surf_c = theme->palette.surface;
	uint32_t accent = theme->palette.accent;
	uint32_t fg     = theme->palette.text_primary;
	uint32_t fg_dim = theme->palette.text_dim;
	uint32_t text_area_y, text_area_h, input_y;
	uint32_t visible_rows, first_line;
	uint32_t row;
	char     prompt_line[TERM_LINE_MAX + 8];

	/* Fill background */
	term_fill(t, 0, 0, t->pix_w, t->pix_h, bg);

	/* Title bar */
	term_fill(t, 0, 0, t->pix_w, TERM_TITLE_H, surf_c);
	term_fill(t, 0, TERM_TITLE_H - 2, t->pix_w, 2, accent);
	term_str(t, TERM_PAD, (TERM_TITLE_H - ANX_FONT_HEIGHT) / 2,
		 "ansh", fg, surf_c);

	/* Input line area */
	input_y      = t->pix_h - TERM_INPUT_H;
	term_fill(t, 0, input_y, t->pix_w, 1, accent);
	term_fill(t, 0, input_y + 1, t->pix_w, TERM_INPUT_H - 1, surf_c);

	anx_snprintf(prompt_line, sizeof(prompt_line), "anx> %s_", t->input);
	term_str(t, TERM_PAD, input_y + TERM_PAD, prompt_line, fg, surf_c);

	/* Scrollback text area */
	text_area_y  = TERM_TITLE_H;
	text_area_h  = input_y - text_area_y;
	visible_rows = text_area_h / ANX_FONT_HEIGHT;

	/* Determine which lines to show (last visible_rows lines) */
	{
		uint32_t total_vis  = visible_rows;
		uint32_t skip_rows;	/* empty rows above text (bottom-align) */

		if (t->line_total <= total_vis) {
			first_line = 0;
			visible_rows = t->line_total;
			skip_rows = total_vis - visible_rows;
		} else {
			first_line = t->line_total - total_vis;
			skip_rows  = 0;
		}

		for (row = 0; row < visible_rows; row++) {
			uint32_t line_idx = (first_line + row) % TERM_SCROLLBACK;
			uint32_t y = text_area_y + (skip_rows + row) * ANX_FONT_HEIGHT;

			(void)fg_dim;
			term_str(t, TERM_PAD, y, t->lines[line_idx], fg, bg);
		}
	}

	/* Commit canvas to framebuffer */
	if (t->surf->state == ANX_SURF_VISIBLE)
		anx_iface_surface_commit(t->surf);
}

/* ------------------------------------------------------------------ */
/* Execute a command and append output to scrollback                   */
/* ------------------------------------------------------------------ */

static void term_exec(struct anx_terminal *t)
{
	uint32_t n;
	char prompt_echo[TERM_LINE_MAX];

	if (t->input_len == 0)
		return;

	/* Echo the command */
	anx_snprintf(prompt_echo, sizeof(prompt_echo), "anx> %s", t->input);
	term_append_line(t, prompt_echo, anx_strlen(prompt_echo));

	/* Capture command output */
	anx_kprintf_capture_start(t->cmd_buf, TERM_CMD_BUF);
	anx_shell_execute(t->input);
	n = anx_kprintf_capture_stop();

	/* Append output to scrollback */
	if (n > 0)
		term_append_text(t, t->cmd_buf);

	/* Clear input */
	t->input[0]  = '\0';
	t->input_len = 0;
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

		switch (key) {
		case ANX_KEY_ENTER:
			term_exec(t);
			break;

		case ANX_KEY_BACKSPACE:
			if (t->input_len > 0) {
				t->input_len--;
				t->input[t->input_len] = '\0';
			}
			break;

		case ANX_KEY_ESC:
			/* Close the terminal */
			anx_wm_window_close(surf);
			t->surf   = NULL;
			t->active = false;
			return;

		default:
			/* Append printable ASCII */
			if (ucp >= 0x20 && ucp < 0x7F &&
			    t->input_len < TERM_INPUT_MAX - 1) {
				t->input[t->input_len++] = (char)ucp;
				t->input[t->input_len]   = '\0';
			}
			break;
		}

		term_render(t);
	}
}

/* ------------------------------------------------------------------ */
/* Launch                                                              */
/* ------------------------------------------------------------------ */

void anx_wm_launch_terminal(void)
{
	const struct anx_fb_info *fb;
	struct anx_content_node  *cn;
	struct anx_terminal      *t = NULL;
	uint32_t i, w, h, buf_size;

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
	h = fb->height - ANX_WM_MENUBAR_H - 20;

	buf_size  = w * h * 4;
	t->pixels = anx_alloc(buf_size);
	if (!t->pixels)
		return;

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
				     (int32_t)(fb->width / 10),
				     (int32_t)(ANX_WM_MENUBAR_H + 10),
				     w, h, &t->surf) != ANX_OK) {
		anx_free(cn);
		anx_free(t->pixels);
		t->pixels = NULL;
		return;
	}

	t->pix_w      = w;
	t->pix_h      = h;
	t->cols       = (w - TERM_PAD * 2) / ANX_FONT_WIDTH;
	t->rows       = (h - TERM_TITLE_H - TERM_INPUT_H) / ANX_FONT_HEIGHT;
	t->line_total = 0;
	t->input[0]   = '\0';
	t->input_len  = 0;
	t->active     = true;

	/* Register event handler */
	t->surf->on_event = term_on_event;

	/* Initial welcome lines */
	term_append_line(t, "Anunix Shell  (type 'help' for commands)", 41);
	term_append_line(t, "Meta+Q to close  |  Meta+Enter for new terminal", 47);
	term_append_line(t, "", 0);

	anx_iface_surface_map(t->surf);
	anx_wm_window_open(t->surf);
	term_render(t);
	kprintf("[terminal] opened %ux%u cols=%u rows=%u\n",
		w, h, t->cols, t->rows);
}
