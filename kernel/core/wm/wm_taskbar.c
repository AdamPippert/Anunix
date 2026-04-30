/*
 * wm_taskbar.c — Bottom-of-screen minimized window dock.
 *
 * Shows a button for each minimized window on the active workspace.
 * Clicking a button restores and focuses that window.  When no windows
 * are minimized the bar shows only a thin top-edge separator line.
 *
 * Layout:
 *   [4px margin] [btn0][btn1]...[btnN] [right edge]
 *   Each button: TASKBAR_BTN_W px wide, full height minus top separator.
 */

#include <anx/wm.h>
#include <anx/types.h>
#include <anx/interface_plane.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/theme.h>
#include <anx/alloc.h>
#include <anx/string.h>

#define TASKBAR_BTN_W	120	/* button width including 2px right gap */
#define TASKBAR_BTN_GAP	2
#define TASKBAR_MARGIN	4
#define TASKBAR_MAX_BTN	24	/* hard cap on displayed buttons */

/* Per-button title length at ANX_FONT_WIDTH pixels per char */
#define TASKBAR_TITLE_CHARS  ((TASKBAR_BTN_W - TASKBAR_BTN_GAP - 8) \
			      / ANX_FONT_WIDTH)

static struct anx_surface *g_taskbar;
static uint32_t           *g_taskbar_pixels;
static uint32_t            tb_width;
static uint32_t            tb_height;

/* Hover state: index into current button list, or -1 */
static int32_t g_tb_hover = -1;

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                        */
/* ------------------------------------------------------------------ */

static void tb_fill_rect(uint32_t x, uint32_t y,
			  uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row, col;

	if (!g_taskbar_pixels)
		return;
	for (row = y; row < y + h && row < tb_height; row++) {
		for (col = x; col < x + w && col < tb_width; col++)
			g_taskbar_pixels[row * tb_width + col] = color;
	}
}

static void tb_draw_str(uint32_t x, uint32_t y, const char *s,
			 uint32_t fg, uint32_t bg)
{
	if (g_taskbar_pixels)
		anx_font_blit_str(g_taskbar_pixels, tb_width, tb_height,
				  x, y, s, fg, bg);
}

/* ------------------------------------------------------------------ */
/* Surface creation                                                    */
/* ------------------------------------------------------------------ */

int anx_wm_taskbar_create(void)
{
	const struct anx_fb_info *fb;
	struct anx_content_node  *cn;
	uint32_t buf_size;

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return ANX_ENOENT;

	tb_width  = fb->width;
	tb_height = ANX_WM_TASKBAR_H;
	buf_size  = tb_width * tb_height * 4;

	g_taskbar_pixels = anx_alloc(buf_size);
	if (!g_taskbar_pixels)
		return ANX_ENOMEM;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_taskbar_pixels);
		g_taskbar_pixels = NULL;
		return ANX_ENOMEM;
	}

	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_taskbar_pixels;
	cn->data_len = buf_size;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     0, (int32_t)(fb->height - ANX_WM_TASKBAR_H),
				     tb_width, tb_height,
				     &g_taskbar) != ANX_OK) {
		anx_free(cn);
		anx_free(g_taskbar_pixels);
		g_taskbar_pixels = NULL;
		return ANX_ENOMEM;
	}

	anx_iface_surface_map(g_taskbar);
	anx_iface_surface_raise(g_taskbar);
	anx_wm_taskbar_refresh();
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Redraw                                                              */
/* ------------------------------------------------------------------ */

void anx_wm_taskbar_refresh(void)
{
	const struct anx_theme *theme;
	uint32_t bg, sep, btn_bg, btn_fg, btn_hov;
	anx_oid_t oids[TASKBAR_MAX_BTN];
	uint32_t count, i;
	uint32_t text_y;
	uint32_t bx;

	if (!g_taskbar || !g_taskbar_pixels)
		return;

	theme   = anx_theme_get();
	bg      = theme->palette.surface;
	sep     = theme->palette.accent;
	btn_bg  = theme->palette.background;
	btn_fg  = theme->palette.text_primary;
	btn_hov = theme->palette.accent;

	/* Background */
	tb_fill_rect(0, 0, tb_width, tb_height, bg);

	/* Separator line at top edge */
	tb_fill_rect(0, 0, tb_width, 2, sep);

	count  = anx_wm_minimized_list(oids, TASKBAR_MAX_BTN);
	text_y = (tb_height > ANX_FONT_HEIGHT)
		 ? (tb_height - ANX_FONT_HEIGHT) / 2 + 1   /* +1 for sep */
		 : 2;

	bx = TASKBAR_MARGIN;
	for (i = 0; i < count; i++) {
		struct anx_surface *s = NULL;
		char label[TASKBAR_TITLE_CHARS + 1];
		uint32_t llen, k;
		uint32_t bg_i;

		anx_iface_surface_lookup(oids[i], &s);
		if (!s)
			continue;

		bg_i = (g_tb_hover == (int32_t)i) ? btn_hov : btn_bg;

		/* Button background, with 2px right gap */
		tb_fill_rect(bx, 2, TASKBAR_BTN_W - TASKBAR_BTN_GAP,
			     tb_height - 2, bg_i);

		/* Truncate title */
		llen = (uint32_t)anx_strlen(s->title);
		if (llen > TASKBAR_TITLE_CHARS)
			llen = TASKBAR_TITLE_CHARS;
		for (k = 0; k < llen; k++)
			label[k] = s->title[k];
		label[llen] = '\0';

		tb_draw_str(bx + 4, text_y, label,
			    (g_tb_hover == (int32_t)i) ? bg : btn_fg,
			    bg_i);

		bx += TASKBAR_BTN_W;
		if (bx + TASKBAR_BTN_W > tb_width)
			break;
	}

	if (g_taskbar->state == ANX_SURF_VISIBLE)
		anx_iface_surface_commit(g_taskbar);
}

/* ------------------------------------------------------------------ */
/* Pointer events                                                      */
/* ------------------------------------------------------------------ */

void anx_wm_taskbar_raise(void)
{
	if (g_taskbar)
		anx_iface_surface_raise(g_taskbar);
}

bool anx_wm_taskbar_pointer(int32_t x, int32_t y,
			     uint32_t buttons, bool move_only)
{
	anx_oid_t oids[TASKBAR_MAX_BTN];
	uint32_t count;
	int32_t  btn_idx;
	int32_t  bx;
	bool     left_down = (buttons & 1) != 0;

	if (!g_taskbar || !g_taskbar_pixels)
		return false;

	/* Check if pointer is in the taskbar region */
	if (y < g_taskbar->y ||
	    y >= g_taskbar->y + (int32_t)g_taskbar->height)
		return false;

	count   = anx_wm_minimized_list(oids, TASKBAR_MAX_BTN);
	btn_idx = -1;
	bx      = (int32_t)TASKBAR_MARGIN;

	for (uint32_t i = 0; i < count; i++) {
		if (x >= bx && x < bx + (int32_t)(TASKBAR_BTN_W - TASKBAR_BTN_GAP)) {
			btn_idx = (int32_t)i;
			break;
		}
		bx += (int32_t)TASKBAR_BTN_W;
		if (bx + (int32_t)TASKBAR_BTN_W > (int32_t)tb_width)
			break;
	}

	/* Update hover highlight */
	if (btn_idx != g_tb_hover) {
		g_tb_hover = btn_idx;
		anx_wm_taskbar_refresh();
	}

	if (!move_only && left_down && btn_idx >= 0 &&
	    (uint32_t)btn_idx < count) {
		struct anx_surface *s = NULL;

		anx_iface_surface_lookup(oids[(uint32_t)btn_idx], &s);
		if (s) {
			g_tb_hover = -1;
			anx_wm_window_restore(s);
			anx_wm_taskbar_refresh();
		}
	}

	return true;
}
