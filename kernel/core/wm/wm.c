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
#include <anx/gui.h>
#include <anx/font.h>
#include <anx/theme.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/spinlock.h>
#include <anx/arch.h>

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

static struct anx_wm_workspace g_workspaces[ANX_WM_WORKSPACES];
static uint32_t                g_active_ws;	/* 0-based index */
struct anx_surface            *g_menubar;	/* always-on-top bar (used by wm_menubar.c) */
uint32_t                      *g_menubar_pixels;
static bool                    g_wm_running;
static struct anx_spinlock     g_wm_lock;

/* Saved pre-fullscreen bounds for a surface */
static struct {
	struct anx_surface *surf;
	int32_t  x, y;
	uint32_t w, h;
	bool     active;
} g_fs_saved;

/* Drag-to-move state */
static struct {
	struct anx_surface *surf;
	int32_t             off_x;  /* cursor x minus surface x at drag start */
	int32_t             off_y;
	bool                active;
} g_drag;

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
	uint32_t           *pixels;
	uint32_t            age;	/* polls since creation */
} g_toast;

static void toast_dismiss(void)
{
	if (!g_toast.surf)
		return;
	anx_iface_surface_destroy(g_toast.surf);
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
	const char *p;
	uint32_t x;

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

	/* Draw message text at (8, (TOAST_H - ANX_FONT_HEIGHT)/2) */
	{
		uint32_t ty = (TOAST_H - ANX_FONT_HEIGHT) / 2;
		const uint16_t *glyph;
		uint32_t row, col;

		x = 8;
		for (p = msg; *p && x + ANX_FONT_WIDTH <= TOAST_W - 8; p++) {
			glyph = anx_font_glyph(*p);
			for (row = 0; row < ANX_FONT_HEIGHT; row++) {
				uint16_t bits = glyph[row];
				for (col = 0; col < ANX_FONT_WIDTH; col++) {
					uint32_t px = (ty + row) * TOAST_W + x + col;
					g_toast.pixels[px] =
						(bits & (0x800u >> col)) ? fg : bg;
				}
			}
			x += ANX_FONT_WIDTH;
		}
	}

	g_toast.age = 0;
	anx_iface_surface_map(g_toast.surf);
	anx_iface_surface_raise(g_toast.surf);
	anx_iface_surface_commit(g_toast.surf);
}

/* ------------------------------------------------------------------ */
/* Mouse cursor                                                        */
/* ------------------------------------------------------------------ */

#define CURSOR_W  10
#define CURSOR_H  11

/*
 * 0 = transparent, 1 = white (#FFFFFF), 2 = black outline (#000000).
 * Shape: filled triangle pointing upper-left, hotspot at (0,0).
 */
static const uint8_t cursor_px[CURSOR_H][CURSOR_W] = {
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

static int32_t  g_cur_x  = -1;
static int32_t  g_cur_y  = -1;
static bool     g_cur_on = false;
static uint32_t g_cur_saved[CURSOR_H][CURSOR_W];

static void cursor_erase(void)
{
	const struct anx_fb_info *fb;
	uint32_t r, c;

	if (!g_cur_on)
		return;
	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return;

	for (r = 0; r < CURSOR_H; r++) {
		int32_t py = g_cur_y + (int32_t)r;
		if (py < 0 || (uint32_t)py >= fb->height)
			continue;
		for (c = 0; c < CURSOR_W; c++) {
			int32_t px = g_cur_x + (int32_t)c;
			if (px < 0 || (uint32_t)px >= fb->width)
				continue;
			if (cursor_px[r][c] != 0)
				anx_fb_row_ptr((uint32_t)py)[px] =
					g_cur_saved[r][c];
		}
	}
	g_cur_on = false;
}

static void cursor_draw(int32_t x, int32_t y)
{
	const struct anx_fb_info *fb;
	uint32_t r, c;

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return;

	g_cur_x = x;
	g_cur_y = y;

	for (r = 0; r < CURSOR_H; r++) {
		int32_t py = y + (int32_t)r;
		if (py < 0 || (uint32_t)py >= fb->height)
			continue;
		for (c = 0; c < CURSOR_W; c++) {
			int32_t px = x + (int32_t)c;
			if (px < 0 || (uint32_t)px >= fb->width)
				continue;
			if (cursor_px[r][c] != 0) {
				uint32_t *row = anx_fb_row_ptr((uint32_t)py);
				g_cur_saved[r][c] = row[px];
				row[px] = (cursor_px[r][c] == 1)
					  ? 0xFFFFFF : 0x000000;
			}
		}
	}
	g_cur_on = true;
}

/* ------------------------------------------------------------------ */
/* Workspace helpers                                                   */
/* ------------------------------------------------------------------ */

static struct anx_wm_workspace *active_ws(void)
{
	return &g_workspaces[g_active_ws];
}

static bool oid_null(const anx_oid_t *o)
{
	return o->hi == 0 && o->lo == 0;
}

static bool oid_eq(const anx_oid_t *a, const anx_oid_t *b)
{
	return a->hi == b->hi && a->lo == b->lo;
}

/* True if (x,y) falls on the close button of surf's decoration. */
static bool wm_decor_close_hit(struct anx_surface *surf, int32_t x, int32_t y)
{
	uint32_t btn, bx, by;

	if (!surf || !surf->title[0] || surf->y < (int32_t)ANX_WM_DECOR_H)
		return false;
	btn = ANX_WM_DECOR_H - 4;
	bx  = (uint32_t)(surf->x) + surf->width - btn - 2;
	by  = (uint32_t)(surf->y) - ANX_WM_DECOR_H + 2;
	return (uint32_t)x >= bx && (uint32_t)x < bx + btn &&
	       (uint32_t)y >= by && (uint32_t)y < by + btn;
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
			ws->surf_count--;
			if (i < ws->surf_count)
				ws->surfs[i] = ws->surfs[ws->surf_count];
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

	/* Minimize all surfaces on the old workspace */
	for (i = 0; i < old_ws->surf_count; i++) {
		struct anx_surface *s = NULL;
		anx_iface_surface_lookup(old_ws->surfs[i], &s);
		if (s && s->state == ANX_SURF_VISIBLE)
			s->state = ANX_SURF_MINIMIZED;
	}

	g_active_ws = ws_id - 1;

	/* Restore surfaces on the new workspace */
	for (i = 0; i < new_ws->surf_count; i++) {
		struct anx_surface *s = NULL;
		anx_iface_surface_lookup(new_ws->surfs[i], &s);
		if (s && s->state == ANX_SURF_MINIMIZED)
			s->state = ANX_SURF_VISIBLE;
	}

	/* Restore focus on new workspace */
	if (!oid_null(&new_ws->focused)) {
		anx_input_focus_set(new_ws->focused);
	} else {
		anx_input_focus_set(null_oid);
	}

	anx_wm_menubar_refresh();
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

/* ------------------------------------------------------------------ */
/* Window lifecycle                                                    */
/* ------------------------------------------------------------------ */

int anx_wm_window_open(struct anx_surface *surf)
{
	struct anx_wm_workspace *ws;

	if (!surf)
		return ANX_EINVAL;

	ws = active_ws();
	if (ws->surf_count >= ANX_WM_WS_SURFS)
		return ANX_ENOMEM;

	ws->surfs[ws->surf_count++] = surf->oid;
	anx_iface_surface_raise(surf);
	anx_input_focus_set(surf->oid);
	ws->focused = surf->oid;
	anx_wm_menubar_refresh();
	return ANX_OK;
}

int anx_wm_window_close(struct anx_surface *surf)
{
	struct anx_wm_workspace *ws;
	anx_oid_t null_oid = {0, 0};

	if (!surf)
		return ANX_EINVAL;

	ws = ws_of(&surf->oid);
	if (ws) {
		ws_remove(ws, &surf->oid);

		/* If this was focused, focus the previous surface */
		if (oid_eq(&ws->focused, &surf->oid)) {
			if (ws->surf_count > 0) {
				ws->focused = ws->surfs[ws->surf_count - 1];
				anx_input_focus_set(ws->focused);
			} else {
				ws->focused = null_oid;
				anx_input_focus_set(null_oid);
			}
		}
	}

	anx_iface_surface_destroy(surf);
	anx_wm_menubar_refresh();
	return ANX_OK;
}

int anx_wm_window_focus(struct anx_surface *surf)
{
	struct anx_wm_workspace *ws;

	if (!surf)
		return ANX_EINVAL;

	ws = ws_of(&surf->oid);
	if (!ws)
		ws = active_ws();

	anx_iface_surface_raise(surf);
	anx_input_focus_set(surf->oid);
	ws->focused = surf->oid;

	/* Record last-used timestamp for the switcher. */
	anx_wm_activity_touch(surf->oid);

	/* Raise menubar above the newly focused surface and refresh title */
	if (g_menubar) {
		anx_iface_surface_raise(g_menubar);
		anx_wm_menubar_refresh();
	}

	return ANX_OK;
}

int anx_wm_window_minimize(struct anx_surface *surf)
{
	anx_oid_t null_oid = {0, 0};

	if (!surf)
		return ANX_EINVAL;

	surf->state = ANX_SURF_MINIMIZED;

	/* Shift focus to previous window */
	{
		struct anx_wm_workspace *ws = ws_of(&surf->oid);

		if (ws && oid_eq(&ws->focused, &surf->oid)) {
			uint32_t i;
			anx_oid_t prev = null_oid;

			for (i = 0; i < ws->surf_count; i++) {
				if (!oid_eq(&ws->surfs[i], &surf->oid)) {
					struct anx_surface *s = NULL;
					anx_iface_surface_lookup(ws->surfs[i], &s);
					if (s && s->state == ANX_SURF_VISIBLE)
						prev = ws->surfs[i];
				}
			}
			ws->focused = prev;
			anx_input_focus_set(prev);
		}
	}
	return ANX_OK;
}

int anx_wm_window_fullscreen_toggle(struct anx_surface *surf)
{
	const struct anx_fb_info *fb;

	if (!surf)
		return ANX_EINVAL;

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return ANX_ENOENT;

	if (g_fs_saved.active && g_fs_saved.surf == surf) {
		/* Restore */
		anx_iface_surface_move(surf, g_fs_saved.x, g_fs_saved.y);
		surf->width  = g_fs_saved.w;
		surf->height = g_fs_saved.h;
		g_fs_saved.active = false;
	} else {
		/* Save and go fullscreen */
		g_fs_saved.surf   = surf;
		g_fs_saved.x      = surf->x;
		g_fs_saved.y      = surf->y;
		g_fs_saved.w      = surf->width;
		g_fs_saved.h      = surf->height;
		g_fs_saved.active = true;
		anx_iface_surface_move(surf, 0, 0);
		surf->width  = fb->width;
		surf->height = fb->height;
	}
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Tiling                                                             */
/* ------------------------------------------------------------------ */

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

int anx_wm_window_tile_left(struct anx_surface *surf)
{
	const struct anx_fb_info *fb;
	uint32_t top;

	if (!surf)
		return ANX_EINVAL;
	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return ANX_ENOENT;

	tile_save(surf);
	top = ANX_WM_MENUBAR_H;
	anx_iface_surface_move(surf, 0, (int32_t)(top + ANX_WM_DECOR_H));
	surf->width  = fb->width / 2;
	surf->height = fb->height - top - ANX_WM_DECOR_H;
	anx_iface_surface_commit(surf);
	anx_wm_notify("Tiled left");
	return ANX_OK;
}

int anx_wm_window_tile_right(struct anx_surface *surf)
{
	const struct anx_fb_info *fb;
	uint32_t top;

	if (!surf)
		return ANX_EINVAL;
	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return ANX_ENOENT;

	tile_save(surf);
	top = ANX_WM_MENUBAR_H;
	anx_iface_surface_move(surf,
			       (int32_t)(fb->width / 2),
			       (int32_t)(top + ANX_WM_DECOR_H));
	surf->width  = fb->width - fb->width / 2;
	surf->height = fb->height - top - ANX_WM_DECOR_H;
	anx_iface_surface_commit(surf);
	anx_wm_notify("Tiled right");
	return ANX_OK;
}

int anx_wm_window_float(struct anx_surface *surf)
{
	if (!surf)
		return ANX_EINVAL;
	if (!g_tile_saved.active || g_tile_saved.surf != surf)
		return ANX_OK;	/* nothing saved — already floating */

	anx_iface_surface_move(surf, g_tile_saved.x, g_tile_saved.y);
	surf->width  = g_tile_saved.w;
	surf->height = g_tile_saved.h;
	g_tile_saved.active = false;
	anx_iface_surface_commit(surf);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Send window to workspace                                           */
/* ------------------------------------------------------------------ */

int anx_wm_window_send_to_workspace(struct anx_surface *surf, uint32_t ws_id)
{
	struct anx_wm_workspace *src, *dst;
	anx_oid_t null_oid = {0, 0};

	if (!surf || ws_id < 1 || ws_id > ANX_WM_WORKSPACES)
		return ANX_EINVAL;

	src = ws_of(&surf->oid);
	dst = &g_workspaces[ws_id - 1];

	if (src == dst)
		return ANX_OK;

	if (dst->surf_count >= ANX_WM_WS_SURFS)
		return ANX_ENOMEM;

	if (src) {
		ws_remove(src, &surf->oid);
		if (oid_eq(&src->focused, &surf->oid)) {
			if (src->surf_count > 0) {
				src->focused = src->surfs[src->surf_count - 1];
				if (src == active_ws())
					anx_input_focus_set(src->focused);
			} else {
				src->focused = null_oid;
				if (src == active_ws())
					anx_input_focus_set(null_oid);
			}
		}
	}

	dst->surfs[dst->surf_count++] = surf->oid;

	/* If sending to the inactive workspace, hide the surface */
	if (dst != active_ws()) {
		surf->state = ANX_SURF_MINIMIZED;
	} else {
		surf->state = ANX_SURF_VISIBLE;
		anx_iface_surface_raise(surf);
		anx_input_focus_set(surf->oid);
		dst->focused = surf->oid;
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

	/* Advance to next visible surface */
	for (i = 1; i <= ws->surf_count; i++) {
		uint32_t idx = (start + i) % ws->surf_count;
		struct anx_surface *s = NULL;

		anx_iface_surface_lookup(ws->surfs[idx], &s);
		if (s && s->state == ANX_SURF_VISIBLE) {
			anx_wm_window_focus(s);
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
static void wm_handle_pointer(int32_t x, int32_t y,
			       uint32_t buttons, bool move_only)
{
	struct anx_surface *under;
	bool left_down = (buttons & 1) != 0;

	cursor_erase();

	/* Button released: end any active drag or resize */
	if (!left_down && !move_only) {
		if (g_drag.active) {
			g_drag.active = false;
			g_drag.surf   = NULL;
		}
		if (g_resize.active) {
			g_resize.active = false;
			g_resize.surf   = NULL;
		}
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
		g_resize.surf->width  = nw;
		g_resize.surf->height = nh;
		anx_iface_surface_commit(g_resize.surf);
		cursor_draw(x, y);
		return;
	}

	/* Active drag: move surface */
	if (g_drag.active && g_drag.surf && left_down) {
		anx_iface_surface_move(g_drag.surf,
				       x - g_drag.off_x,
				       y - g_drag.off_y);
		anx_iface_surface_commit(g_drag.surf);
		cursor_draw(x, y);
		return;
	}

	/* Menu bar: always on top, handle clicks, no drag */
	if (g_menubar && x >= g_menubar->x && y >= g_menubar->y &&
	    x < g_menubar->x + (int32_t)g_menubar->width &&
	    y < g_menubar->y + (int32_t)g_menubar->height) {
		if (left_down && !move_only) {
			uint32_t ws;
			int32_t  dot_y = ANX_WM_MENUBAR_H / 2;

			/* Workspace dots: centers at x = 16 + (ws-1)*20 */
			for (ws = 1; ws <= ANX_WM_WORKSPACES; ws++) {
				int32_t dot_x = (int32_t)(16 + (ws - 1) * 20);

				if (x >= dot_x - 7 && x <= dot_x + 7 &&
				    y >= dot_y - 7 && y <= dot_y + 7) {
					anx_wm_workspace_switch(ws);
					anx_wm_menubar_refresh();
					cursor_draw(x, y);
					return;
				}
			}

			/* Power button: rightmost 24px of menubar → halt */
			if (x >= (int32_t)g_menubar->width - 24) {
				kprintf("[wm] power button clicked — halting\n");
				arch_halt();
			}
		}
		cursor_draw(x, y);
		return;
	}

	under = anx_iface_surface_at(x, y);

	if (under && left_down && !move_only) {
		uint8_t edges = surf_resize_edges(under, x, y);

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
			g_drag.surf   = under;
			g_drag.off_x  = x - under->x;
			g_drag.off_y  = y - under->y;
			g_drag.active = true;
		} else {
			/* Titled surface: focus only (drag comes via decor area) */
			anx_wm_window_focus(under);
		}
	} else if (left_down && !move_only) {
		/* Click landed in a decoration area (not on the canvas) */
		struct anx_surface *decor = wm_surface_at_decor(x, y);

		if (decor) {
			if (wm_decor_close_hit(decor, x, y)) {
				/* Close button */
				anx_wm_window_close(decor);
			} else {
				anx_wm_window_focus(decor);
				g_drag.surf   = decor;
				g_drag.off_x  = x - decor->x;
				g_drag.off_y  = y - decor->y;
				g_drag.active = true;
			}
		}
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

	/* Erase the pre-WM GUI framebuffer content (boot splash, old terminal
	 * frame) so the desktop starts with a clean background. */
	if (fb && fb->available) {
		anx_fb_fill_rect(0, 0, fb->width, fb->height,
				 0x000B1A2B /* ANX_COLOR_AX_BG */);
		cursor_draw((int32_t)(fb->width  / 2),
			    (int32_t)(fb->height / 2));
	}

	kprintf("[wm] desktop session started (workspace 1)\n");
	kprintf("[wm] keybindings:\n");
	kprintf("[wm]   Meta+1..9      switch workspace\n");
	kprintf("[wm]   Meta+Shift+1..9 send window to workspace\n");
	kprintf("[wm]   Meta+Enter     open terminal\n");
	kprintf("[wm]   Meta+Q         close window\n");
	kprintf("[wm]   Meta+F         fullscreen toggle\n");
	kprintf("[wm]   Meta+[         tile left\n");
	kprintf("[wm]   Meta+]         tile right\n");
	kprintf("[wm]   Meta+Shift+F   float (restore from tile)\n");
	kprintf("[wm]   Meta+Tab       cycle window focus\n");
	kprintf("[wm]   Meta+Space     command search\n");
	kprintf("[wm]   Meta+W         workflow designer\n");
	kprintf("[wm]   Meta+O         object viewer\n");
	kprintf("[wm]   Meta+M         minimize window\n");
	kprintf("[wm]   Meta+Shift+H   halt system\n");

	while (g_wm_running) {
		struct anx_event ev;

		/* Poll WM-targeted events (null target_surf) from the event ring */
		if (anx_iface_event_poll_wm(&ev) == ANX_OK) {
			switch (ev.type) {
			case ANX_EVENT_KEY_DOWN:
				/* Hotkeys are already intercepted in input.c;
				 * remaining events have passed the WM filter. */
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

			default:
				break;
			}
		}

		/* Refresh menu bar (includes clock) every ~60 polls */
		{
			static uint32_t tick;
			tick++;
			if (tick >= 60) {
				tick = 0;
				anx_wm_menubar_refresh();
			}
		}

		/* Auto-dismiss expired toast notifications */
		if (g_toast.surf) {
			g_toast.age++;
			if (g_toast.age >= TOAST_LIFE)
				toast_dismiss();
		}
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

	for (i = 0; i < ANX_WM_WORKSPACES; i++)
		g_workspaces[i].id = i + 1;

	g_active_ws  = 0;
	g_menubar    = NULL;
	g_wm_running = false;

	anx_wm_hotkeys_init();
	anx_wm_menubar_create();

	kprintf("[wm] initialized (%u workspaces)\n", ANX_WM_WORKSPACES);
	return ANX_OK;
}
