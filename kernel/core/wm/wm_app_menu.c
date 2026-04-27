/*
 * wm_app_menu.c — Standard app menu panels (Obj, Edit, View, Panel, Help).
 *
 * A 480x520 surface centred on screen. Dark background, accent title bar.
 * Each panel lists items with left-justified label and right-justified
 * hotkey hint. Up/Down selects, Enter executes, Esc closes.
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

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define AM_W		480
#define AM_H		520
#define TITLE_H		32
#define ITEM_H		32
#define FONT_W		ANX_FONT_WIDTH
#define FONT_H		ANX_FONT_HEIGHT

/* ------------------------------------------------------------------ */
/* Menu definitions                                                    */
/* ------------------------------------------------------------------ */

struct menu_item {
	const char *label;
	const char *hint;	/* hotkey string, right-aligned */
};

static const struct menu_item g_obj_items[] = {
	{ "New Instance",    "Meta+N"       },
	{ "Open State Obj",  "Meta+O"       },
	{ "Save Snapshot",   "Meta+S"       },
	{ "Export",          "Meta+E"       },
	{ "Close",           "Meta+Q"       },
	{ NULL, NULL }
};

static const struct menu_item g_edit_items[] = {
	{ "Undo",            "Meta+Z"       },
	{ "Redo",            "Meta+Shift+Z" },
	{ "Cut",             "Meta+X"       },
	{ "Copy",            "Meta+C"       },
	{ "Paste",           "Meta+V"       },
	{ "Find",            "Meta+F"       },
	{ NULL, NULL }
};

static const struct menu_item g_view_items[] = {
	{ "Fullscreen",      "Meta+F11"     },
	{ "Zoom In",         "Meta+="       },
	{ "Zoom Out",        "Meta+-"       },
	{ "Theme Toggle",    ""             },
	{ "Show Panels",     ""             },
	{ NULL, NULL }
};

static const struct menu_item g_panel_items[] = {
	{ "New Panel",       "Meta+Shift+N" },
	{ "Tile Left",       "Meta+["       },
	{ "Tile Right",      "Meta+]"       },
	{ "Float",           "Meta+Shift+F" },
	{ "Workspace 1-9",   "Meta+1..9"    },
	{ NULL, NULL }
};

static const struct menu_item g_help_items[] = {
	{ "About Anunix",        "" },
	{ "Keyboard Shortcuts",  "" },
	{ "Workflow Docs",       "" },
	{ "Report Issue",        "" },
	{ NULL, NULL }
};

static const struct menu_item *const g_menus[] = {
	g_obj_items,
	g_edit_items,
	g_view_items,
	g_panel_items,
	g_help_items,
};

static const char *const g_menu_titles[] = {
	"Obj", "Edit", "View", "Panel", "Help"
};

#define MENU_COUNT	5

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct {
	struct anx_surface	*surf;
	uint32_t		*pixels;

	uint32_t		 menu_index;
	anx_oid_t		 invocation_oid;

	int32_t			 selected;
	uint32_t		 item_count;
} g_am;

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                        */
/* ------------------------------------------------------------------ */

static void am_fill(uint32_t x, uint32_t y,
		    uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row, col;

	for (row = y; row < y + h && row < AM_H; row++)
		for (col = x; col < x + w && col < AM_W; col++)
			g_am.pixels[row * AM_W + col] = color;
}

static void am_draw_char(uint32_t x, uint32_t y, char c,
			  uint32_t fg, uint32_t bg)
{
	const uint16_t *glyph = anx_font_glyph(c);
	uint32_t row, col;

	for (row = 0; row < (uint32_t)FONT_H && (y + row) < AM_H; row++) {
		uint16_t bits = glyph[row];

		for (col = 0; col < (uint32_t)FONT_W && (x + col) < AM_W; col++)
			g_am.pixels[(y + row) * AM_W + (x + col)] =
				(bits & (0x800u >> col)) ? fg : bg;
	}
}

static void am_draw_str(uint32_t x, uint32_t y, const char *s,
			 uint32_t fg, uint32_t bg)
{
	for (; *s; s++, x += FONT_W) {
		if (x + FONT_W > AM_W)
			break;
		am_draw_char(x, y, *s, fg, bg);
	}
}

/* Right-justify string ending at x_end. */
static void am_draw_str_right(uint32_t x_end, uint32_t y,
			       const char *s, uint32_t fg, uint32_t bg)
{
	uint32_t len = (uint32_t)anx_strlen(s);
	uint32_t tw  = len * FONT_W;

	if (tw > x_end)
		return;
	am_draw_str(x_end - tw, y, s, fg, bg);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                            */
/* ------------------------------------------------------------------ */

static void am_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t panel_bg  = theme->palette.surface;
	uint32_t accent    = theme->palette.accent;
	uint32_t text_hi   = theme->palette.text_primary;
	uint32_t text_dim  = theme->palette.text_dim;
	uint32_t sel_bg    = 0x00103040;
	const struct menu_item *items;
	uint32_t i;

	if (!g_am.surf || !g_am.pixels)
		return;

	/* Background */
	am_fill(0, 0, AM_W, AM_H, panel_bg);

	/* Title bar */
	am_fill(0, 0, AM_W, TITLE_H, accent);
	{
		char title[72];
		const char *mname = (g_am.menu_index < MENU_COUNT)
				    ? g_menu_titles[g_am.menu_index] : "Menu";

		anx_snprintf(title, sizeof(title), "%s", mname);
		am_draw_str(10, (TITLE_H - FONT_H) / 2, title,
			    0x00000000, accent);
	}

	/* Column separator header */
	am_fill(0, TITLE_H, AM_W, 1, text_dim);

	/* Items */
	items = (g_am.menu_index < MENU_COUNT) ? g_menus[g_am.menu_index] : g_obj_items;

	for (i = 0; items[i].label; i++) {
		uint32_t iy  = TITLE_H + 2 + i * ITEM_H;
		bool     sel = ((int32_t)i == g_am.selected);
		uint32_t ibg = sel ? sel_bg : panel_bg;
		uint32_t ifg = sel ? text_hi : text_dim;

		if (iy + ITEM_H > AM_H)
			break;

		if (sel)
			am_fill(0, iy, AM_W, ITEM_H, sel_bg);

		/* Left: label */
		am_draw_str(12, iy + (ITEM_H - FONT_H) / 2,
			    items[i].label, ifg, ibg);

		/* Right: hint */
		if (items[i].hint && items[i].hint[0])
			am_draw_str_right(AM_W - 10,
					  iy + (ITEM_H - FONT_H) / 2,
					  items[i].hint, text_dim, ibg);

		/* Divider */
		am_fill(0, iy + ITEM_H - 1, AM_W, 1, 0x00181828);
	}

	/* Footer hint */
	{
		uint32_t fy = AM_H - FONT_H - 6;

		am_fill(0, fy - 2, AM_W, 1, text_dim);
		am_draw_str(8, fy, "Up/Down: select   Enter: run   Esc: close",
			    text_dim, panel_bg);
	}

	anx_iface_surface_commit(g_am.surf);
}

/* ------------------------------------------------------------------ */
/* Item execution                                                       */
/* ------------------------------------------------------------------ */

static void am_execute(uint32_t menu_idx, uint32_t item_idx)
{
	(void)menu_idx; (void)item_idx;
	/* Stub: future implementation dispatches to the invocation_oid.
	 * E.g. menu_idx==0,item_idx==0 → create new workflow instance.
	 *      menu_idx==2,item_idx==0 → toggle fullscreen.
	 *      menu_idx==2,item_idx==3 → anx_theme_set_mode toggle. */
	kprintf("[app_menu] execute menu=%u item=%u oid=%08llx%08llx\n",
		menu_idx, item_idx,
		(unsigned long long)g_am.invocation_oid.hi,
		(unsigned long long)g_am.invocation_oid.lo);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

bool anx_wm_app_menu_active(void)
{
	return g_am.surf != NULL;
}

void anx_wm_app_menu_open(uint32_t menu_index, anx_oid_t invocation_oid)
{
	struct anx_content_node *cn;
	uint32_t buf_size;
	const struct anx_fb_info *fb;
	const struct menu_item *items;
	uint32_t n;

	if (g_am.surf) {
		/* Already open — close and reopen with new index */
		anx_wm_window_close(g_am.surf);
		g_am.surf   = NULL;
		g_am.pixels = NULL;
	}

	buf_size    = AM_W * AM_H * 4;
	g_am.pixels = anx_alloc(buf_size);
	if (!g_am.pixels)
		return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_am.pixels);
		g_am.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_am.pixels;
	cn->data_len = buf_size;

	fb = anx_fb_get_info();
	{
		int32_t sx = fb && fb->available
			     ? (int32_t)((fb->width  - AM_W) / 2) : 100;
		int32_t sy = fb && fb->available
			     ? (int32_t)((fb->height - AM_H) / 2) : 80;

		if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
					     sx, sy, AM_W, AM_H,
					     &g_am.surf) != ANX_OK) {
			anx_free(cn);
			anx_free(g_am.pixels);
			g_am.pixels = NULL;
			return;
		}
	}

	g_am.menu_index     = menu_index < MENU_COUNT ? menu_index : 0;
	g_am.invocation_oid = invocation_oid;
	g_am.selected       = 0;

	/* Count items */
	items = g_menus[g_am.menu_index];
	for (n = 0; items[n].label; n++)
		;
	g_am.item_count = n;

	anx_iface_surface_map(g_am.surf);
	anx_wm_window_open(g_am.surf);
	am_render();
	kprintf("[app_menu] opened panel %u\n", menu_index);
}

void anx_wm_app_menu_key_event(struct anx_key_event *ev)
{
	uint32_t key = ev->keycode;

	switch (key) {
	case ANX_KEY_ESC:
		anx_wm_window_close(g_am.surf);
		g_am.surf   = NULL;
		g_am.pixels = NULL;
		return;

	case ANX_KEY_UP:
		if (g_am.selected > 0)
			g_am.selected--;
		break;

	case ANX_KEY_DOWN:
		if ((uint32_t)(g_am.selected + 1) < g_am.item_count)
			g_am.selected++;
		break;

	case ANX_KEY_ENTER:
		am_execute(g_am.menu_index, (uint32_t)g_am.selected);
		anx_wm_window_close(g_am.surf);
		g_am.surf   = NULL;
		g_am.pixels = NULL;
		return;

	default:
		return;
	}

	am_render();
}
