/*
 * wm.c — Anunix window manager: workspace management, focus, desktop loop.
 *
 * The WM sits between the input subsystem and the compositor:
 *   - Intercepts global hotkeys (Meta+key) before forwarding to surfaces
 *   - Manages 9 virtual workspaces; switching hides/shows surface groups
 *   - Tracks focused surface per workspace; routes keyboard focus
 *   - Drives the desktop event loop (replaces anx_shell_run on FB hardware)
 *
 * All state is statically allocated — no heap use in the WM core.
 */

#include <anx/wm.h>
#include <anx/types.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/fb.h>
#include <anx/fbcon.h>
#include <anx/gui.h>
#include <anx/font.h>
#include <anx/theme.h>
#include <anx/window_chrome.h>
#include <anx/color_editor.h>
#include <anx/tools.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/spinlock.h>
#include <anx/arch.h>
#include <anx/mt7925.h>
#include <anx/e1000.h>
#include <anx/httpd.h>
#include <anx/sshd.h>
#include <anx/xhci.h>
#include <anx/i2c_input.h>
#include <anx/net.h>
#include <anx/browser_cell.h>

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static struct anx_wm_workspace g_workspaces[ANX_WM_WORKSPACES];
static uint32_t                g_active_ws;	/* 0-based index */
struct anx_surface            *g_menubar;	/* always-on-top bar (used by wm_menubar.c) */
uint32_t                      *g_menubar_pixels;
static bool                    g_wm_running;
static struct anx_spinlock     g_wm_lock;
static uint32_t                g_wm_tick;	/* incremented every event loop iteration */

/* Saved pre-fullscreen bounds for a surface */
static struct {
	struct anx_surface *surf;
	int32_t  x, y;
	uint32_t w, h;
	bool     active;
} g_fs_saved;

/* Drag-to-move state */
#define SNAP_ZONE  40   /* px from screen edge that triggers edge-snap */
static struct {
	struct anx_surface *surf;
	int32_t             off_x;  /* cursor x minus surface x at drag start */
	int32_t             off_y;
	bool                active;
	int                 snap;   /* 0=none, 1=tile_left, 2=tile_right */
} g_drag;

/* Double-click detection on titlebar */
#define DBLCLICK_WINDOW  8   /* event-loop polls; ~133ms @ 60Hz */
static struct {
	struct anx_surface *surf;
	uint32_t            tick;  /* g_wm_tick at first click */
} g_dblclick;

#define RESIZE_EDGE    6u   /* px from edge that counts as resize zone */
#define RESIZE_MIN_W  80u
#define RESIZE_MIN_H  40u

/* Drag-to-resize state */
static struct {
	struct anx_surface *surf;
	int32_t  start_x, start_y;   /* cursor pos at drag start */
	uint32_t orig_w,  orig_h;    /* surface dims at drag start */
	int32_t  orig_rx, orig_ry;   /* surface right/bottom edge at drag start */
	uint8_t  edges;              /* bitmask: 1=right, 2=bottom */
	bool     active;
} g_resize;

/* Saved pre-tile bounds (for Float restore) */
static struct {
	struct anx_surface *surf;
	int32_t  x, y;
	uint32_t w, h;
	bool     active;
} g_tile_saved;

/* ------------------------------------------------------------------ */
/* Toast notification                                                  */
/* ------------------------------------------------------------------ */

#define TOAST_W		400u
#define TOAST_H		32u
#define TOAST_LIFE	180u	/* poll cycles before auto-dismiss (~3s) */

static struct {
	struct anx_surface *surf;
	anx_oid_t           oid;	/* to tell a live toast from a reused slot */
	uint32_t           *pixels;
	uint32_t            age;	/* polls since creation */
} g_toast;

static void toast_dismiss(void)
{
	struct anx_surface *live = NULL;

	if (!g_toast.surf)
		return;
	/*
	 * Only destroy the toast if its slot still holds it; a surface pool
	 * reset (the host tests re-initialise the interface plane) would
	 * otherwise send us into a freed slot.
	 */
	if (anx_iface_surface_lookup(g_toast.oid, &live) == ANX_OK &&
	    live == g_toast.surf) {
		/* Each notification allocates a fresh buffer; free the last one. */
		anx_wm_canvas_free(g_toast.surf);
		anx_wm_overlay_destroy(g_toast.surf);
	}
	g_toast.surf   = NULL;
	g_toast.pixels = NULL;
	g_toast.age    = 0;
}

void anx_wm_notify(const char *msg)
{
	const struct anx_fb_info *fb;
	const struct anx_theme   *theme;
	struct anx_content_node  *cn;
	uint32_t buf_size, i, fg, bg;
	int32_t  sx, sy;

	if (!msg)
		return;

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return;

	toast_dismiss();

	buf_size = TOAST_W * TOAST_H * 4;
	g_toast.pixels = anx_alloc(buf_size);
	if (!g_toast.pixels)
		return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_toast.pixels);
		g_toast.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_toast.pixels;
	cn->data_len = buf_size;

	sx = (int32_t)(fb->width  - TOAST_W - 16);
	sy = (int32_t)(fb->height - TOAST_H - 16);
	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     sx, sy, TOAST_W, TOAST_H,
				     &g_toast.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_toast.pixels);
		g_toast.pixels = NULL;
		return;
	}

	theme = anx_theme_get();
	bg    = theme->palette.surface;
	fg    = theme->palette.text_primary;

	/* Background fill */
	for (i = 0; i < TOAST_W * TOAST_H; i++)
		g_toast.pixels[i] = bg;

	/* Accent top border */
	for (i = 0; i < TOAST_W; i++)
		g_toast.pixels[i] = theme->palette.accent;

	/* Draw message text centred vertically in the toast strip */
	{
		uint32_t ty = (TOAST_H - ANX_FONT_HEIGHT) / 2;

		anx_font_blit_str(g_toast.pixels, TOAST_W, TOAST_H,
				  8, ty, msg, fg, bg);
	}

	g_toast.age = 0;
	g_toast.oid = g_toast.surf->oid;
	/* A notification must not take the keyboard from the user's window. */
	g_toast.surf->no_focus = true;
	anx_iface_surface_map(g_toast.surf);
	anx_iface_surface_raise(g_toast.surf);
	anx_iface_surface_commit(g_toast.surf);
}

/* ------------------------------------------------------------------ */
/* Mouse cursor                                                        */
/* ------------------------------------------------------------------ */

#define CURSOR_W  10
#define CURSOR_H  11

/* 0=transparent, 1=white (#FFFFFF), 2=black outline (#000000) */
enum cursor_type { CURSOR_ARROW = 0, CURSOR_RESIZE, CURSOR_MOVE,
		   CURSOR_COUNT };

/* Arrow: filled upper-left triangle, hotspot at (0,0) */
static const uint8_t cur_arrow[CURSOR_H][CURSOR_W] = {
	{2,0,0,0,0,0,0,0,0,0},
	{2,2,0,0,0,0,0,0,0,0},
	{2,1,2,0,0,0,0,0,0,0},
	{2,1,1,2,0,0,0,0,0,0},
	{2,1,1,1,2,0,0,0,0,0},
	{2,1,1,1,1,2,0,0,0,0},
	{2,1,1,1,1,1,2,0,0,0},
	{2,1,1,1,1,1,1,2,0,0},
	{2,1,1,1,1,1,1,1,2,0},
	{0,2,2,2,2,2,2,2,2,0},
	{0,0,0,0,0,0,0,0,0,0},
};

/* Resize: four-directional cross, hotspot at (4,4) */
static const uint8_t cur_resize[CURSOR_H][CURSOR_W] = {
	{0,0,0,0,2,2,0,0,0,0},
	{0,0,0,2,1,1,2,0,0,0},
	{0,0,0,0,2,2,0,0,0,0},
	{0,2,0,0,2,2,0,0,2,0},
	{2,1,2,2,1,1,2,2,1,2},
	{2,1,2,2,1,1,2,2,1,2},
	{0,2,0,0,2,2,0,0,2,0},
	{0,0,0,0,2,2,0,0,0,0},
	{0,0,0,2,1,1,2,0,0,0},
	{0,0,0,0,2,2,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0},
};

/* Move: open crosshair with gaps, hotspot at (4,4) */
static const uint8_t cur_move[CURSOR_H][CURSOR_W] = {
	{0,0,0,0,2,2,0,0,0,0},
	{0,0,0,0,2,1,2,0,0,0},
	{0,0,0,0,0,0,0,0,0,0},
	{0,2,0,0,0,0,0,0,2,0},
	{2,1,2,0,0,0,0,2,1,2},
	{0,2,0,0,0,0,0,0,2,0},
	{0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,2,1,2,0,0,0},
	{0,0,0,0,2,2,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0},
	{0,0,0,0,0,0,0,0,0,0},
};

static const uint8_t (* const cursor_shapes[CURSOR_COUNT])[CURSOR_W] = {
	[CURSOR_ARROW]  = cur_arrow,
	[CURSOR_RESIZE] = cur_resize,
	[CURSOR_MOVE]   = cur_move,
};

static enum cursor_type g_cursor_type = CURSOR_ARROW;

static int32_t  g_cur_x  = -1;
static int32_t  g_cur_y  = -1;
static bool     g_cur_on = false;
static uint32_t g_cur_saved[CURSOR_H][CURSOR_W];

static void cursor_draw(int32_t x, int32_t y);

static void cursor_set(enum cursor_type t)
{
	g_cursor_type = t;
}

static void cursor_erase(void)
{
	const struct anx_fb_info *fb;
	const uint8_t (*shape)[CURSOR_W];
	uint32_t r, c;

	if (!g_cur_on)
		return;
	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return;

	shape = cursor_shapes[g_cursor_type];
	if (g_cur_x >= 0 && g_cur_y >= 0)
		anx_fb_mark_dirty((uint32_t)g_cur_x, (uint32_t)g_cur_y,
				  CURSOR_W, CURSOR_H);
	for (r = 0; r < CURSOR_H; r++) {
		int32_t py = g_cur_y + (int32_t)r;
		if (py < 0 || (uint32_t)py >= fb->height)
			continue;
		for (c = 0; c < CURSOR_W; c++) {
			int32_t px = g_cur_x + (int32_t)c;
			if (px < 0 || (uint32_t)px >= fb->width)
				continue;
			if (shape[r][c] != 0)
				anx_fb_row_ptr((uint32_t)py)[px] =
					g_cur_saved[r][c];
		}
	}
	g_cur_on = false;
}

/*
 * The cursor is a save-under sprite written straight into the framebuffer,
 * and it is drawn only when the pointer moves. Anything else that paints the
 * same pixels -- the menu bar redrawing its clock, for instance -- overwrites
 * it, and it stays gone until the next pointer event. That is why the pointer
 * was invisible over the top bar while clicks still landed: hit testing never
 * depended on the sprite being there.
 *
 * A repaint also makes g_cur_saved stale. Restoring it would stamp the old
 * content back over whatever was just drawn, so invalidating means "forget
 * the saved pixels", not "erase".
 */
#ifndef INT32_MIN
#define INT32_MIN (-2147483647 - 1)
#endif

void anx_wm_cursor_invalidate_rect(int32_t rx, int32_t ry,
				   uint32_t rw, uint32_t rh)
{
	const struct anx_fb_info *fb;
	const uint8_t (*shape)[CURSOR_W];
	int32_t rx2 = rx + (int32_t)rw;
	int32_t ry2 = ry + (int32_t)rh;
	uint32_t r, c;

	if (!g_cur_on)
		return;

	/*
	 * No overlap: the repaint did not touch the sprite, so the saved
	 * pixels are still valid and nothing needs to happen. Invalidating
	 * anyway is what left a trail -- the next move skipped the erase and
	 * the old sprite stayed on the desktop.
	 */
	if (g_cur_x >= rx2 || g_cur_x + (int32_t)CURSOR_W <= rx ||
	    g_cur_y >= ry2 || g_cur_y + (int32_t)CURSOR_H <= ry)
		return;

	/*
	 * Partial overlap: pixels inside the repainted rect are already new
	 * content and must not be restored; pixels outside it still show the
	 * sprite and must be. Restore those now, then forget the save.
	 */
	fb = anx_fb_get_info();
	if (fb && fb->available) {
		shape = cursor_shapes[g_cursor_type];
		for (r = 0; r < CURSOR_H; r++) {
			int32_t py = g_cur_y + (int32_t)r;

			if (py < 0 || (uint32_t)py >= fb->height)
				continue;
			for (c = 0; c < CURSOR_W; c++) {
				int32_t px = g_cur_x + (int32_t)c;

				if (px < 0 || (uint32_t)px >= fb->width)
					continue;
				if (shape[r][c] == 0)
					continue;
				if (px >= rx && px < rx2 && py >= ry && py < ry2)
					continue;
				anx_fb_row_ptr((uint32_t)py)[px] =
					g_cur_saved[r][c];
			}
		}
	}
	g_cur_on = false;
}

/*
 * Erase before painting, not after: the saved pixels are still exactly what
 * lies under the sprite, so restoring them is always correct, and whatever
 * is painted next lands on a clean screen. The WM loop redraws the sprite
 * on its next pass. Covering only the menu bar left every other surface --
 * a terminal printing output, a toast, a window being raised -- free to
 * paint over the sprite and leave stale save-under pixels behind.
 */
void anx_wm_cursor_hide_rect(int32_t rx, int32_t ry, uint32_t rw, uint32_t rh)
{
	int32_t rx2 = rx + (int32_t)rw;
	int32_t ry2 = ry + (int32_t)rh;

	if (!g_cur_on)
		return;
	if (g_cur_x >= rx2 || g_cur_x + (int32_t)CURSOR_W <= rx ||
	    g_cur_y >= ry2 || g_cur_y + (int32_t)CURSOR_H <= ry)
		return;
	cursor_erase();
}

/* Whole-screen form, for callers that repaint everything. */
void anx_wm_cursor_invalidate(void)
{
	anx_wm_cursor_invalidate_rect(INT32_MIN / 2, INT32_MIN / 2,
				      0xFFFFFFFFu, 0xFFFFFFFFu);
}

/* Repaint the sprite if a commit has painted over it. */
static void cursor_refresh(void)
{
	if (g_cur_on || g_cur_x < 0 || g_cur_y < 0)
		return;
	cursor_draw(g_cur_x, g_cur_y);
}

static void cursor_draw(int32_t x, int32_t y)
{
	const struct anx_fb_info *fb;
	const uint8_t (*shape)[CURSOR_W];
	uint32_t r, c;

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return;

	g_cur_x = x;
	g_cur_y = y;
	shape   = cursor_shapes[g_cursor_type];
	if (x >= 0 && y >= 0)
		anx_fb_mark_dirty((uint32_t)x, (uint32_t)y,
				  CURSOR_W, CURSOR_H);

	for (r = 0; r < CURSOR_H; r++) {
		int32_t py = y + (int32_t)r;
		if (py < 0 || (uint32_t)py >= fb->height)
			continue;
		for (c = 0; c < CURSOR_W; c++) {
			int32_t px = x + (int32_t)c;
			if (px < 0 || (uint32_t)px >= fb->width)
				continue;
			if (shape[r][c] != 0) {
				uint32_t *row = anx_fb_row_ptr((uint32_t)py);
				g_cur_saved[r][c] = row[px];
				row[px] = (shape[r][c] == 1)
					  ? 0xFFFFFF : 0x000000;
			}
		}
	}
	g_cur_on = true;
}

/* ------------------------------------------------------------------ */
/* Snap preview                                                        */
/* ------------------------------------------------------------------ */

#define SNAP_BORDER  3u   /* border thickness in pixels */

static int g_snap_preview;  /* last drawn preview: 0=none, 1=left, 2=right */

static void snap_preview_draw(int snap)
{
	const struct anx_fb_info *fb = anx_fb_get_info();
	const struct anx_theme   *theme = anx_theme_get();
	uint32_t color;
	uint32_t x0, y0, w, h;

	if (!fb || !fb->available)
		return;
	if (snap == 0)
		return;

	color = theme->palette.accent;
	y0    = ANX_WM_MENUBAR_H;
	h     = fb->height - ANX_WM_MENUBAR_H - ANX_WM_TASKBAR_H;

	if (snap == 1) {
		x0 = 0;
		w  = fb->width / 2;
	} else {
		x0 = fb->width / 2;
		w  = fb->width - x0;
	}

	/* Outline: top, bottom, left, right strips */
	anx_fb_fill_rect(x0, y0, w, SNAP_BORDER, color);
	anx_fb_fill_rect(x0, y0 + h - SNAP_BORDER, w, SNAP_BORDER, color);
	anx_fb_fill_rect(x0, y0, SNAP_BORDER, h, color);
	anx_fb_fill_rect(x0 + w - SNAP_BORDER, y0, SNAP_BORDER, h, color);

	g_snap_preview = snap;
}

static void snap_preview_erase(void)
{
	const struct anx_fb_info *fb = anx_fb_get_info();
	uint32_t x0, y0, w, h;

	if (!fb || !fb->available || g_snap_preview == 0)
		return;

	y0 = ANX_WM_MENUBAR_H;
	h  = fb->height - ANX_WM_MENUBAR_H - ANX_WM_TASKBAR_H;

	if (g_snap_preview == 1) {
		x0 = 0;
		w  = fb->width / 2;
	} else {
		x0 = fb->width / 2;
		w  = fb->width - x0;
	}

	/* Repaint just the border strips with the desktop behind them */
	anx_wm_desktop_paint(x0, y0, w, SNAP_BORDER);
	anx_wm_desktop_paint(x0, y0 + h - SNAP_BORDER, w, SNAP_BORDER);
	anx_wm_desktop_paint(x0, y0, SNAP_BORDER, h);
	anx_wm_desktop_paint(x0 + w - SNAP_BORDER, y0, SNAP_BORDER, h);

	g_snap_preview = 0;
}

/* ------------------------------------------------------------------ */
/* Workspace helpers                                                   */
/* ------------------------------------------------------------------ */

static struct anx_wm_workspace *active_ws(void)
{
	return &g_workspaces[g_active_ws];
}

static void ws_retile(struct anx_wm_workspace *ws);

static bool oid_null(const anx_oid_t *o)
{
	return o->hi == 0 && o->lo == 0;
}

static bool oid_eq(const anx_oid_t *a, const anx_oid_t *b)
{
	return a->hi == b->hi && a->lo == b->lo;
}

/* Renderer and pointer routing share the exact titlebar layout. */
static enum anx_window_button wm_decor_button_at(struct anx_surface *surf,
					       int32_t x, int32_t y)
{
	struct anx_window_chrome chrome;
	int32_t top;

	if (!surf || !surf->title[0] || surf->y < (int32_t)ANX_WM_DECOR_H)
		return ANX_WINDOW_BUTTON_NONE;
	top = surf->y - (int32_t)ANX_WM_DECOR_H;
	anx_window_chrome_layout(anx_theme_get()->deco.controls,
				surf->width, ANX_WM_DECOR_H, &chrome);
	return anx_window_chrome_hit(&chrome, x - surf->x, y - top);
}

/* Find the surface whose decoration area (above canvas) contains (x, y). */
static struct anx_surface *wm_surface_at_decor(int32_t x, int32_t y)
{
	struct anx_wm_workspace *ws = &g_workspaces[g_active_ws];
	uint32_t i;

	for (i = 0; i < ws->surf_count; i++) {
		struct anx_surface *s = NULL;

		if (anx_iface_surface_lookup(ws->surfs[i], &s) != ANX_OK || !s)
			continue;
		if (s->state != ANX_SURF_VISIBLE || !s->title[0])
			continue;
		if (x >= s->x && x < s->x + (int32_t)s->width &&
		    y >= s->y - (int32_t)ANX_WM_DECOR_H && y < s->y)
			return s;
	}
	return NULL;
}

/* Return which resize edges (1=right, 2=bottom) cursor (x,y) hits on surf. */
static uint8_t surf_resize_edges(struct anx_surface *surf, int32_t x, int32_t y)
{
	int32_t  sx = surf->x, sy = surf->y;
	int32_t  rx = sx + (int32_t)surf->width;
	int32_t  by = sy + (int32_t)surf->height;
	uint8_t  edges = 0;

	if (x >= rx - (int32_t)RESIZE_EDGE && x <= rx + (int32_t)RESIZE_EDGE &&
	    y >= sy && y <= by)
		edges |= 1;
	if (y >= by - (int32_t)RESIZE_EDGE && y <= by + (int32_t)RESIZE_EDGE &&
	    x >= sx && x <= rx)
		edges |= 2;
	return edges;
}

/* Return the workspace a surface belongs to, or NULL. */
static struct anx_wm_workspace *ws_of(const anx_oid_t *oid)
{
	uint32_t i, j;

	for (i = 0; i < ANX_WM_WORKSPACES; i++) {
		struct anx_wm_workspace *ws = &g_workspaces[i];

		for (j = 0; j < ws->surf_count; j++) {
			if (oid_eq(&ws->surfs[j], oid))
				return ws;
		}
	}
	return NULL;
}

/* Remove a surface OID from a workspace's list. */
static void ws_remove(struct anx_wm_workspace *ws, const anx_oid_t *oid)
{
	uint32_t i;

	for (i = 0; i < ws->surf_count; i++) {
		if (oid_eq(&ws->surfs[i], oid)) {
			/* Keep the rest in opening order; focus fallback uses it. */
			ws->surf_count--;
			for (; i < ws->surf_count; i++)
				ws->surfs[i] = ws->surfs[i + 1];
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Workspace switching                                                 */
/* ------------------------------------------------------------------ */

int anx_wm_workspace_switch(uint32_t ws_id)
{
	struct anx_wm_workspace *old_ws, *new_ws;
	uint32_t i;
	anx_oid_t null_oid = {0, 0};

	if (ws_id < 1 || ws_id > ANX_WM_WORKSPACES)
		return ANX_EINVAL;

	if (ws_id - 1 == g_active_ws)
		return ANX_OK;

	old_ws = &g_workspaces[g_active_ws];
	new_ws = &g_workspaces[ws_id - 1];

	/* Hide all visible surfaces on the old workspace */
	for (i = 0; i < old_ws->surf_count; i++) {
		struct anx_surface *s = NULL;
		anx_iface_surface_lookup(old_ws->surfs[i], &s);
		if (s && s->state == ANX_SURF_VISIBLE)
			s->state = ANX_SURF_MINIMIZED;
	}

	g_active_ws = ws_id - 1;

	/* Show windows on the new workspace that were not user-minimized */
	for (i = 0; i < new_ws->surf_count; i++) {
		struct anx_surface *s = NULL;
		anx_iface_surface_lookup(new_ws->surfs[i], &s);
		if (s && s->state == ANX_SURF_MINIMIZED && !s->user_minimized)
			s->state = ANX_SURF_VISIBLE;
	}

	/* Restore focus first: the repaint below colours title bars by it. */
	if (!oid_null(&new_ws->focused)) {
		anx_input_focus_set(new_ws->focused);
	} else {
		anx_input_focus_set(null_oid);
	}

	/* Lay out and repaint the whole work area in stacking order; that
	 * also clears the old workspace's windows. */
	ws_retile(new_ws);

	anx_wm_menubar_refresh();
	anx_wm_taskbar_refresh();
	kprintf("[wm] workspace %u\n", ws_id);
	return ANX_OK;
}

uint32_t anx_wm_workspace_active(void)
{
	return g_active_ws + 1;
}

bool anx_wm_workspace_occupied(uint32_t ws_id)
{
	if (ws_id < 1 || ws_id > ANX_WM_WORKSPACES)
		return false;
	return g_workspaces[ws_id - 1].surf_count > 0;
}

uint32_t anx_wm_minimized_list(anx_oid_t *out, uint32_t max)
{
	struct anx_wm_workspace *ws = &g_workspaces[g_active_ws];
	uint32_t n = 0, i;

	for (i = 0; i < ws->surf_count && n < max; i++) {
		struct anx_surface *s = NULL;

		anx_iface_surface_lookup(ws->surfs[i], &s);
		if (s && s->state == ANX_SURF_MINIMIZED)
			out[n++] = ws->surfs[i];
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Window lifecycle                                                    */
/* ------------------------------------------------------------------ */

void anx_wm_canvas_free(struct anx_surface *surf)
{
	struct anx_content_node *cn;

	if (!surf || !surf->content_root)
		return;
	cn = surf->content_root;
	surf->content_root = NULL;
	if (cn->data)
		anx_free(cn->data);
	anx_free(cn);
}

uint32_t *anx_wm_canvas_realloc(struct anx_surface *surf, uint32_t w,
				uint32_t h)
{
	struct anx_content_node *cn;
	uint64_t bytes = (uint64_t)w * h * 4u;
	uint32_t *nb;

	if (!surf || !surf->content_root || !w || !h ||
	    bytes > ANX_WM_WINDOW_BYTES_MAX)
		return NULL;
	nb = anx_alloc((size_t)bytes);
	if (!nb)
		return NULL;
	anx_memset(nb, 0, (size_t)bytes);
	cn = surf->content_root;
	if (cn->data)
		anx_free(cn->data);
	cn->data     = nb;
	cn->data_len = (uint32_t)bytes;
	surf->buf_w  = w;
	surf->buf_h  = h;
	return nb;
}

/*
 * A window's painted extent: canvas, title bar, border and the shadow
 * around them. Repaints work from this rectangle, so leaving the shadow
 * out of it would smear the shadow across the desktop when a window
 * moves or closes.
 */
static void surf_outer_rect(const struct anx_surface *s, int32_t *x,
			    int32_t *y, uint32_t *w, uint32_t *h)
{
	uint32_t bw = s->title[0] ? anx_wm_tiling.border_w : 0;
	uint32_t reach = s->title[0] ? anx_wm_shadow_reach() : 0;

	*x = s->x;
	*y = s->y;
	*w = s->width;
	*h = s->height;
	if (s->title[0] && s->y >= (int32_t)ANX_WM_DECOR_H) {
		*y -= (int32_t)ANX_WM_DECOR_H;
		*h += ANX_WM_DECOR_H;
	}
	*x -= (int32_t)(bw + reach);
	*y -= (int32_t)(bw + reach);
	*w += 2 * (bw + reach);
	*h += 2 * (bw + reach);
}

/* Move and resize without repainting; the caller exposes afterwards. */
static void wm_place(struct anx_surface *s, int32_t x, int32_t y,
		     uint32_t w, uint32_t h)
{
	bool resized = w != s->width || h != s->height;

	anx_iface_surface_move(s, x, y);
	s->width  = w;
	s->height = h;
	if (resized && s->on_resize)
		s->on_resize(s);
}

/* Scratch for anx_wm_expose(); the WM runs on one thread. */
static anx_oid_t          g_expose_oids[ANX_SURF_MAX];
static struct anx_wm_rect g_expose_dirty[ANX_SURF_MAX + 1];

static bool rect_overlaps(const struct anx_wm_rect *r, int32_t x, int32_t y,
			  uint32_t w, uint32_t h)
{
	return x < r->x + (int32_t)r->w && r->x < x + (int32_t)w &&
	       y < r->y + (int32_t)r->h && r->y < y + (int32_t)h;
}

/*
 * A commit paints a whole surface, not just the exposed part, so a window
 * repainted here can cover windows above it that lie outside the exposed
 * rectangle. Painter's algorithm over a dirty list: walk bottom to top,
 * and every window repainted adds its own rectangle, so anything above
 * that it overlaps is repainted after it.
 */
static bool g_in_repaint;

void anx_wm_expose(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	const struct anx_fb_info *fb = anx_fb_get_info();
	int32_t x2, y2;
	uint32_t n = 0, i, ndirty, d;

	if (!fb || !fb->available || !w || !h)
		return;
	x2 = x + (int32_t)w;
	y2 = y + (int32_t)h;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x2 > (int32_t)fb->width)
		x2 = (int32_t)fb->width;
	if (y2 > (int32_t)fb->height)
		y2 = (int32_t)fb->height;
	if (x2 <= x || y2 <= y)
		return;

	anx_wm_cursor_hide_rect(x, y, (uint32_t)(x2 - x), (uint32_t)(y2 - y));
	g_in_repaint = true;
	anx_wm_desktop_paint((uint32_t)x, (uint32_t)y, (uint32_t)(x2 - x),
			     (uint32_t)(y2 - y));

	g_expose_dirty[0].x = x;
	g_expose_dirty[0].y = y;
	g_expose_dirty[0].w = (uint32_t)(x2 - x);
	g_expose_dirty[0].h = (uint32_t)(y2 - y);
	ndirty = 1;

	/* The list is front to back; paint back to front. */
	anx_iface_surface_list(g_expose_oids, ANX_SURF_MAX, &n);
	for (i = n; i-- > 0;) {
		struct anx_surface *s = NULL;
		int32_t sx, sy;
		uint32_t sw, sh;
		bool flags, hit = false;

		if (anx_iface_surface_lookup(g_expose_oids[i], &s) != ANX_OK ||
		    !s || s->state != ANX_SURF_VISIBLE)
			continue;
		surf_outer_rect(s, &sx, &sy, &sw, &sh);
		for (d = 0; d < ndirty && !hit; d++)
			hit = rect_overlaps(&g_expose_dirty[d], sx, sy, sw, sh);
		if (!hit)
			continue;
		/* Pending damage would clip the blit to a stale rectangle. */
		anx_spin_lock_irqsave(&s->lock, &flags);
		s->damage_valid = false;
		anx_spin_unlock_irqrestore(&s->lock, flags);
		anx_iface_surface_commit_flat(s);
		if (ndirty <= ANX_SURF_MAX) {
			g_expose_dirty[ndirty].x = sx;
			g_expose_dirty[ndirty].y = sy;
			g_expose_dirty[ndirty].w = sw;
			g_expose_dirty[ndirty].h = sh;
			ndirty++;
		}
	}
	g_in_repaint = false;
}

void anx_wm_repaint_all(void)
{
	const struct anx_fb_info *fb = anx_fb_get_info();
	const struct anx_theme *theme = anx_theme_get();
	static enum anx_font_family painted_family = ANX_FONT_FAMILY_COUNT;
	static bool painted_antialiased;

	if (painted_family != theme->font.family ||
	    painted_antialiased != theme->font.antialiased) {
		/* Canvas pixels cache glyphs; repaint them before composing windows. */
		painted_family = theme->font.family;
		painted_antialiased = theme->font.antialiased;
		anx_wm_native_terminals_redraw();
		anx_wm_terminal_redraw();
		anx_wm_agent_redraw();
	}
	anx_wm_color_editor_redraw();

	if (!fb || !fb->available)
		return;
	anx_wm_menubar_refresh();
	anx_wm_taskbar_refresh();
	anx_wm_cursor_invalidate();
	anx_wm_expose(0, 0, fb->width, fb->height);
}

bool anx_wm_in_repaint(void)
{
	return g_in_repaint;
}

int anx_wm_window_set_geometry(struct anx_surface *surf, int32_t x,
			       int32_t y, uint32_t w, uint32_t h)
{
	int32_t ox, oy, nx, ny, ux, uy, ux2, uy2;
	uint32_t ow, oh, nw, nh;

	if (!surf)
		return ANX_EINVAL;
	if (surf->x == x && surf->y == y && surf->width == w &&
	    surf->height == h)
		return ANX_OK;
	surf_outer_rect(surf, &ox, &oy, &ow, &oh);
	wm_place(surf, x, y, w, h);
	surf_outer_rect(surf, &nx, &ny, &nw, &nh);

	/*
	 * Repaint old and new rectangles together, in stacking order, so the
	 * uncovered area is cleaned up and the window does not land on top
	 * of the menu bar or anything else above it.
	 */
	ux  = ox < nx ? ox : nx;
	uy  = oy < ny ? oy : ny;
	ux2 = ox + (int32_t)ow > nx + (int32_t)nw ? ox + (int32_t)ow
						   : nx + (int32_t)nw;
	uy2 = oy + (int32_t)oh > ny + (int32_t)nh ? oy + (int32_t)oh
						   : ny + (int32_t)nh;
	anx_wm_expose(ux, uy, (uint32_t)(ux2 - ux), (uint32_t)(uy2 - uy));
	return ANX_OK;
}

void anx_wm_window_fit(uint32_t *w, uint32_t *h)
{
	uint64_t pages, max_px;

	if (!w || !h)
		return;
	anx_page_stats(&pages, NULL);
	max_px = (pages << ANX_PAGE_SHIFT) >= ANX_WM_LARGE_HEAP_BYTES ?
		 ANX_WM_WINDOW_BYTES_MAX / 4u : ANX_WM_WINDOW_BYTES_SMALL / 4u;
	/* 7/8 steps keep the aspect ratio without floating point. */
	while ((uint64_t)*w * *h > max_px && *w > 8 && *h > 8) {
		*w = *w * 7u / 8u;
		*h = *h * 7u / 8u;
	}
}

/* ---- Layout plumbing -------------------------------------------------- */

/* The screen between the menu bar and the taskbar. */
static bool work_area(struct anx_wm_rect *r)
{
	const struct anx_fb_info *fb = anx_fb_get_info();

	if (!fb || !fb->available ||
	    fb->height <= ANX_WM_MENUBAR_H + ANX_WM_TASKBAR_H)
		return false;
	r->x = 0;
	r->y = (int32_t)ANX_WM_MENUBAR_H;
	r->w = fb->width;
	r->h = fb->height - ANX_WM_MENUBAR_H - ANX_WM_TASKBAR_H;
	return true;
}

/* A layout box holds the border and title bar; the canvas sits inside. */
static void box_to_window(const struct anx_surface *s,
			  const struct anx_wm_rect *b, struct anx_wm_rect *win)
{
	uint32_t bw  = s->title[0] ? anx_wm_tiling.border_w : 0;
	uint32_t top = bw + (s->title[0] ? ANX_WM_DECOR_H : 0);

	win->x = b->x + (int32_t)bw;
	win->y = b->y + (int32_t)top;
	win->w = b->w > 2 * bw + 16 ? b->w - 2 * bw : 16;
	win->h = b->h > top + bw + 16 ? b->h - top - bw : 16;
}

static anx_oid_t          g_lay_oids[ANX_WM_WS_SURFS];
static struct anx_wm_rect g_lay_boxes[ANX_WM_WS_SURFS];

/*
 * Apply a workspace's layout. Windows are moved first and the work area
 * repainted once at the end, in stacking order, so floating windows stay
 * above tiled ones. An inactive workspace only gets new geometry.
 */
static void ws_retile(struct anx_wm_workspace *ws)
{
	struct anx_wm_rect area, win;
	uint32_t n, i;

	if (!work_area(&area))
		return;
	n = anx_wm_tile_layout(&ws->tiles, &area, &anx_wm_tiling,
			       g_lay_oids, g_lay_boxes, ANX_WM_WS_SURFS);
	for (i = 0; i < n; i++) {
		struct anx_surface *s = NULL;
		struct anx_wm_rect box = g_lay_boxes[i];

		if (anx_iface_surface_lookup(g_lay_oids[i], &s) != ANX_OK || !s)
			continue;
		/* Fullscreen (sway "fullscreen", Hyprland mode 1): the whole
		 * work area, gaps dropped, drawn above its neighbours. */
		if (oid_eq(&ws->fullscreen, &s->oid))
			box = area;
		box_to_window(s, &box, &win);
		wm_place(s, win.x, win.y, win.w, win.h);
	}
	/* Down to the screen edge: a drag can leave frames over the taskbar. */
	if (ws == active_ws())
		anx_wm_expose(area.x, area.y, area.w,
			      area.h + ANX_WM_TASKBAR_H);
}

void anx_wm_retile(void)
{
	ws_retile(active_ws());
}

void anx_wm_overlay_destroy(struct anx_surface *surf)
{
	int32_t x, y;
	uint32_t w, h;

	if (!surf)
		return;
	surf_outer_rect(surf, &x, &y, &w, &h);
	anx_iface_surface_destroy(surf);
	/* The unmap only fills with the desktop colour; bring back what the
	 * overlay covered, now that tiled windows fill the screen. */
	anx_wm_expose(x, y, w, h);
}

struct anx_surface *anx_wm_focused_window(void)
{
	struct anx_surface *s = NULL;
	struct anx_wm_workspace *ws = active_ws();

	if (oid_null(&ws->focused) ||
	    anx_iface_surface_lookup(ws->focused, &s) != ANX_OK)
		return NULL;
	return s;
}

bool anx_wm_window_is_tiled(const struct anx_surface *surf)
{
	struct anx_wm_workspace *ws;

	if (!surf)
		return false;
	ws = ws_of(&surf->oid);
	return ws && anx_wm_tile_contains(&ws->tiles, &surf->oid);
}

/* Repaint a window's rectangle in stacking order (title colour changes). */
static void wm_repaint_window(struct anx_surface *s)
{
	int32_t x, y;
	uint32_t w, h;

	if (!s || s->state != ANX_SURF_VISIBLE)
		return;
	surf_outer_rect(s, &x, &y, &w, &h);
	anx_wm_expose(x, y, w, h);
}

/* The newest visible window on ws other than skip, or a nil OID. */
static anx_oid_t ws_pick_focus(struct anx_wm_workspace *ws,
			       const anx_oid_t *skip)
{
	anx_oid_t none = {0, 0};
	uint32_t i;

	for (i = ws->surf_count; i-- > 0;) {
		struct anx_surface *s = NULL;

		if (skip && oid_eq(&ws->surfs[i], skip))
			continue;
		anx_iface_surface_lookup(ws->surfs[i], &s);
		if (s && s->state == ANX_SURF_VISIBLE)
			return ws->surfs[i];
	}
	return none;
}

/* Move ws's focus to oid (or the best fallback) and tell input about it. */
static void ws_set_focus(struct anx_wm_workspace *ws, anx_oid_t oid)
{
	struct anx_surface *s = NULL;

	if (oid_null(&oid))
		oid = ws_pick_focus(ws, NULL);
	if (!oid_null(&oid) &&
	    (anx_iface_surface_lookup(oid, &s) != ANX_OK || !s ||
	     s->state != ANX_SURF_VISIBLE))
		oid = ws_pick_focus(ws, NULL);
	ws->focused = oid;
	if (ws == active_ws())
		anx_input_focus_set(oid);
}

static int wm_open(struct anx_surface *surf, bool tile)
{
	struct anx_wm_workspace *ws;
	anx_oid_t target;

	if (!surf)
		return ANX_EINVAL;

	ws = active_ws();
	if (ws->surf_count >= ANX_WM_WS_SURFS)
		return ANX_ENOMEM;

	target = ws->focused;
	ws->surfs[ws->surf_count++] = surf->oid;

	if (tile && anx_wm_tile_insert(&ws->tiles, &surf->oid,
				       &target) == ANX_OK) {
		struct anx_surface *fs = NULL;

		surf->wm_flags &= ~ANX_WM_SF_FLOATING;
		/* A new window ends fullscreen, as in sway. */
		if (!oid_null(&ws->fullscreen) &&
		    anx_iface_surface_lookup(ws->fullscreen, &fs) == ANX_OK &&
		    fs)
			anx_iface_surface_lower(fs);
		ws->fullscreen.hi = ws->fullscreen.lo = 0;
		/* Tiled windows stack below floating ones. */
		anx_iface_surface_lower(surf);
		ws_retile(ws);
	} else {
		surf->wm_flags |= ANX_WM_SF_FLOATING;
		anx_iface_surface_raise(surf);
		wm_repaint_window(surf);
	}
	return anx_wm_window_focus(surf);
}

int anx_wm_window_open(struct anx_surface *surf)
{
	return wm_open(surf, anx_wm_tiling.enabled);
}

int anx_wm_window_open_floating(struct anx_surface *surf)
{
	return wm_open(surf, false);
}

int anx_wm_window_close(struct anx_surface *surf)
{
	struct anx_wm_workspace *ws;
	anx_oid_t heir = {0, 0};
	struct anx_surface *next = NULL;
	bool was_tiled = false, was_focused = false;
	int32_t ox, oy;
	uint32_t ow, oh;

	if (!surf)
		return ANX_EINVAL;

	surf_outer_rect(surf, &ox, &oy, &ow, &oh);
	ws = ws_of(&surf->oid);
	if (ws) {
		was_tiled = anx_wm_tile_remove(&ws->tiles, &surf->oid,
					       &heir) == ANX_OK;
		if (oid_eq(&ws->fullscreen, &surf->oid))
			ws->fullscreen.hi = ws->fullscreen.lo = 0;
		ws_remove(ws, &surf->oid);
		/* Focus the window that takes over the closed one's space
		 * (sway focuses the sibling), else the newest visible one. */
		if (oid_eq(&ws->focused, &surf->oid)) {
			was_focused = true;
			ws_set_focus(ws, heir);
		}
	}

	anx_iface_surface_destroy(surf);

	if (ws && was_tiled)
		ws_retile(ws);		/* the rest grow into the space */
	else
		anx_wm_expose(ox, oy, ow, oh);

	if (ws && was_focused && ws == active_ws() &&
	    anx_iface_surface_lookup(ws->focused, &next) == ANX_OK && next)
		anx_wm_window_focus(next);
	anx_wm_menubar_refresh();
	return ANX_OK;
}

int anx_wm_window_focus(struct anx_surface *surf)
{
	struct anx_wm_workspace *ws;
	struct anx_surface *prev_surf = NULL;
	bool floating_or_fs;

	if (!surf)
		return ANX_EINVAL;

	ws = ws_of(&surf->oid);
	if (!ws)
		ws = active_ws();

	/* The workspace's record, not input focus, says who had it. */
	anx_iface_surface_lookup(ws->focused, &prev_surf);

	/* Tiled windows never overlap, so only floating (or fullscreen)
	 * windows need raising; raising a tiled one would cover floats. */
	floating_or_fs = !anx_wm_tile_contains(&ws->tiles, &surf->oid) ||
			 oid_eq(&ws->fullscreen, &surf->oid);
	if (floating_or_fs)
		anx_iface_surface_raise(surf);
	ws->focused = surf->oid;
	if (ws == active_ws())
		anx_input_focus_set(surf->oid);

	/* Record last-used timestamp for the switcher. */
	anx_wm_activity_touch(surf->oid);

	/* Repaint both title bars and borders in stacking order. */
	wm_repaint_window(surf);
	if (prev_surf && prev_surf != surf)
		wm_repaint_window(prev_surf);

	/* Keep the panels on top. They never take the keyboard (no_focus). */
	if (g_menubar) {
		anx_iface_surface_raise(g_menubar);
		anx_wm_menubar_refresh();
	}
	anx_wm_taskbar_raise();

	return ANX_OK;
}

int anx_wm_window_restore(struct anx_surface *surf)
{
	struct anx_wm_workspace *ws;

	if (!surf)
		return ANX_EINVAL;

	ws = ws_of(&surf->oid);
	surf->user_minimized = false;
	surf->state = ANX_SURF_VISIBLE;

	if (ws && (surf->wm_flags & ANX_WM_SF_WAS_TILED) &&
	    anx_wm_tile_insert(&ws->tiles, &surf->oid,
			       &ws->focused) == ANX_OK) {
		surf->wm_flags &= ~ANX_WM_SF_WAS_TILED;
		anx_iface_surface_lower(surf);
		ws_retile(ws);
	} else {
		surf->wm_flags &= ~ANX_WM_SF_WAS_TILED;
		anx_iface_surface_raise(surf);
		/* Marking it visible draws nothing; paint it back. */
		wm_repaint_window(surf);
	}

	anx_wm_window_focus(surf);
	anx_wm_taskbar_refresh();
	return ANX_OK;
}

int anx_wm_window_minimize(struct anx_surface *surf)
{
	struct anx_wm_workspace *ws;
	anx_oid_t heir = {0, 0};
	bool was_tiled = false;
	int32_t ox, oy;
	uint32_t ow, oh;

	if (!surf)
		return ANX_EINVAL;

	surf_outer_rect(surf, &ox, &oy, &ow, &oh);
	ws = ws_of(&surf->oid);
	if (ws) {
		was_tiled = anx_wm_tile_remove(&ws->tiles, &surf->oid,
					       &heir) == ANX_OK;
		if (was_tiled)
			surf->wm_flags |= ANX_WM_SF_WAS_TILED;
		if (oid_eq(&ws->fullscreen, &surf->oid))
			ws->fullscreen.hi = ws->fullscreen.lo = 0;
	}

	surf->user_minimized = true;
	surf->state = ANX_SURF_MINIMIZED;

	if (ws && oid_eq(&ws->focused, &surf->oid))
		ws_set_focus(ws, heir);

	if (ws && was_tiled)
		ws_retile(ws);
	else
		anx_wm_expose(ox, oy, ow, oh);

	if (ws && ws == active_ws()) {
		struct anx_surface *next = NULL;

		if (anx_iface_surface_lookup(ws->focused, &next) == ANX_OK &&
		    next)
			anx_wm_window_focus(next);
	}
	anx_wm_taskbar_refresh();
	return ANX_OK;
}

int anx_wm_window_fullscreen_toggle(struct anx_surface *surf)
{
	const struct anx_fb_info *fb;
	struct anx_wm_workspace *ws;

	if (!surf)
		return ANX_EINVAL;

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return ANX_ENOENT;

	ws = ws_of(&surf->oid);
	if (ws && anx_wm_tile_contains(&ws->tiles, &surf->oid)) {
		struct anx_surface *old = NULL;

		if (oid_eq(&ws->fullscreen, &surf->oid)) {
			ws->fullscreen.hi = ws->fullscreen.lo = 0;
			anx_iface_surface_lower(surf);
		} else {
			if (!oid_null(&ws->fullscreen) &&
			    anx_iface_surface_lookup(ws->fullscreen, &old) ==
			    ANX_OK && old)
				anx_iface_surface_lower(old);
			ws->fullscreen = surf->oid;
			anx_iface_surface_raise(surf);
		}
		ws_retile(ws);
		return anx_wm_window_focus(surf);
	}

	if (g_fs_saved.active && g_fs_saved.surf == surf) {
		/* Restore */
		g_fs_saved.active = false;
		return anx_wm_window_set_geometry(surf, g_fs_saved.x,
						  g_fs_saved.y, g_fs_saved.w,
						  g_fs_saved.h);
	} else {
		/* Save and go fullscreen */
		g_fs_saved.surf   = surf;
		g_fs_saved.x      = surf->x;
		g_fs_saved.y      = surf->y;
		g_fs_saved.w      = surf->width;
		g_fs_saved.h      = surf->height;
		g_fs_saved.active = true;
		/*
		 * The work area, not the whole screen: at (0,0) the window hid
		 * the menu bar and lost its own title bar, and with it the
		 * button that would restore it.
		 */
		return anx_wm_window_set_geometry(surf, 0,
			(int32_t)(ANX_WM_MENUBAR_H + ANX_WM_DECOR_H),
			fb->width,
			fb->height - ANX_WM_MENUBAR_H - ANX_WM_DECOR_H -
			ANX_WM_TASKBAR_H);
	}
}

int anx_wm_window_float_toggle(struct anx_surface *surf)
{
	struct anx_wm_workspace *ws;
	struct anx_wm_rect area, box, win;

	if (!surf)
		return ANX_EINVAL;
	ws = ws_of(&surf->oid);
	if (!ws || !work_area(&area))
		return ANX_ENOENT;

	if (anx_wm_tile_contains(&ws->tiles, &surf->oid)) {
		/* Out of the layout: centred at 60% of the work area, on top
		 * (Hyprland togglefloating / sway floating toggle). */
		anx_wm_tile_remove(&ws->tiles, &surf->oid, NULL);
		if (oid_eq(&ws->fullscreen, &surf->oid))
			ws->fullscreen.hi = ws->fullscreen.lo = 0;
		surf->wm_flags |= ANX_WM_SF_FLOATING;
		box.w = area.w * 3u / 5u;
		box.h = area.h * 3u / 5u;
		box.x = area.x + (int32_t)((area.w - box.w) / 2u);
		box.y = area.y + (int32_t)((area.h - box.h) / 2u);
		box_to_window(surf, &box, &win);
		wm_place(surf, win.x, win.y, win.w, win.h);
		anx_iface_surface_raise(surf);
		anx_wm_notify("Floating");
	} else {
		if (anx_wm_tile_insert(&ws->tiles, &surf->oid, NULL) != ANX_OK)
			return ANX_ENOMEM;
		surf->wm_flags &= ~ANX_WM_SF_FLOATING;
		anx_iface_surface_lower(surf);
		anx_wm_notify("Tiled");
	}
	ws_retile(ws);
	return anx_wm_window_focus(surf);
}

/* ------------------------------------------------------------------ */
/* Tiling                                                             */
/* ------------------------------------------------------------------ */

/*
 * The nearest window from cur in one direction, measured between window
 * centres, counting movement along the requested axis first so a window
 * slightly off to the side does not beat one directly ahead.
 */
static struct anx_surface *wm_neighbour(struct anx_wm_workspace *ws,
					struct anx_surface *cur,
					int32_t dx, int32_t dy,
					bool tiled_only)
{
	struct anx_surface *best = NULL;
	uint64_t best_score = 0;
	int32_t cx, cy;
	uint32_t i;

	cx = cur->x + (int32_t)cur->width / 2;
	cy = cur->y + (int32_t)cur->height / 2;

	for (i = 0; i < ws->surf_count; i++) {
		struct anx_surface *s = NULL;
		int32_t sx, sy, along, across;
		uint64_t score;

		anx_iface_surface_lookup(ws->surfs[i], &s);
		if (!s || s == cur || s->state != ANX_SURF_VISIBLE)
			continue;
		if (tiled_only && !anx_wm_tile_contains(&ws->tiles, &s->oid))
			continue;

		sx = s->x + (int32_t)s->width / 2;
		sy = s->y + (int32_t)s->height / 2;
		along  = dx ? (sx - cx) * dx : (sy - cy) * dy;
		across = dx ? (sy - cy) : (sx - cx);
		if (along <= 0)
			continue;	/* not in the requested direction */
		if (across < 0)
			across = -across;

		score = (uint64_t)along + (uint64_t)across * 4u;
		if (!best || score < best_score) {
			best = s;
			best_score = score;
		}
	}
	return best;
}

int anx_wm_window_swap_dir(struct anx_surface *surf, int32_t dx, int32_t dy)
{
	struct anx_wm_workspace *ws;
	struct anx_surface *other;

	if (!surf)
		return ANX_EINVAL;
	ws = ws_of(&surf->oid);
	if (!ws || !anx_wm_tile_contains(&ws->tiles, &surf->oid))
		return ANX_ENOENT;
	other = wm_neighbour(ws, surf, dx, dy, true);
	if (!other)
		return ANX_ENOENT;
	anx_wm_tile_swap(&ws->tiles, &surf->oid, &other->oid);
	ws_retile(ws);
	return anx_wm_window_focus(surf);
}

int anx_wm_window_resize_tiled(struct anx_surface *surf, bool vertical,
			       int32_t delta_permille)
{
	struct anx_wm_workspace *ws;
	int rc;

	if (!surf)
		return ANX_EINVAL;
	ws = ws_of(&surf->oid);
	if (!ws)
		return ANX_ENOENT;
	rc = anx_wm_tile_resize(&ws->tiles, &surf->oid, vertical,
				delta_permille);
	if (rc == ANX_OK)
		ws_retile(ws);
	return rc;
}

/* ---- Half-screen snapping for floating windows ---- */

static void tile_save(struct anx_surface *surf)
{
	if (g_tile_saved.active && g_tile_saved.surf == surf)
		return;
	g_tile_saved.surf   = surf;
	g_tile_saved.x      = surf->x;
	g_tile_saved.y      = surf->y;
	g_tile_saved.w      = surf->width;
	g_tile_saved.h      = surf->height;
	g_tile_saved.active = true;
}

/* Meta+[ and Meta+]: a tiled window swaps with its neighbour; a floating
 * one snaps to that half of the screen. */
int anx_wm_window_tile_left(struct anx_surface *surf)
{
	const struct anx_fb_info *fb;
	uint32_t top;

	if (!surf)
		return ANX_EINVAL;
	if (anx_wm_window_is_tiled(surf))
		return anx_wm_window_swap_dir(surf, -1, 0);
	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return ANX_ENOENT;

	tile_save(surf);
	top = ANX_WM_MENUBAR_H;
	anx_wm_window_set_geometry(surf, 0, (int32_t)(top + ANX_WM_DECOR_H),
				   fb->width / 2,
				   fb->height - top - ANX_WM_DECOR_H -
				   ANX_WM_TASKBAR_H);
	anx_wm_notify("Snapped left");
	return ANX_OK;
}

int anx_wm_window_tile_right(struct anx_surface *surf)
{
	const struct anx_fb_info *fb;
	uint32_t top;

	if (!surf)
		return ANX_EINVAL;
	if (anx_wm_window_is_tiled(surf))
		return anx_wm_window_swap_dir(surf, 1, 0);
	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return ANX_ENOENT;

	tile_save(surf);
	top = ANX_WM_MENUBAR_H;
	anx_wm_window_set_geometry(surf, (int32_t)(fb->width / 2),
				   (int32_t)(top + ANX_WM_DECOR_H),
				   fb->width - fb->width / 2,
				   fb->height - top - ANX_WM_DECOR_H -
				   ANX_WM_TASKBAR_H);
	anx_wm_notify("Snapped right");
	return ANX_OK;
}

int anx_wm_window_float(struct anx_surface *surf)
{
	if (!surf)
		return ANX_EINVAL;
	if (!g_tile_saved.active || g_tile_saved.surf != surf)
		return ANX_OK;	/* nothing saved — not snapped */

	g_tile_saved.active = false;
	return anx_wm_window_set_geometry(surf, g_tile_saved.x, g_tile_saved.y,
					  g_tile_saved.w, g_tile_saved.h);
}

/* ------------------------------------------------------------------ */
/* Send window to workspace                                           */
/* ------------------------------------------------------------------ */

/* Focus the nearest window in a direction (sway "focus left" etc.). */
int anx_wm_window_focus_dir(int32_t dx, int32_t dy)
{
	struct anx_wm_workspace *ws = active_ws();
	struct anx_surface *cur = NULL, *best;

	anx_iface_surface_lookup(ws->focused, &cur);
	if (!cur)
		return ANX_ENOENT;
	best = wm_neighbour(ws, cur, dx, dy, false);
	if (!best)
		return ANX_ENOENT;
	return anx_wm_window_focus(best);
}

int anx_wm_window_send_to_workspace(struct anx_surface *surf, uint32_t ws_id)
{
	struct anx_wm_workspace *src, *dst;
	anx_oid_t heir = {0, 0};
	bool tile;

	if (!surf || ws_id < 1 || ws_id > ANX_WM_WORKSPACES)
		return ANX_EINVAL;

	src = ws_of(&surf->oid);
	dst = &g_workspaces[ws_id - 1];

	if (src == dst)
		return ANX_OK;

	if (dst->surf_count >= ANX_WM_WS_SURFS)
		return ANX_ENOMEM;

	/* A window keeps its tiled or floating state across workspaces. */
	tile = src ? anx_wm_tile_contains(&src->tiles, &surf->oid)
		   : anx_wm_tiling.enabled &&
		     !(surf->wm_flags & ANX_WM_SF_FLOATING);

	if (src) {
		anx_wm_tile_remove(&src->tiles, &surf->oid, &heir);
		if (oid_eq(&src->fullscreen, &surf->oid))
			src->fullscreen.hi = src->fullscreen.lo = 0;
		ws_remove(src, &surf->oid);
		if (oid_eq(&src->focused, &surf->oid))
			ws_set_focus(src, heir);
	}

	dst->surfs[dst->surf_count++] = surf->oid;
	if (tile)
		anx_wm_tile_insert(&dst->tiles, &surf->oid, &dst->focused);

	if (dst != active_ws()) {
		/* Hidden until that workspace is shown. */
		surf->state = ANX_SURF_MINIMIZED;
		dst->focused = surf->oid;
		ws_retile(dst);
		if (src)
			ws_retile(src);
		if (src == active_ws()) {
			struct anx_surface *next = NULL;

			if (!tile) {
				int32_t ox, oy;
				uint32_t ow, oh;

				surf_outer_rect(surf, &ox, &oy, &ow, &oh);
				anx_wm_expose(ox, oy, ow, oh);
			}
			if (anx_iface_surface_lookup(src->focused, &next) ==
			    ANX_OK && next)
				anx_wm_window_focus(next);
		}
	} else {
		surf->state = ANX_SURF_VISIBLE;
		if (tile) {
			anx_iface_surface_lower(surf);
			ws_retile(dst);
		} else {
			anx_iface_surface_raise(surf);
		}
		anx_wm_window_focus(surf);
	}

	anx_wm_menubar_refresh();
	return ANX_OK;
}

void anx_wm_focus_cycle(void)
{
	struct anx_wm_workspace *ws = active_ws();
	uint32_t i, start = 0;

	if (ws->surf_count == 0)
		return;

	/* Find current focus index */
	for (i = 0; i < ws->surf_count; i++) {
		if (oid_eq(&ws->surfs[i], &ws->focused)) {
			start = i;
			break;
		}
	}

	/* Advance to next visible (or minimized) surface */
	for (i = 1; i <= ws->surf_count; i++) {
		uint32_t idx = (start + i) % ws->surf_count;
		struct anx_surface *s = NULL;

		anx_iface_surface_lookup(ws->surfs[idx], &s);
		if (!s)
			continue;
		if (s->state == ANX_SURF_VISIBLE) {
			anx_wm_window_focus(s);
			return;
		}
		if (s->state == ANX_SURF_MINIMIZED) {
			anx_wm_window_restore(s);
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Pointer event routing                                               */
/* ------------------------------------------------------------------ */

/*
 * buttons: bitmask of held buttons (bit 0 = left).  0 = all released.
 * move_only: true when called from POINTER_MOVE (no button state change).
 */
static void wm_drag_begin(struct anx_surface *s, int32_t x, int32_t y)
{
	g_drag.surf   = s;
	g_drag.off_x  = x - s->x;
	g_drag.off_y  = y - s->y;
	g_drag.snap   = 0;
	g_drag.active = true;
	/* Keep a dragged tiled window visible above its neighbours. */
	if (anx_wm_window_is_tiled(s))
		anx_iface_surface_raise(s);
}

/*
 * End a drag of a tiled window: swap it with the tiled window under the
 * pointer (like dragging in Hyprland), or put it back where it was.
 */
static void wm_drop_tiled(struct anx_surface *ds, int32_t x, int32_t y)
{
	struct anx_wm_workspace *ws = ws_of(&ds->oid);
	uint32_t i;

	if (!ws)
		return;
	for (i = 0; i < ws->surf_count; i++) {
		struct anx_surface *s = NULL;
		int32_t sx, sy;
		uint32_t sw, sh;

		anx_iface_surface_lookup(ws->surfs[i], &s);
		if (!s || s == ds || s->state != ANX_SURF_VISIBLE ||
		    !anx_wm_tile_contains(&ws->tiles, &s->oid))
			continue;
		surf_outer_rect(s, &sx, &sy, &sw, &sh);
		if (x >= sx && x < sx + (int32_t)sw &&
		    y >= sy && y < sy + (int32_t)sh) {
			anx_wm_tile_swap(&ws->tiles, &ds->oid, &s->oid);
			break;
		}
	}
	anx_iface_surface_lower(ds);
	ws_retile(ws);
	anx_wm_window_focus(ds);
}

static void wm_handle_pointer(int32_t x, int32_t y,
			       uint32_t buttons, bool move_only)
{
	struct anx_surface *under;
	bool left_down  = (buttons & 1) != 0;
	bool right_down = (buttons & 2) != 0;

	cursor_erase();

	/* Power dialog: modal, takes every event while open */
	if (anx_wm_power_active()) {
		if (anx_wm_power_pointer(x, y, buttons, move_only)) {
			cursor_draw(x, y);
			return;
		}
	}

	/* Help overlay: any click dismisses it */
	if (anx_wm_help_active() && !move_only) {
		anx_wm_help_close();
		cursor_draw(x, y);
		return;
	}

	/* Context menu: forward all events while active */
	if (anx_wm_ctx_menu_active()) {
		if (anx_wm_ctx_menu_pointer(x, y, buttons, move_only)) {
			cursor_draw(x, y);
			return;
		}
	}

	/* Right-click: open context menu on the surface under cursor */
	if (right_down && !move_only && !g_drag.active && !g_resize.active) {
		struct anx_surface *rc = anx_iface_surface_at(x, y);

		if (!rc)
			rc = wm_surface_at_decor(x, y);
		if (rc) {
			anx_wm_window_focus(rc);
			anx_wm_ctx_menu_open(rc, x, y);
		} else {
			/* Desktop right-click: NULL target = desktop menu */
			anx_wm_ctx_menu_open(NULL, x, y);
		}
		cursor_draw(x, y);
		return;
	}

	/*
	 * Button released: end any active drag or resize. A move that shows
	 * the button up counts too. A touchpad can lift without a separate
	 * release report; the drag then stayed active, and the next click
	 * moved the window instead of focusing it.
	 */
	if (!left_down &&
	    (!move_only || g_drag.active || g_resize.active)) {
		if (g_drag.active) {
			struct anx_surface *ds = g_drag.surf;
			int snap = g_drag.snap;

			snap_preview_erase();

			g_drag.active = false;
			g_drag.surf   = NULL;
			g_drag.snap   = 0;

			if (ds && anx_wm_window_is_tiled(ds))
				wm_drop_tiled(ds, x, y);
			else if (ds && snap == 1)
				anx_wm_window_tile_left(ds);
			else if (ds && snap == 2)
				anx_wm_window_tile_right(ds);
		}
		if (g_resize.active) {
			g_resize.active = false;
			g_resize.surf   = NULL;
		}
		cursor_set(CURSOR_ARROW);
		cursor_draw(x, y);
		return;
	}

	/* Active resize: adjust surface dimensions */
	if (g_resize.active && g_resize.surf && left_down) {
		int32_t  dx = x - g_resize.start_x;
		int32_t  dy = y - g_resize.start_y;
		uint32_t nw = g_resize.orig_w;
		uint32_t nh = g_resize.orig_h;

		if (g_resize.edges & 1) {
			int32_t w = (int32_t)g_resize.orig_w + dx;
			nw = (uint32_t)(w < (int32_t)RESIZE_MIN_W
					? (int32_t)RESIZE_MIN_W : w);
		}
		if (g_resize.edges & 2) {
			int32_t h = (int32_t)g_resize.orig_h + dy;
			nh = (uint32_t)(h < (int32_t)RESIZE_MIN_H
					? (int32_t)RESIZE_MIN_H : h);
		}
		anx_wm_window_set_geometry(g_resize.surf, g_resize.surf->x,
					   g_resize.surf->y, nw, nh);
		cursor_set(CURSOR_RESIZE);
		cursor_draw(x, y);
		return;
	}

	/* Active drag: move surface; detect edge-snap zone */
	if (g_drag.active && g_drag.surf && left_down) {
		const struct anx_fb_info *fbinfo = anx_fb_get_info();
		int prev_snap = g_drag.snap;
		bool tiled = anx_wm_window_is_tiled(g_drag.surf);

		anx_wm_window_set_geometry(g_drag.surf,
					   x - g_drag.off_x,
					   y - g_drag.off_y,
					   g_drag.surf->width,
					   g_drag.surf->height);

		/* A tiled window is dropped onto another to swap; no snapping. */
		if (fbinfo && fbinfo->available && !tiled) {
			if (x < SNAP_ZONE)
				g_drag.snap = 1;
			else if (x >= (int32_t)fbinfo->width - SNAP_ZONE)
				g_drag.snap = 2;
			else
				g_drag.snap = 0;
		}

		if (g_drag.snap != prev_snap) {
			snap_preview_erase();
			if (g_drag.snap != 0) {
				snap_preview_draw(g_drag.snap);
				anx_wm_notify(g_drag.snap == 1
					      ? "Snap left — release to tile"
					      : "Snap right — release to tile");
			}
		}

		cursor_set(g_drag.snap ? CURSOR_RESIZE : CURSOR_MOVE);
		cursor_draw(x, y);
		return;
	}

	/* Menu bar: always on top, handle clicks, no drag */
	if (g_menubar && x >= g_menubar->x && y >= g_menubar->y &&
	    x < g_menubar->x + (int32_t)g_menubar->width &&
	    y < g_menubar->y + (int32_t)g_menubar->height) {
		if (left_down && !move_only) {
			int hit = anx_wm_menubar_hit(x - g_menubar->x, y - g_menubar->y);
			if (hit == -1) {
				struct anx_surface *focused = anx_wm_focused_window();
				anx_oid_t invocation = focused ? focused->oid : (anx_oid_t){0, 0};
				anx_wm_app_menu_open(2, invocation);
			} else if (hit == -2) {
				anx_wm_power_open();
			} else if (hit > 0) {
				anx_wm_workspace_switch((uint32_t)hit);
				anx_wm_menubar_refresh();
			}
		}
		cursor_draw(x, y);
		return;
	}

	/* Taskbar: restore minimized window on click */
	if (anx_wm_taskbar_pointer(x, y, buttons, move_only)) {
		cursor_draw(x, y);
		return;
	}

	under = anx_iface_surface_at(x, y);

	if (under && left_down && !move_only) {
		/* Tiled windows resize through the layout (Meta+Ctrl+arrows). */
		uint8_t edges = anx_wm_window_is_tiled(under) ? 0 :
				surf_resize_edges(under, x, y);

		if (edges) {
			/* Edge/corner resize */
			anx_wm_window_focus(under);
			g_resize.surf    = under;
			g_resize.start_x = x;
			g_resize.start_y = y;
			g_resize.orig_w  = under->width;
			g_resize.orig_h  = under->height;
			g_resize.edges   = edges;
			g_resize.active  = true;
		} else if (!under->title[0]) {
			/* Untitled surface: drag to move */
			anx_wm_window_focus(under);
			wm_drag_begin(under, x, y);
		} else {
			/* Titled surface: focus only (drag comes via decor area) */
			anx_wm_window_focus(under);
		}
	} else if (left_down && !move_only) {
		/* Click landed in a decoration area (not on the canvas) */
		struct anx_surface *decor = wm_surface_at_decor(x, y);

		if (decor) {
			enum anx_window_button btn = wm_decor_button_at(decor, x, y);

			if (btn == ANX_WINDOW_BUTTON_CLOSE) {
				anx_wm_window_close(decor);
			} else if (btn == ANX_WINDOW_BUTTON_MINIMIZE) {
				anx_wm_window_minimize(decor);
			} else if (btn == ANX_WINDOW_BUTTON_MAXIMIZE) {
				anx_wm_window_focus(decor);
				anx_wm_window_fullscreen_toggle(decor);
			} else {
				/* Double-click on titlebar → fullscreen toggle */
				bool dbl = (g_dblclick.surf == decor &&
					    g_wm_tick - g_dblclick.tick
					    <= DBLCLICK_WINDOW);

				anx_wm_window_focus(decor);
				if (dbl) {
					g_dblclick.surf = NULL;
					anx_wm_window_fullscreen_toggle(decor);
				} else {
					g_dblclick.surf = decor;
					g_dblclick.tick = g_wm_tick;
					wm_drag_begin(decor, x, y);
				}
			}
		}
	}

	/* Hover: update cursor shape without pressing a button */
	if (move_only || !left_down) {
		struct anx_surface *hov = anx_iface_surface_at(x, y);
		enum cursor_type shape = CURSOR_ARROW;

		if (hov && !anx_wm_window_is_tiled(hov) &&
		    surf_resize_edges(hov, x, y))
			shape = CURSOR_RESIZE;
		else if (g_drag.active)
			shape = CURSOR_MOVE;
		cursor_set(shape);
	}

	cursor_draw(x, y);
}

/* ------------------------------------------------------------------ */
/* Desktop event loop                                                  */
/* ------------------------------------------------------------------ */

void anx_wm_run(void)
{
	const struct anx_fb_info *fb = anx_fb_get_info();

	g_wm_running = true;


	/* Take framebuffer ownership: disable text console and clear screen */
	anx_fbcon_disable();

	/*
	 * Draw into RAM from here on. Shadows, transparency and the
	 * wallpaper all read back what is underneath, and reading the
	 * write-combining framebuffer for that would crawl.
	 */
	if (anx_fb_enable_backbuffer() != ANX_OK)
		kprintf("[wm] no back buffer: drawing straight to video memory\n");

	/* Paint desktop background */
	if (fb && fb->available) {
		anx_wm_desktop_paint(0, 0, fb->width, fb->height);
		cursor_draw((int32_t)(fb->width  / 2),
			    (int32_t)(fb->height / 2));
	}

	kprintf("[wm] desktop session started (workspace 1)\n");
	kprintf("[wm] keybindings:\n");
	kprintf("[wm]   Meta+1..9      switch workspace\n");
	kprintf("[wm]   Meta+Shift+1..9 send window to workspace\n");
	kprintf("[wm]   Meta+Enter     open terminal\n");
	kprintf("[wm]   Meta+Q, Meta+W close window\n");
	kprintf("[wm]   Meta+F         fullscreen toggle\n");
	kprintf("[wm]   Meta+Shift+F   toggle floating (Meta+T too)\n");
	kprintf("[wm]   Meta+arrows    focus left/right/up/down (or H/J/K/L)\n");
	kprintf("[wm]   Meta+Shift+arrows swap tiled window (floating: move)\n");
	kprintf("[wm]   Meta+Ctrl+arrows  resize\n");
	kprintf("[wm]   Meta+[ / ]     swap left/right (floating: snap to half)\n");
	kprintf("[wm]   Meta+Tab       cycle window focus\n");
	kprintf("[wm]   Meta+Space     command search\n");
	kprintf("[wm]   Meta+Shift+W   workflow designer\n");
	kprintf("[wm]   Meta+O         object viewer\n");
	kprintf("[wm]   Meta+M         minimize window\n");
	kprintf("[wm]   Meta+Esc       power: restart or halt\n");
	kprintf("[wm]   Ctrl+Alt+Del   power: restart or halt\n");

	/* Open agent surface as primary boot interface */
	anx_wm_agent_open();

	while (g_wm_running) {
		struct anx_event ev;

		/* Put the cursor back if a commit painted over it. */
		cursor_refresh();

		/* One copy to video memory for everything drawn since. */
		anx_fb_flush();

		/* Poll WM-targeted events (null target_surf) from the event ring */
		if (anx_iface_event_poll_wm(&ev) == ANX_OK) {
			switch (ev.type) {
			case ANX_EVENT_KEY_DOWN:
				/* The power dialog is modal while it is open. */
				if (anx_wm_power_active()) {
					anx_wm_power_key(ev.data.key.keycode);
					break;
				}
				/* F1 toggles the help overlay; Esc closes it. */
				if (ev.data.key.keycode == ANX_KEY_F1)
					anx_wm_help_toggle();
				else if (anx_wm_help_active())
					anx_wm_help_key(ev.data.key.keycode);
				break;

			case ANX_EVENT_POINTER_MOVE:
				wm_handle_pointer(ev.data.pointer.x,
						  ev.data.pointer.y,
						  ev.data.pointer.buttons,
						  true);
				break;

			case ANX_EVENT_POINTER_BUTTON:
				wm_handle_pointer(ev.data.pointer.x,
						  ev.data.pointer.y,
						  ev.data.pointer.buttons,
						  false);
				break;

			case ANX_EVENT_POINTER_SCROLL:
				{
					/* Route scroll to terminal if focused */
					struct anx_surface *ts = anx_wm_terminal_surface();
					anx_oid_t foc = anx_input_focus_get();
					if (ts && ts->oid.hi == foc.hi &&
					    ts->oid.lo == foc.lo) {
						int32_t delta = (int32_t)ev.data.pointer.buttons;
						uint32_t key = (delta > 0)
							? ANX_KEY_PAGEUP
							: ANX_KEY_PAGEDOWN;
						anx_wm_terminal_key_event(key, 0, 0);
					}
				}
				break;

			default:
				break;
			}
		}

		/* Flush pixel buffers if key events marked them dirty */
		anx_wm_agent_flush_if_dirty();
		anx_wm_terminal_flush_if_dirty();

		/* Dispatch surface-targeted events to the focused surface handler */
		{
			struct anx_wm_workspace *ws = active_ws();
			struct anx_surface *focused = NULL;

			if (!oid_null(&ws->focused))
				anx_iface_surface_lookup(ws->focused, &focused);

			if (focused && focused->on_event) {
				struct anx_event sev;

				while (anx_iface_event_poll_surf(focused->oid, &sev) == ANX_OK)
					focused->on_event(focused, &sev);
			}
		}

		/* Poll USB and I2C input (both drivers have no interrupts) */
		anx_xhci_poll();
		anx_i2c_input_poll();

		/* Poll network stack — keeps HTTP/SSH/TCP alive in desktop mode */
		anx_e1000_poll();
		anx_mt7925_poll();
		anx_net_poll();
		anx_httpd_poll();
		anx_sshd_poll();
		anx_browser_cell_tick();

		/* Refresh menu bar (includes clock) every ~60 polls */
		g_wm_tick++;
		if (g_wm_tick % 60 == 0)
			anx_wm_menubar_refresh();

		/* Auto-dismiss expired toast notifications */
		if (g_toast.surf) {
			g_toast.age++;
			if (g_toast.age >= TOAST_LIFE)
				toast_dismiss();
		}

		/* Yield to reduce CPU pressure on bare metal */
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause");
#else
		__asm__ volatile("yield");
#endif
	}
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

int anx_wm_init(void)
{
	uint32_t i;

	anx_spin_init(&g_wm_lock);
	anx_memset(g_workspaces, 0, sizeof(g_workspaces));

	for (i = 0; i < ANX_WM_WORKSPACES; i++) {
		g_workspaces[i].id = i + 1;
		anx_wm_tile_init(&g_workspaces[i].tiles);
	}

	g_active_ws  = 0;
	g_menubar    = NULL;
	g_wm_running = false;

	anx_wm_hotkeys_init();
	anx_wm_menubar_create();
	anx_wm_taskbar_create();

	kprintf("[wm] initialized (%u workspaces)\n", ANX_WM_WORKSPACES);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* WiFi state hooks (override weak symbols from mt7925 driver)        */
/* ------------------------------------------------------------------ */

void mt7925_on_connect(const char *ssid)
{
	(void)ssid;
	anx_wm_notify("WiFi connected");
	anx_wm_menubar_refresh();
}

void mt7925_on_disconnect(void)
{
	anx_wm_notify("WiFi disconnected");
	anx_wm_menubar_refresh();
}
