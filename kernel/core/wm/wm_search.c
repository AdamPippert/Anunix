/*
 * wm_search.c — Command search overlay (Meta+Space).
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
#include <anx/workflow.h>
#include <anx/workflow_library.h>
#include <anx/shell.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define CS_W		600
#define CS_H		400

#define FONT_W		ANX_FONT_WIDTH
#define FONT_H		ANX_FONT_HEIGHT
#define ROW_H		(FONT_H + 6)

#define TITLE_H		30
#define INPUT_H		28
#define LIST_Y		(TITLE_H + INPUT_H + 4)
#define MAX_RESULTS	12

static const char *const g_builtins[] = {
	"ls", "cat", "cp", "mv", "inspect", "search",
	"meta", "tensor", "model", "workflow", "kickstart",
	"fetch", "netinfo", "sysinfo", "hwd", "wifi",
	"cells", "vm", "display", "theme", "conform",
	NULL
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct {
	struct anx_surface *surf;
	uint32_t           *pixels;
	uint32_t            pos_x;
	uint32_t            pos_y;

	char     query[64];
	uint32_t query_len;

	char     results[MAX_RESULTS][64];
	uint32_t result_count;
	int32_t  selected;
} g_cs;

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                       */
/* ------------------------------------------------------------------ */

static void cs_fill(uint32_t x, uint32_t y,
		    uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row, col;

	for (row = y; row < y + h && row < CS_H; row++)
		for (col = x; col < x + w && col < CS_W; col++)
			g_cs.pixels[row * CS_W + col] = color;
}

static void cs_draw_char(uint32_t x, uint32_t y, char c,
			  uint32_t fg, uint32_t bg)
{
	const uint16_t *glyph = anx_font_glyph(c);
	uint32_t row, col;

	for (row = 0; row < (uint32_t)FONT_H && (y + row) < CS_H; row++) {
		uint16_t bits = glyph[row];

		for (col = 0; col < (uint32_t)FONT_W && (x + col) < CS_W; col++)
			g_cs.pixels[(y + row) * CS_W + (x + col)] =
				(bits & (0x800u >> col)) ? fg : bg;
	}
}

static void cs_draw_str(uint32_t x, uint32_t y, const char *s,
			 uint32_t fg, uint32_t bg)
{
	for (; *s; s++, x += FONT_W) {
		if (x + FONT_W > CS_W)
			break;
		cs_draw_char(x, y, *s, fg, bg);
	}
}

/* ------------------------------------------------------------------ */
/* Fuzzy match (case-insensitive substring)                            */
/* ------------------------------------------------------------------ */

static bool cs_match(const char *name, const char *query)
{
	const char *n, *q;

	if (query[0] == '\0')
		return true;

	for (n = name; *n; n++) {
		const char *nn = n;

		for (q = query; *q; q++, nn++) {
			char nc = *nn;
			char qc = *q;

			if (!*nn)
				goto next;
			if (nc >= 'A' && nc <= 'Z')
				nc += 32;
			if (qc >= 'A' && qc <= 'Z')
				qc += 32;
			if (nc != qc)
				goto next;
		}
		return true;
next:;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Result population                                                   */
/* ------------------------------------------------------------------ */

static void cs_populate(void)
{
	uint32_t i;
	const char *const *b;

	g_cs.result_count = 0;

	for (b = g_builtins; *b && g_cs.result_count < MAX_RESULTS; b++) {
		if (cs_match(*b, g_cs.query)) {
			anx_strlcpy(g_cs.results[g_cs.result_count],
				     *b, 64);
			g_cs.result_count++;
		}
	}

	if (g_cs.result_count < MAX_RESULTS) {
		const char *uris[32];
		uint32_t wf_count = 0;

		anx_wf_lib_list(uris, 32, &wf_count);
		for (i = 0; i < wf_count && g_cs.result_count < MAX_RESULTS; i++) {
			const char *name = uris[i];
			const char *slash = name;

			/* Use the last path component after '/' */
			for (; *name; name++) {
				if (*name == '/')
					slash = name + 1;
			}
			if (cs_match(slash, g_cs.query)) {
				anx_strlcpy(g_cs.results[g_cs.result_count],
					     slash, 64);
				g_cs.result_count++;
			}
		}
	}

	if (g_cs.selected >= (int32_t)g_cs.result_count)
		g_cs.selected = (int32_t)g_cs.result_count - 1;
	if (g_cs.selected < 0 && g_cs.result_count > 0)
		g_cs.selected = 0;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void cs_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg      = theme->palette.surface;
	uint32_t accent  = theme->palette.accent;
	uint32_t text    = theme->palette.text_primary;
	uint32_t dim     = theme->palette.text_dim;
	uint32_t sel_bg  = 0x00103040;
	char     buf[68];
	uint32_t i;

	if (!g_cs.surf || !g_cs.pixels)
		return;

	/* Background */
	cs_fill(0, 0, CS_W, CS_H, bg);

	/* Title bar */
	cs_fill(0, 0, CS_W, TITLE_H, theme->palette.background);
	cs_fill(0, TITLE_H - 1, CS_W, 1, accent);
	cs_draw_str(10, (TITLE_H - FONT_H) / 2, "Command Search", accent, theme->palette.background);

	/* Input area */
	cs_fill(0, TITLE_H, CS_W, INPUT_H, 0x00111111);
	cs_fill(0, TITLE_H + INPUT_H - 1, CS_W, 1, dim);
	{
		uint32_t ql;

		anx_snprintf(buf, sizeof(buf), "> %s", g_cs.query);
		ql = (uint32_t)anx_strlen(buf);
		cs_draw_str(4, TITLE_H + 4, buf, text, 0x00111111);
		cs_fill(4 + ql * FONT_W, TITLE_H + 4, 2, FONT_H, accent);
	}

	/* Result list */
	for (i = 0; i < g_cs.result_count; i++) {
		uint32_t row_y = LIST_Y + i * ROW_H;
		bool sel = ((int32_t)i == g_cs.selected);

		if (sel)
			cs_fill(0, row_y, CS_W, ROW_H, sel_bg);

		cs_draw_str(4, row_y + 3, sel ? "> " : "  ", accent, sel ? sel_bg : bg);
		cs_draw_str(4 + 2 * FONT_W, row_y + 3,
			     g_cs.results[i], sel ? text : dim,
			     sel ? sel_bg : bg);
	}

	/* No results */
	if (g_cs.result_count == 0) {
		cs_draw_str(10, LIST_Y + 8, "no matches", dim, bg);
	}

	/* Help hint */
	{
		uint32_t hint_y = CS_H - FONT_H - 4;
		cs_fill(0, hint_y - 2, CS_W, 1, dim);
		cs_draw_str(4, hint_y,
			     "Up/Down: select   Enter: run   Esc: close",
			     dim, bg);
	}

	anx_iface_surface_commit(g_cs.surf);
}

/* ------------------------------------------------------------------ */
/* Key event routing                                                   */
/* ------------------------------------------------------------------ */

static void cs_execute_selected(void)
{
	char cmd[68];
	uint32_t i;

	if (g_cs.selected < 0 || (uint32_t)g_cs.selected >= g_cs.result_count)
		return;

	anx_strlcpy(cmd, g_cs.results[g_cs.selected], sizeof(cmd));

	anx_wm_window_close(g_cs.surf);
	g_cs.surf = NULL;

	/* Open terminal so output appears there */
	anx_wm_terminal_open();

	/* Type the command into the terminal one character at a time */
	for (i = 0; cmd[i]; i++)
		anx_wm_terminal_key_event(ANX_KEY_NONE, 0, (uint32_t)(uint8_t)cmd[i]);

	/* Submit */
	anx_wm_terminal_key_event(ANX_KEY_ENTER, 0, '\n');
}

void anx_wm_search_key_event(uint32_t key, uint32_t mods, uint32_t unicode)
{
	(void)mods;

	switch (key) {
	case ANX_KEY_ESC:
		anx_wm_window_close(g_cs.surf);
		g_cs.surf = NULL;
		return;

	case ANX_KEY_ENTER:
		cs_execute_selected();
		return;

	case ANX_KEY_UP:
		if (g_cs.selected > 0)
			g_cs.selected--;
		break;

	case ANX_KEY_DOWN:
		if (g_cs.selected + 1 < (int32_t)g_cs.result_count)
			g_cs.selected++;
		break;

	case ANX_KEY_BACKSPACE:
		if (g_cs.query_len > 0) {
			g_cs.query_len--;
			g_cs.query[g_cs.query_len] = '\0';
			cs_populate();
		}
		break;

	default:
		if (unicode >= 0x20 && unicode < 0x7F &&
		    g_cs.query_len < 63) {
			g_cs.query[g_cs.query_len++] = (char)unicode;
			g_cs.query[g_cs.query_len]   = '\0';
			cs_populate();
		}
		break;
	}

	cs_render();
}

struct anx_surface *anx_wm_search_surface(void)
{
	return g_cs.surf;
}

/* ------------------------------------------------------------------ */
/* Launch                                                              */
/* ------------------------------------------------------------------ */

void anx_wm_launch_command_search(void)
{
	struct anx_content_node *cn;
	uint32_t buf_size;
	const struct anx_fb_info *fb;

	if (g_cs.surf) {
		anx_wm_window_focus(g_cs.surf);
		return;
	}

	buf_size   = CS_W * CS_H * 4;
	g_cs.pixels = anx_alloc(buf_size);
	if (!g_cs.pixels)
		return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_cs.pixels);
		g_cs.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_cs.pixels;
	cn->data_len = buf_size;

	/* Centre on screen */
	fb = anx_fb_get_info();
	g_cs.pos_x = fb && fb->available
		     ? (fb->width  - CS_W) / 2 : 100;
	g_cs.pos_y = fb && fb->available
		     ? (fb->height - CS_H) / 3 : 100;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     (int32_t)g_cs.pos_x, (int32_t)g_cs.pos_y,
				     CS_W, CS_H, &g_cs.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_cs.pixels);
		g_cs.pixels = NULL;
		return;
	}

	g_cs.query[0]    = '\0';
	g_cs.query_len   = 0;
	g_cs.selected    = 0;

	cs_populate();

	anx_iface_surface_map(g_cs.surf);
	anx_wm_window_open(g_cs.surf);
	cs_render();
	kprintf("[search] opened\n");
}
