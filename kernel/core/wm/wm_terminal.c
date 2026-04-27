/*
 * wm_terminal.c — Terminal surface (shell-in-a-window).
 */

#include <anx/types.h>
#include <anx/wm.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/theme.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/shell.h>

/* ------------------------------------------------------------------ */
/* Layout constants                                                    */
/* ------------------------------------------------------------------ */

#define TERM_W		800
#define TERM_H		500
#define TERM_X		80
#define TERM_Y		60

#define FONT_W		ANX_FONT_WIDTH
#define FONT_H		ANX_FONT_HEIGHT
#define LINE_H		(FONT_H + 4)

#define INPUT_H		28
#define INPUT_Y		(TERM_H - INPUT_H)

#define HIST_LINES	200
#define HIST_COLS	120

#define VISIBLE_LINES	((INPUT_Y - 8) / LINE_H)

#define CAPTURE_SZ	(HIST_COLS * 80)

#define CMD_HIST_COUNT	50

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct {
	struct anx_surface *surf;
	uint32_t           *pixels;

	char     hist[HIST_LINES][HIST_COLS];
	uint32_t hist_count;
	int32_t  scroll_off;

	char     input[HIST_COLS];
	uint32_t input_len;

	char     cmd_hist[CMD_HIST_COUNT][HIST_COLS];
	uint32_t cmd_hist_count;
	int32_t  cmd_hist_pos;		/* -1 = live input */
	char     input_saved[HIST_COLS];
} g_term;

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                       */
/* ------------------------------------------------------------------ */

static void term_fill(uint32_t x, uint32_t y,
		      uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row, col;

	for (row = y; row < y + h && row < TERM_H; row++)
		for (col = x; col < x + w && col < TERM_W; col++)
			g_term.pixels[row * TERM_W + col] = color;
}

static void term_draw_char(uint32_t x, uint32_t y, char c,
			    uint32_t fg, uint32_t bg)
{
	const uint16_t *glyph = anx_font_glyph(c);
	uint32_t row, col;

	for (row = 0; row < (uint32_t)FONT_H && (y + row) < TERM_H; row++) {
		uint16_t bits = glyph[row];

		for (col = 0; col < (uint32_t)FONT_W && (x + col) < TERM_W; col++)
			g_term.pixels[(y + row) * TERM_W + (x + col)] =
				(bits & (0x800u >> col)) ? fg : bg;
	}
}

static void term_draw_str(uint32_t x, uint32_t y, const char *s,
			   uint32_t fg, uint32_t bg)
{
	for (; *s; s++, x += FONT_W) {
		if (x + FONT_W > TERM_W)
			break;
		term_draw_char(x, y, *s, fg, bg);
	}
}

/* ------------------------------------------------------------------ */
/* History helpers                                                     */
/* ------------------------------------------------------------------ */

static void hist_append(const char *s, uint32_t len)
{
	uint32_t slot = g_term.hist_count % HIST_LINES;
	uint32_t copy = len < HIST_COLS - 1 ? len : HIST_COLS - 1;

	anx_memcpy(g_term.hist[slot], s, copy);
	g_term.hist[slot][copy] = '\0';
	g_term.hist_count++;
}

static void hist_append_str(const char *s)
{
	const char *start = s;

	while (*s) {
		if (*s == '\n') {
			hist_append(start, (uint32_t)(s - start));
			start = s + 1;
		}
		s++;
	}
	if (s > start)
		hist_append(start, (uint32_t)(s - start));
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void term_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg     = 0x00050A10;
	uint32_t fg     = theme->palette.text_primary;
	uint32_t accent = theme->palette.accent;
	uint32_t i;
	uint32_t count, scroll, vis, first_line;

	if (!g_term.surf || !g_term.pixels)
		return;

	/* Background */
	term_fill(0, 0, TERM_W, TERM_H, bg);

	/* Separator line */
	term_fill(0, INPUT_Y - 1, TERM_W, 1, accent);

	/* Output history — bottom-anchored */
	count       = g_term.hist_count;
	scroll      = (uint32_t)g_term.scroll_off;
	vis         = VISIBLE_LINES;
	first_line  = (count > vis + scroll) ? count - vis - scroll : 0;

	for (i = 0; i < vis; i++) {
		uint32_t idx  = first_line + i;
		uint32_t slot = idx % HIST_LINES;
		uint32_t row_y = 8 + i * LINE_H;

		if (idx >= count)
			break;
		term_draw_str(4, row_y, g_term.hist[slot], fg, bg);
	}

	/* Input area */
	term_fill(0, INPUT_Y, TERM_W, INPUT_H, 0x00111111);

	{
		char prompt[HIST_COLS + 4];
		uint32_t pl;

		anx_snprintf(prompt, sizeof(prompt), "> %s", g_term.input);
		pl = (uint32_t)anx_strlen(prompt);

		term_draw_str(4, INPUT_Y + 4, prompt, fg, 0x00111111);

		/* Teal cursor block */
		term_fill(4 + pl * FONT_W, INPUT_Y + 4, 2, FONT_H, accent);
	}

	anx_iface_surface_commit(g_term.surf);
}

/* ------------------------------------------------------------------ */
/* Command execution                                                   */
/* ------------------------------------------------------------------ */

static void term_run_command(const char *cmd)
{
	char *capture;
	uint32_t captured;
	char echo[HIST_COLS + 4];

	anx_snprintf(echo, sizeof(echo), "> %s", cmd);
	hist_append_str(echo);

	capture = anx_alloc(CAPTURE_SZ);
	if (!capture) {
		hist_append_str("[out of memory]");
		return;
	}

	anx_kprintf_capture_start(capture, CAPTURE_SZ);
	anx_shell_execute(cmd);
	captured = anx_kprintf_capture_stop();

	if (captured > 0) {
		if (captured < CAPTURE_SZ)
			capture[captured] = '\0';
		else
			capture[CAPTURE_SZ - 1] = '\0';
		hist_append_str(capture);
	}

	anx_free(capture);
	g_term.scroll_off = 0;
}

/* ------------------------------------------------------------------ */
/* Key event routing (called by wm.c key interceptor)                 */
/* ------------------------------------------------------------------ */

void anx_wm_terminal_key_event(uint32_t key, uint32_t mods, uint32_t unicode)
{
	(void)mods;

	switch (key) {
	case ANX_KEY_ESC:
		anx_wm_window_close(g_term.surf);
		g_term.surf = NULL;
		return;

	case ANX_KEY_ENTER:
		if (g_term.input_len > 0) {
			char cmd[HIST_COLS];

			anx_strlcpy(cmd, g_term.input, sizeof(cmd));
			g_term.input[0]  = '\0';
			g_term.input_len = 0;
			/* Push to command history */
			{
				uint32_t slot = g_term.cmd_hist_count % CMD_HIST_COUNT;

				anx_strlcpy(g_term.cmd_hist[slot], cmd, HIST_COLS);
				g_term.cmd_hist_count++;
			}
			g_term.cmd_hist_pos = -1;
			term_run_command(cmd);
		} else {
			hist_append_str("> ");
		}
		break;

	case ANX_KEY_BACKSPACE:
		if (g_term.input_len > 0) {
			g_term.input_len--;
			g_term.input[g_term.input_len] = '\0';
		}
		g_term.cmd_hist_pos = -1;
		break;

	case ANX_KEY_UP:
		{
			uint32_t hist_len = g_term.cmd_hist_count > CMD_HIST_COUNT
					    ? CMD_HIST_COUNT : g_term.cmd_hist_count;
			if (hist_len == 0) break;
			if (g_term.cmd_hist_pos == -1) {
				anx_strlcpy(g_term.input_saved, g_term.input,
					    sizeof(g_term.input_saved));
				g_term.cmd_hist_pos = 0;
			} else if ((uint32_t)(g_term.cmd_hist_pos + 1) < hist_len) {
				g_term.cmd_hist_pos++;
			} else {
				break;
			}
			{
				uint32_t idx = (g_term.cmd_hist_count - 1
						- (uint32_t)g_term.cmd_hist_pos)
					       % CMD_HIST_COUNT;
				anx_strlcpy(g_term.input, g_term.cmd_hist[idx],
					    sizeof(g_term.input));
				g_term.input_len = (uint32_t)anx_strlen(g_term.input);
			}
		}
		break;

	case ANX_KEY_DOWN:
		if (g_term.cmd_hist_pos < 0) break;
		g_term.cmd_hist_pos--;
		if (g_term.cmd_hist_pos < 0) {
			anx_strlcpy(g_term.input, g_term.input_saved,
				    sizeof(g_term.input));
			g_term.input_len = (uint32_t)anx_strlen(g_term.input);
		} else {
			uint32_t idx = (g_term.cmd_hist_count - 1
					- (uint32_t)g_term.cmd_hist_pos)
				       % CMD_HIST_COUNT;
			anx_strlcpy(g_term.input, g_term.cmd_hist[idx],
				    sizeof(g_term.input));
			g_term.input_len = (uint32_t)anx_strlen(g_term.input);
		}
		break;

	case ANX_KEY_PAGEUP:
		{
			uint32_t max_scroll = g_term.hist_count > (uint32_t)VISIBLE_LINES
					      ? g_term.hist_count - VISIBLE_LINES : 0;
			uint32_t step = VISIBLE_LINES / 2;

			if ((uint32_t)g_term.scroll_off + step <= max_scroll)
				g_term.scroll_off += (int32_t)step;
			else
				g_term.scroll_off = (int32_t)max_scroll;
		}
		break;

	case ANX_KEY_PAGEDOWN:
		g_term.scroll_off -= (int32_t)(VISIBLE_LINES / 2);
		if (g_term.scroll_off < 0)
			g_term.scroll_off = 0;
		break;

	default:
		if (unicode >= 0x20 && unicode < 0x7F &&
		    g_term.input_len < HIST_COLS - 1) {
			g_term.cmd_hist_pos = -1;
			g_term.input[g_term.input_len++] = (char)unicode;
			g_term.input[g_term.input_len]   = '\0';
		}
		break;
	}

	term_render();
}

/* ------------------------------------------------------------------ */
/* Launch / focus                                                      */
/* ------------------------------------------------------------------ */

void anx_wm_terminal_open(void)
{
	struct anx_content_node *cn;
	uint32_t buf_size;

	if (g_term.surf) {
		anx_wm_window_focus(g_term.surf);
		return;
	}

	buf_size     = TERM_W * TERM_H * 4;
	g_term.pixels = anx_alloc(buf_size);
	if (!g_term.pixels)
		return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_term.pixels);
		g_term.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_term.pixels;
	cn->data_len = buf_size;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     TERM_X, TERM_Y, TERM_W, TERM_H,
				     &g_term.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_term.pixels);
		g_term.pixels = NULL;
		return;
	}

	g_term.input[0]     = '\0';
	g_term.input_len    = 0;
	g_term.scroll_off   = 0;
	g_term.cmd_hist_pos = -1;

	if (g_term.hist_count == 0)
		hist_append_str("Anunix terminal  -  type 'help' for commands");

	anx_iface_surface_set_title(g_term.surf, "Terminal");
	anx_iface_surface_map(g_term.surf);
	anx_wm_window_open(g_term.surf);
	term_render();
	kprintf("[terminal] opened\n");
}

struct anx_surface *anx_wm_terminal_surface(void)
{
	return g_term.surf;
}

void anx_wm_terminal_paste(const char *text, uint32_t len)
{
	uint32_t i;

	if (!text || !g_term.surf)
		return;

	for (i = 0; i < len && g_term.input_len < HIST_COLS - 1; i++) {
		char c = text[i];

		if (c < 0x20 || c >= 0x7F)
			continue;
		g_term.input[g_term.input_len++] = c;
	}
	g_term.input[g_term.input_len] = '\0';
	term_render();
}
