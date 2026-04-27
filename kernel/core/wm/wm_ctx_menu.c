/*
 * wm_ctx_menu.c — Right-click window context menu.
 *
 * Shows a small popup with tile / float / minimize / close actions for the
 * surface that was right-clicked. The menu is a bare canvas surface (no
 * title decoration) that is not tracked in any workspace, similar to the
 * toast notification.
 *
 * Layout (per item):
 *   CTX_ITEM_H = 28px tall: 2px top-pad + 24px font + 2px bottom-pad
 * Total height = CTX_ITEMS * CTX_ITEM_H + 2 (border top/bottom).
 */

#include <anx/types.h>
#include <anx/wm.h>
#include <anx/interface_plane.h>
#include <anx/theme.h>
#include <anx/font.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/fb.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define CTX_W		160
#define CTX_ITEM_H	28
#define CTX_PAD_X	8
#define CTX_ITEMS	5
#define CTX_H		(CTX_ITEMS * CTX_ITEM_H + 2)

/* Window context menu labels (target != NULL) */
static const char *ctx_labels[CTX_ITEMS] = {
	"Tile Left",
	"Tile Right",
	"Float",
	"Minimize",
	"Close",
};

/* Desktop context menu labels (target == NULL) */
static const char * const desk_labels[CTX_ITEMS] = {
	"New Terminal",
	"Theme Toggle",
	"Help (F1)",
	"Object Viewer",
	"Workflows",
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct {
	struct anx_surface *surf;
	uint32_t           *pixels;
	struct anx_surface *target;	/* window the menu is for; NULL = desktop */
	int                 hovered;	/* -1 = none, 0..CTX_ITEMS-1 */
	int32_t             menu_x;
	int32_t             menu_y;
	bool                desktop;	/* true when target == NULL */
} g_ctx;

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void ctx_draw_char(uint32_t x, uint32_t y, char c,
			   uint32_t fg, uint32_t bg)
{
	const uint16_t *glyph = anx_font_glyph(c);
	uint32_t row, col;

	for (row = 0; row < ANX_FONT_HEIGHT && (y + row) < (uint32_t)CTX_H; row++) {
		uint16_t bits = glyph[row];

		for (col = 0; col < ANX_FONT_WIDTH &&
		     (x + col) < (uint32_t)CTX_W; col++)
			g_ctx.pixels[(y + row) * CTX_W + x + col] =
				(bits & (0x800u >> col)) ? fg : bg;
	}
}

static void ctx_draw_str(uint32_t x, uint32_t y, const char *s,
			  uint32_t fg, uint32_t bg)
{
	for (; *s && x + ANX_FONT_WIDTH <= (uint32_t)CTX_W; s++, x += ANX_FONT_WIDTH)
		ctx_draw_char(x, y, *s, fg, bg);
}

static void ctx_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg     = theme->palette.surface;
	uint32_t fg     = theme->palette.text_primary;
	uint32_t accent = theme->palette.accent;
	uint32_t border = theme->palette.border;
	uint32_t i, px;

	/* Background */
	for (px = 0; px < (uint32_t)(CTX_W * CTX_H); px++)
		g_ctx.pixels[px] = bg;

	/* Border: top and bottom rows */
	for (px = 0; px < (uint32_t)CTX_W; px++) {
		g_ctx.pixels[px]                          = border;
		g_ctx.pixels[(CTX_H - 1) * CTX_W + px]   = border;
	}
	/* Border: left and right columns */
	for (px = 0; px < (uint32_t)CTX_H; px++) {
		g_ctx.pixels[px * CTX_W]              = border;
		g_ctx.pixels[px * CTX_W + CTX_W - 1] = border;
	}

	for (i = 0; i < CTX_ITEMS; i++) {
		uint32_t item_y  = 1 + i * CTX_ITEM_H;
		uint32_t text_y  = item_y + (CTX_ITEM_H - ANX_FONT_HEIGHT) / 2;
		uint32_t row, col;
		uint32_t row_bg;

		row_bg = ((int)i == g_ctx.hovered) ? accent : bg;
		uint32_t row_fg = ((int)i == g_ctx.hovered) ? 0xFFFFFF : fg;

		/* Fill item row */
		for (row = 0; row < (uint32_t)CTX_ITEM_H; row++) {
			for (col = 1; col < (uint32_t)(CTX_W - 1); col++)
				g_ctx.pixels[(item_y + row) * CTX_W + col] = row_bg;
		}

		{
			const char *lbl = g_ctx.desktop
					  ? desk_labels[i] : ctx_labels[i];
			ctx_draw_str(CTX_PAD_X, text_y, lbl, row_fg, row_bg);
		}
	}

	anx_iface_surface_commit(g_ctx.surf);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool anx_wm_ctx_menu_active(void)
{
	return g_ctx.surf != NULL;
}

void anx_wm_ctx_menu_close(void)
{
	if (!g_ctx.surf)
		return;
	anx_iface_surface_destroy(g_ctx.surf);
	g_ctx.surf   = NULL;
	if (g_ctx.pixels) {
		anx_free(g_ctx.pixels);
		g_ctx.pixels = NULL;
	}
	g_ctx.target  = NULL;
	g_ctx.hovered = -1;
}

void anx_wm_ctx_menu_open(struct anx_surface *target, int32_t x, int32_t y)
{
	const struct anx_fb_info  *fb;
	struct anx_content_node   *cn;
	uint32_t buf_size;
	int32_t  sx, sy;

	anx_wm_ctx_menu_close();

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return;

	buf_size = (uint32_t)(CTX_W * CTX_H) * 4u;
	g_ctx.pixels = anx_alloc(buf_size);
	if (!g_ctx.pixels)
		return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_ctx.pixels);
		g_ctx.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_ctx.pixels;
	cn->data_len = buf_size;

	/* Clamp position so the menu stays on screen */
	sx = x;
	sy = y;
	if (sx + CTX_W > (int32_t)fb->width)
		sx = (int32_t)fb->width  - CTX_W;
	if (sy + CTX_H > (int32_t)fb->height)
		sy = (int32_t)fb->height - CTX_H;
	if (sx < 0) sx = 0;
	if (sy < 0) sy = 0;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     sx, sy, CTX_W, CTX_H,
				     &g_ctx.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_ctx.pixels);
		g_ctx.pixels = NULL;
		return;
	}

	g_ctx.target  = target;
	g_ctx.hovered = -1;
	g_ctx.menu_x  = sx;
	g_ctx.menu_y  = sy;
	g_ctx.desktop = (target == NULL);

	/* Adjust label 3 based on target minimize state */
	if (!g_ctx.desktop)
		ctx_labels[3] = (target->state == ANX_SURF_MINIMIZED)
				? "Restore" : "Minimize";

	ctx_render();
	anx_iface_surface_map(g_ctx.surf);
	anx_iface_surface_raise(g_ctx.surf);
}

bool anx_wm_ctx_menu_pointer(int32_t x, int32_t y, uint32_t buttons,
			      bool move_only)
{
	int item;
	int32_t rel_x, rel_y;

	if (!g_ctx.surf)
		return false;

	rel_x = x - g_ctx.menu_x;
	rel_y = y - g_ctx.menu_y;

	/* Click outside → close */
	if (!move_only && (buttons == 0) &&
	    (rel_x < 0 || rel_x >= CTX_W || rel_y < 0 || rel_y >= CTX_H)) {
		anx_wm_ctx_menu_close();
		return false;
	}

	/* Determine hovered item */
	if (rel_x >= 0 && rel_x < CTX_W && rel_y >= 1 &&
	    rel_y < CTX_H - 1) {
		item = (rel_y - 1) / CTX_ITEM_H;
		if (item < 0 || item >= CTX_ITEMS)
			item = -1;
	} else {
		item = -1;
	}

	if (item != g_ctx.hovered) {
		g_ctx.hovered = item;
		ctx_render();
	}

	/* Left-click on an item → execute */
	if (!move_only && (buttons & 1) && item >= 0) {
		bool is_desktop = g_ctx.desktop;
		struct anx_surface *t = g_ctx.target;

		anx_wm_ctx_menu_close();

		if (is_desktop) {
			switch (item) {
			case 0: anx_wm_terminal_open();      break;
			case 1: {
				enum anx_theme_mode m = anx_theme_get_mode();
				anx_theme_set_mode(m == ANX_THEME_PRETTY
						   ? ANX_THEME_BORING
						   : ANX_THEME_PRETTY);
				anx_wm_menubar_refresh();
				break;
			}
			case 2: anx_wm_help_toggle();        break;
			case 3: anx_wm_launch_object_viewer();  break;
			case 4: anx_wm_launch_workflow_designer(); break;
			default: break;
			}
		} else if (t) {
			switch (item) {
			case 0: anx_wm_window_tile_left(t);  break;
			case 1: anx_wm_window_tile_right(t); break;
			case 2: anx_wm_window_float(t);      break;
			case 3:
				if (t->state == ANX_SURF_MINIMIZED)
					anx_wm_window_restore(t);
				else
					anx_wm_window_minimize(t);
				break;
			case 4: anx_wm_window_close(t);      break;
			default: break;
			}
		}
		return true;
	}

	/* Right-click anywhere on the menu → close */
	if (!move_only && (buttons & 2)) {
		anx_wm_ctx_menu_close();
		return true;
	}

	return (rel_x >= 0 && rel_x < CTX_W &&
		rel_y >= 0 && rel_y < CTX_H);
}
