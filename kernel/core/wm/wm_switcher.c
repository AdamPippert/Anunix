/*
 * wm_switcher.c — Meta+Tab hierarchical app switcher.
 *
 * Three-axis navigation:
 *   Horizontal (←/→): cycle workflow "apps" (grouped by template URI).
 *   Vertical (↓/↑):   drill into invocations of the selected app.
 *   Horizontal (←/→) on non-last-used invocation: slide into menu rail.
 *
 * Activation: Meta+Tab opens or advances the switcher. Meta release or
 * Enter commits the selection. Esc dismisses without action.
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

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define SW_W		700
#define SW_H		200

#define ICON_SZ		80		/* icon box side length */
#define ICON_PAD	12		/* gap between icon boxes */
#define ICON_ROW_Y	10		/* y of icon row within panel */
#define NAME_Y		(ICON_ROW_Y + ICON_SZ + 4)
#define INV_LIST_Y	(NAME_Y + ANX_FONT_HEIGHT + 8)
#define INV_ROW_H	(ANX_FONT_HEIGHT + 4)
#define MAX_INV		6
#define MAX_APPS	8		/* max apps shown in switcher */
#define CORNER_R	6		/* rounded corner cut radius */

#define FONT_W		ANX_FONT_WIDTH
#define FONT_H		ANX_FONT_HEIGHT

/* Menu rail items */
#define MENU_COUNT	5
static const char *const g_menu_labels[MENU_COUNT] = {
	"Obj", "Edit", "View", "Panel", "Help"
};

/* ------------------------------------------------------------------ */
/* State machine                                                       */
/* ------------------------------------------------------------------ */

enum switcher_state { SW_APPS, SW_INVOCATIONS, SW_MENU };

/* One "app" = all live workflow OIDs that share a template URI. */
struct sw_app {
	char		name[64];	/* template display name or wf name */
	char		uri[128];	/* template URI or wf name key */
	anx_oid_t	invocations[MAX_INV];
	uint32_t	inv_count;
	int32_t		last_used_idx;	/* index of most-recently-used invocation */
};

static struct {
	struct anx_surface	*surf;
	uint32_t		*pixels;
	uint32_t		 pos_x;
	uint32_t		 pos_y;

	struct sw_app		 apps[MAX_APPS];
	uint32_t		 app_count;

	int32_t			 app_sel;	/* selected app index */
	int32_t			 inv_sel;	/* selected invocation index */
	int32_t			 menu_sel;	/* selected menu item */

	enum switcher_state	 state;
	bool			 active;

	anx_oid_t		 pre_switch_oid; /* focused surface before switcher opened */
} g_sw;

/* ------------------------------------------------------------------ */
/* Last-used tracking (defined here; referenced from wm.c via header) */
/* ------------------------------------------------------------------ */

#define ANX_WM_MAX_SURFACES	64

struct wm_surface_activity {
	anx_oid_t	surface_oid;
	uint64_t	last_used_ns;
};

static struct wm_surface_activity g_activity[ANX_WM_MAX_SURFACES];
static uint32_t                   g_activity_count;

/* Simple monotonic ns counter (good enough for ordering). */
static uint64_t g_sw_tick;

static uint64_t sw_now(void)
{
	return ++g_sw_tick;
}

void anx_wm_activity_touch(anx_oid_t oid)
{
	uint32_t i;

	for (i = 0; i < g_activity_count; i++) {
		if (g_activity[i].surface_oid.hi == oid.hi &&
		    g_activity[i].surface_oid.lo == oid.lo) {
			g_activity[i].last_used_ns = sw_now();
			return;
		}
	}
	if (g_activity_count < ANX_WM_MAX_SURFACES) {
		g_activity[g_activity_count].surface_oid = oid;
		g_activity[g_activity_count].last_used_ns = sw_now();
		g_activity_count++;
	}
}

static uint64_t sw_last_used(anx_oid_t oid)
{
	uint32_t i;

	for (i = 0; i < g_activity_count; i++) {
		if (g_activity[i].surface_oid.hi == oid.hi &&
		    g_activity[i].surface_oid.lo == oid.lo)
			return g_activity[i].last_used_ns;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                        */
/* ------------------------------------------------------------------ */

static void sw_fill(uint32_t x, uint32_t y,
		    uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row, col;

	for (row = y; row < y + h && row < SW_H; row++)
		for (col = x; col < x + w && col < SW_W; col++)
			g_sw.pixels[row * SW_W + col] = color;
}

static void sw_draw_char(uint32_t x, uint32_t y, char c,
			  uint32_t fg, uint32_t bg)
{
	const uint16_t *glyph = anx_font_glyph(c);
	uint32_t row, col;

	for (row = 0; row < (uint32_t)FONT_H && (y + row) < SW_H; row++) {
		uint16_t bits = glyph[row];

		for (col = 0; col < (uint32_t)FONT_W && (x + col) < SW_W; col++)
			g_sw.pixels[(y + row) * SW_W + (x + col)] =
				(bits & (0x800u >> col)) ? fg : bg;
	}
}

static void sw_draw_str(uint32_t x, uint32_t y, const char *s,
			 uint32_t fg, uint32_t bg)
{
	for (; *s; s++, x += FONT_W) {
		if (x + FONT_W > SW_W)
			break;
		sw_draw_char(x, y, *s, fg, bg);
	}
}

/* Rounded-square icon box: fill rect, cut corners. */
static void sw_draw_icon_box(uint32_t x, uint32_t y, uint32_t sz,
			      uint32_t border_color, uint32_t bg)
{
	uint32_t r = CORNER_R;

	/* Fill main rect */
	sw_fill(x, y, sz, sz, bg);

	/* Cut corners (paint them background = panel bg) */
	sw_fill(x,          y,          r, r, 0x00101820);
	sw_fill(x + sz - r, y,          r, r, 0x00101820);
	sw_fill(x,          y + sz - r, r, r, 0x00101820);
	sw_fill(x + sz - r, y + sz - r, r, r, 0x00101820);

	/* Border lines (top, bottom, left, right), skipping corners */
	sw_fill(x + r,     y,          sz - 2 * r, 2, border_color);
	sw_fill(x + r,     y + sz - 2, sz - 2 * r, 2, border_color);
	sw_fill(x,         y + r,      2, sz - 2 * r, border_color);
	sw_fill(x + sz - 2, y + r,     2, sz - 2 * r, border_color);
}

/* Draw centered text in a box. */
static void sw_center_str(uint32_t box_x, uint32_t box_y,
			   uint32_t box_w, uint32_t box_h,
			   const char *s, uint32_t fg, uint32_t bg)
{
	uint32_t len = (uint32_t)anx_strlen(s);
	uint32_t tw  = len * FONT_W;
	uint32_t tx  = box_x + (tw < box_w ? (box_w - tw) / 2 : 0);
	uint32_t ty  = box_y + (FONT_H < box_h ? (box_h - FONT_H) / 2 : 0);

	sw_draw_str(tx, ty, s, fg, bg);
}

/* ------------------------------------------------------------------ */
/* String helpers                                                       */
/* ------------------------------------------------------------------ */

/* Case-sensitive substring search; returns true if needle found in hay. */
static bool sw_contains(const char *hay, const char *needle)
{
	size_t nl = anx_strlen(needle);

	if (nl == 0)
		return true;
	for (; *hay; hay++) {
		if (anx_strncmp(hay, needle, nl) == 0)
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Icon content per workflow kind                                       */
/* ------------------------------------------------------------------ */

static void sw_draw_icon_content(uint32_t x, uint32_t y, uint32_t sz,
				  const char *uri, const char *name,
				  uint32_t fg, uint32_t bg)
{
	uint32_t cx = x + sz / 2;
	uint32_t cy = y + sz / 2;
	uint32_t i;

	/* Determine icon style from URI keywords */
	bool is_terminal  = (sw_contains(uri, "shell") || sw_contains(uri, "terminal") ||
			     sw_contains(name, "shell") || sw_contains(name, "terminal"));
	bool is_browser   = (sw_contains(uri, "browser") || sw_contains(name, "browser"));
	bool is_designer  = (sw_contains(uri, "workflow") || sw_contains(uri, "designer"));
	bool is_model     = (sw_contains(uri, "model") || sw_contains(uri, "infer") ||
			     sw_contains(uri, "agent") || sw_contains(uri, "rag"));
	bool is_object    = (sw_contains(uri, "object") || sw_contains(uri, "file") ||
			     sw_contains(uri, "state"));

	(void)cx; (void)cy;

	if (is_terminal) {
		/* ">_" centered */
		uint32_t tx = x + (sz - 2 * FONT_W) / 2;
		uint32_t ty = y + (sz - FONT_H) / 2;
		sw_draw_char(tx,           ty, '>', fg, bg);
		sw_draw_char(tx + FONT_W,  ty, '_', fg, bg);
	} else if (is_browser) {
		/* Horizontal lines suggesting a webpage */
		uint32_t lx = x + 8, lw = sz - 16;
		for (i = 0; i < 4; i++)
			sw_fill(lx, y + 18 + i * 12, lw, 2, fg);
	} else if (is_designer) {
		/* Three dots connected by lines (DAG sketch) */
		uint32_t dot_r = 4;
		uint32_t d1x = x + sz / 2, d1y = y + 12;
		uint32_t d2x = x + 14,     d2y = y + sz - 16;
		uint32_t d3x = x + sz - 14, d3y = y + sz - 16;

		sw_fill(d1x - dot_r, d1y - dot_r, dot_r * 2, dot_r * 2, fg);
		sw_fill(d2x - dot_r, d2y - dot_r, dot_r * 2, dot_r * 2, fg);
		sw_fill(d3x - dot_r, d3y - dot_r, dot_r * 2, dot_r * 2, fg);
		sw_fill(d1x, d1y,    2, d2y - d1y, fg);
		sw_fill(d2x, d2y,    d3x - d2x, 2, fg);
	} else if (is_model) {
		/* 3x3 grid of dots */
		uint32_t gx = x + (sz - 32) / 2;
		uint32_t gy = y + (sz - 32) / 2;
		uint32_t r, c;

		for (r = 0; r < 3; r++)
			for (c = 0; c < 3; c++)
				sw_fill(gx + c * 16, gy + r * 16, 4, 4, fg);
	} else if (is_object) {
		/* Horizontal lines like a document */
		uint32_t lx = x + 10, lw = sz - 20;
		for (i = 0; i < 3; i++)
			sw_fill(lx, y + 20 + i * 14, lw, 2, fg);
	} else {
		/* First 2 chars of name, large, centered */
		char two[3] = {name[0] ? name[0] : '?',
			       name[1] ? name[1] : ' ', '\0'};
		uint32_t tx = x + (sz - 2 * FONT_W) / 2;
		uint32_t ty = y + (sz - FONT_H) / 2;

		sw_draw_char(tx, ty, two[0], fg, bg);
		if (two[1] != ' ')
			sw_draw_char(tx + FONT_W, ty, two[1], fg, bg);
	}
}

/* ------------------------------------------------------------------ */
/* App list population                                                  */
/* ------------------------------------------------------------------ */

static void sw_populate(void)
{
	const char *uris[ANX_WF_LIB_MAX];
	uint32_t    wf_count = 0;
	uint32_t    i;

	g_sw.app_count = 0;

	/* Use library template list as the app source */
	anx_wf_lib_list(uris, ANX_WF_LIB_MAX, &wf_count);

	for (i = 0; i < wf_count && g_sw.app_count < MAX_APPS; i++) {
		struct sw_app *a = &g_sw.apps[g_sw.app_count];
		const struct anx_wf_template *tmpl;
		anx_oid_t wf_oids[16];
		uint32_t  n = 0, j;
		uint64_t  best_ts = 0;

		anx_strlcpy(a->uri,  uris[i],  sizeof(a->uri));

		tmpl = anx_wf_lib_lookup(uris[i]);
		if (tmpl && tmpl->display_name[0])
			anx_strlcpy(a->name, tmpl->display_name, sizeof(a->name));
		else {
			/* Last path component after '/' */
			const char *p = uris[i];
			const char *last = p;

			for (; *p; p++)
				if (*p == '/')
					last = p + 1;
			anx_strlcpy(a->name, last, sizeof(a->name));
		}

		/* Find live workflow instances that match this URI */
		anx_wf_list(wf_oids, 16, &n);
		a->inv_count    = 0;
		a->last_used_idx = 0;

		for (j = 0; j < n && a->inv_count < MAX_INV; j++) {
			struct anx_wf_object *wf = anx_wf_object_get(&wf_oids[j]);
			uint64_t ts;

			if (!wf)
				continue;
			/* Accept all live workflows for now; in future match by template */
			a->invocations[a->inv_count] = wf_oids[j];
			ts = sw_last_used(wf_oids[j]);
			if (ts > best_ts) {
				best_ts = ts;
				a->last_used_idx = (int32_t)a->inv_count;
			}
			a->inv_count++;
		}

		g_sw.app_count++;
	}

	/* Ensure selection is valid */
	if (g_sw.app_sel >= (int32_t)g_sw.app_count)
		g_sw.app_sel = (int32_t)g_sw.app_count - 1;
	if (g_sw.app_sel < 0)
		g_sw.app_sel = 0;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                            */
/* ------------------------------------------------------------------ */

static void sw_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t panel_bg  = 0x00101820;
	uint32_t accent    = theme->palette.accent;
	uint32_t text_hi   = theme->palette.text_primary;
	uint32_t text_dim  = theme->palette.text_dim;
	uint32_t i;

	if (!g_sw.surf || !g_sw.pixels)
		return;

	/* Panel background */
	sw_fill(0, 0, SW_W, SW_H, panel_bg);

	/* Border */
	sw_fill(0,        0,        SW_W, 1,    accent);
	sw_fill(0,        SW_H - 1, SW_W, 1,    accent);
	sw_fill(0,        0,        1,    SW_H, accent);
	sw_fill(SW_W - 1, 0,        1,    SW_H, accent);

	/* Icon row */
	for (i = 0; i < g_sw.app_count && i < MAX_APPS; i++) {
		struct sw_app *a    = &g_sw.apps[i];
		bool    sel         = ((int32_t)i == g_sw.app_sel);
		uint32_t icon_x     = (uint32_t)(10 + (int32_t)i * (ICON_SZ + ICON_PAD));
		uint32_t icon_y     = ICON_ROW_Y;
		uint32_t box_bg     = sel ? 0x00182830 : 0x000C1420;
		uint32_t border_col = sel ? accent : text_dim;
		uint32_t fg         = sel ? text_hi : text_dim;

		if (icon_x + ICON_SZ > SW_W)
			break;

		sw_draw_icon_box(icon_x, icon_y, ICON_SZ, border_col, box_bg);
		sw_draw_icon_content(icon_x + 4, icon_y + 4, ICON_SZ - 8,
				     a->uri, a->name, fg, box_bg);

		/* App name below icon */
		{
			uint32_t nl = (uint32_t)anx_strlen(a->name);
			uint32_t tw = nl * FONT_W;
			uint32_t tx = icon_x + (ICON_SZ > tw ? (ICON_SZ - tw) / 2 : 0);

			sw_draw_str(tx, NAME_Y, a->name,
				    sel ? text_hi : text_dim, panel_bg);
		}
	}

	/* Invocation list for selected app */
	if (g_sw.app_count > 0 && g_sw.state != SW_APPS) {
		struct sw_app *a = &g_sw.apps[g_sw.app_sel];

		for (i = 0; i < a->inv_count; i++) {
			bool inv_sel = ((int32_t)i == g_sw.inv_sel &&
					g_sw.state != SW_APPS);
			bool last    = ((int32_t)i == a->last_used_idx);
			uint32_t ry  = INV_LIST_Y + i * INV_ROW_H;
			uint32_t fg  = last ? text_hi : text_dim;
			char     buf[72];
			struct anx_wf_object *wf;

			if (ry + INV_ROW_H > SW_H)
				break;

			wf = anx_wf_object_get(&a->invocations[i]);
			if (wf)
				anx_snprintf(buf, sizeof(buf), "%s%s",
					     last ? "* " : "  ", wf->name);
			else
				anx_snprintf(buf, sizeof(buf), "%s<wf>",
					     last ? "* " : "  ");

			if (inv_sel) {
				sw_fill(0, ry, 3, INV_ROW_H, accent);
				sw_fill(3, ry, SW_W - 3, INV_ROW_H, 0x00101E2A);
			}
			sw_draw_str(8, ry + 2, buf, fg, inv_sel ? 0x00101E2A : panel_bg);
		}
	}

	/* Menu rail when in SW_MENU state */
	if (g_sw.state == SW_MENU) {
		uint32_t rail_y = INV_LIST_Y + (uint32_t)(g_sw.inv_sel >= 0 ? g_sw.inv_sel : 0) * INV_ROW_H;
		uint32_t rail_x = 200;
		uint32_t item_w = 60;

		for (i = 0; i < MENU_COUNT; i++) {
			bool msel    = ((int32_t)i == g_sw.menu_sel);
			uint32_t mx  = rail_x + i * (item_w + 4);
			uint32_t mbg = msel ? accent : 0x00182030;
			uint32_t mfg = msel ? 0x00000000 : text_hi;

			if (mx + item_w > SW_W)
				break;
			sw_fill(mx, rail_y, item_w, INV_ROW_H, mbg);
			sw_center_str(mx, rail_y, item_w, INV_ROW_H,
				      g_menu_labels[i], mfg, mbg);
		}
	}

	anx_iface_surface_commit(g_sw.surf);
}

/* ------------------------------------------------------------------ */
/* State machine actions                                                */
/* ------------------------------------------------------------------ */

static void sw_focus_selected(void)
{
	struct anx_surface *best = NULL;
	uint64_t            best_ts = 0;
	uint32_t            i;
	anx_oid_t           sw_oid;

	/* Switcher surface OID (to exclude it from the search). */
	sw_oid = g_sw.surf ? g_sw.surf->oid : (anx_oid_t){0, 0};

	/*
	 * Walk the activity table to find the most recently active surface
	 * that is not the switcher itself.
	 *
	 * A proper wf→surface mapping would let us find the exact surface for
	 * the selected invocation; for now this is the best we can do without
	 * a structural link between workflow objects and their surfaces.
	 */
	for (i = 0; i < g_activity_count; i++) {
		anx_oid_t         soid = g_activity[i].surface_oid;
		struct anx_surface *s  = NULL;

		if (soid.hi == sw_oid.hi && soid.lo == sw_oid.lo)
			continue;
		if (g_activity[i].last_used_ns <= best_ts)
			continue;
		if (anx_iface_surface_lookup(soid, &s) != ANX_OK || !s)
			continue;
		best    = s;
		best_ts = g_activity[i].last_used_ns;
	}

	if (best) {
		anx_wm_window_focus(best);
		return;
	}

	/* Fall back to whatever was focused before the switcher opened. */
	if (g_sw.pre_switch_oid.hi || g_sw.pre_switch_oid.lo) {
		struct anx_surface *s = NULL;

		if (anx_iface_surface_lookup(g_sw.pre_switch_oid, &s) == ANX_OK
		    && s)
			anx_wm_window_focus(s);
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

bool anx_wm_switcher_active(void)
{
	return g_sw.active;
}

void anx_wm_switcher_open(void)
{
	struct anx_content_node *cn;
	uint32_t buf_size;
	const struct anx_fb_info *fb;

	if (g_sw.active) {
		/* Already open: cycle to next app */
		g_sw.app_sel = (g_sw.app_sel + 1) % (int32_t)(g_sw.app_count > 0 ? g_sw.app_count : 1);
		sw_render();
		return;
	}

	/* Record what was focused before we open so sw_focus_selected() can restore it. */
	g_sw.pre_switch_oid = anx_input_focus_get();

	buf_size    = SW_W * SW_H * 4;
	g_sw.pixels = anx_alloc(buf_size);
	if (!g_sw.pixels)
		return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_sw.pixels);
		g_sw.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_sw.pixels;
	cn->data_len = buf_size;

	fb        = anx_fb_get_info();
	g_sw.pos_x = fb && fb->available ? (fb->width  - SW_W) / 2 : 100;
	g_sw.pos_y = fb && fb->available ? (fb->height * 2 / 3)    : 400;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     (int32_t)g_sw.pos_x, (int32_t)g_sw.pos_y,
				     SW_W, SW_H, &g_sw.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_sw.pixels);
		g_sw.pixels = NULL;
		return;
	}

	g_sw.state    = SW_APPS;
	g_sw.app_sel  = 0;
	g_sw.inv_sel  = -1;
	g_sw.menu_sel = 0;
	g_sw.active   = true;

	sw_populate();

	anx_iface_surface_map(g_sw.surf);
	anx_iface_surface_raise(g_sw.surf);
	sw_render();
	kprintf("[switcher] opened\n");
}

static void sw_close(void)
{
	if (!g_sw.active)
		return;
	anx_wm_window_close(g_sw.surf);
	g_sw.surf   = NULL;
	g_sw.pixels = NULL;
	g_sw.active = false;
}

void anx_wm_switcher_key_event(struct anx_key_event *ev)
{
	uint32_t key = ev->keycode;

	switch (g_sw.state) {
	case SW_APPS:
		if (key == ANX_KEY_RIGHT || key == ANX_KEY_TAB) {
			if (g_sw.app_count > 0)
				g_sw.app_sel = (g_sw.app_sel + 1) %
					       (int32_t)g_sw.app_count;
		} else if (key == ANX_KEY_LEFT) {
			if (g_sw.app_count > 0) {
				g_sw.app_sel--;
				if (g_sw.app_sel < 0)
					g_sw.app_sel = (int32_t)g_sw.app_count - 1;
			}
		} else if (key == ANX_KEY_DOWN) {
			if (g_sw.app_count > 0 &&
			    g_sw.apps[g_sw.app_sel].inv_count > 0) {
				g_sw.state   = SW_INVOCATIONS;
				g_sw.inv_sel = g_sw.apps[g_sw.app_sel].last_used_idx;
			}
		} else if (key == ANX_KEY_ENTER) {
			sw_focus_selected();
			sw_close();
			return;
		} else if (key == ANX_KEY_ESC) {
			sw_close();
			return;
		}
		break;

	case SW_INVOCATIONS:
		if (key == ANX_KEY_DOWN) {
			struct sw_app *a = &g_sw.apps[g_sw.app_sel];

			if (g_sw.inv_sel + 1 < (int32_t)a->inv_count)
				g_sw.inv_sel++;
		} else if (key == ANX_KEY_UP) {
			if (g_sw.inv_sel > 0) {
				g_sw.inv_sel--;
			} else {
				g_sw.state   = SW_APPS;
				g_sw.inv_sel = -1;
			}
		} else if (key == ANX_KEY_RIGHT || key == ANX_KEY_LEFT) {
			struct sw_app *a = &g_sw.apps[g_sw.app_sel];

			/* Only enter menu if not on last-used invocation */
			if (g_sw.inv_sel != a->last_used_idx) {
				g_sw.state    = SW_MENU;
				g_sw.menu_sel = 0;
			}
		} else if (key == ANX_KEY_ENTER) {
			sw_focus_selected();
			sw_close();
			return;
		} else if (key == ANX_KEY_ESC) {
			g_sw.state   = SW_APPS;
			g_sw.inv_sel = -1;
		}
		break;

	case SW_MENU:
		if (key == ANX_KEY_RIGHT) {
			if (g_sw.menu_sel + 1 < MENU_COUNT)
				g_sw.menu_sel++;
		} else if (key == ANX_KEY_LEFT) {
			if (g_sw.menu_sel > 0)
				g_sw.menu_sel--;
		} else if (key == ANX_KEY_ENTER) {
			anx_oid_t inv_oid = {0, 0};

			if (g_sw.app_count > 0 && g_sw.inv_sel >= 0) {
				struct sw_app *a = &g_sw.apps[g_sw.app_sel];

				if ((uint32_t)g_sw.inv_sel < a->inv_count)
					inv_oid = a->invocations[g_sw.inv_sel];
			}
			sw_close();
			anx_wm_app_menu_open((uint32_t)g_sw.menu_sel, inv_oid);
			return;
		} else if (key == ANX_KEY_ESC) {
			g_sw.state = SW_INVOCATIONS;
		}
		break;
	}

	sw_render();
}

void anx_wm_switcher_meta_released(void)
{
	if (!g_sw.active)
		return;
	sw_focus_selected();
	sw_close();
}
