/*
 * test_wm_hotkey.c — global hotkey matching and window lifecycle helpers.
 *
 * Lock states toggle and stay on, so a chord must match with CapsLock or
 * NumLock set. Held modifiers must still match exactly, or Meta+Shift+W
 * would fire the Meta+W binding.
 */

#include <anx/types.h>
#include <anx/input.h>
#include <anx/wm.h>
#include <anx/interface_plane.h>
#include <anx/alloc.h>
#include <anx/string.h>

#define ASSERT(cond, code) do { if (!(cond)) return (code); } while (0)

static uint32_t g_plain_hits;
static uint32_t g_shift_hits;

static void count_plain(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	g_plain_hits++;
}

static void count_shift(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	g_shift_hits++;
}

int test_wm_hotkey(void)
{
	anx_wm_hotkeys_init();
	g_plain_hits = 0;
	g_shift_hits = 0;

	ASSERT(anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_F12,
				      count_plain, NULL) == ANX_OK, -1);
	ASSERT(anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT,
				      ANX_KEY_F12, count_shift,
				      NULL) == ANX_OK, -2);

	/* Plain chord. */
	ASSERT(anx_wm_hotkey_dispatch(ANX_MOD_META, ANX_KEY_F12), -3);
	ASSERT(g_plain_hits == 1 && g_shift_hits == 0, -4);

	/* Lock states do not change which binding fires. */
	ASSERT(anx_wm_hotkey_dispatch(ANX_MOD_META | ANX_MOD_CAPSLOCK,
				      ANX_KEY_F12), -5);
	ASSERT(anx_wm_hotkey_dispatch(ANX_MOD_META | ANX_MOD_LOCKS,
				      ANX_KEY_F12), -6);
	ASSERT(g_plain_hits == 3 && g_shift_hits == 0, -7);

	/* An extra held modifier selects the other binding, locks or not. */
	ASSERT(anx_wm_hotkey_dispatch(ANX_MOD_META | ANX_MOD_SHIFT |
				      ANX_MOD_CAPSLOCK, ANX_KEY_F12), -8);
	ASSERT(g_plain_hits == 3 && g_shift_hits == 1, -9);

	/* Held modifiers still need an exact match. */
	ASSERT(!anx_wm_hotkey_dispatch(ANX_MOD_META | ANX_MOD_CTRL,
				       ANX_KEY_F12), -10);
	ASSERT(!anx_wm_hotkey_dispatch(ANX_MOD_CAPSLOCK, ANX_KEY_F12), -11);
	ASSERT(!anx_wm_hotkey_dispatch(0, ANX_KEY_F12), -12);
	ASSERT(g_plain_hits == 3 && g_shift_hits == 1, -13);

	/* The real defaults register cleanly with the lock-aware matcher. */
	anx_wm_hotkeys_init();
	return 0;
}

/*
 * Window lifecycle helpers. A window closed by the WM (close button,
 * Meta+Q) must still release the app's buffers, exactly once, and every
 * window must fit the per-window pixel budget on a 2560x1600 panel.
 */
static uint32_t g_destroyed;

static void count_destroy(struct anx_surface *surf)
{
	g_destroyed++;
	anx_wm_canvas_free(surf);
}

int test_wm_window(void)
{
	struct anx_content_node *cn;
	struct anx_surface *surf = NULL;
	uint32_t w = 2560, h = 1600 - ANX_WM_MENUBAR_H;

	/* Fit shrinks a full-screen request and keeps its shape. */
	anx_wm_window_fit(&w, &h);
	ASSERT((uint64_t)w * h * 4 <= ANX_WM_WINDOW_BYTES_MAX, -1);
	ASSERT(w > h && w >= 1000, -2);

	/* A request that already fits is left alone. */
	w = 640;
	h = 480;
	anx_wm_window_fit(&w, &h);
	ASSERT(w == 640 && h == 480, -3);

	ASSERT(anx_iface_init() == ANX_OK, -4);
	cn = anx_alloc(sizeof(*cn));
	ASSERT(cn != NULL, -5);
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = anx_alloc(64 * 4);
	cn->data_len = 64 * 4;
	ASSERT(cn->data != NULL, -6);
	ASSERT(anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS, cn,
					100, 100, 8, 8, &surf) == ANX_OK, -7);
	surf->on_destroy = count_destroy;
	g_destroyed = 0;

	ASSERT(anx_wm_window_open(surf) == ANX_OK, -8);
	ASSERT(anx_wm_window_close(surf) == ANX_OK, -9);
	ASSERT(g_destroyed == 1, -10);
	ASSERT(surf->on_destroy == NULL, -11);
	ASSERT(surf->content_root == NULL, -12);
	return 0;
}

/*
 * Dwindle layout. Area 1000x600 with gaps_out 10, gaps_in 5 (10 between
 * windows): one window fills the inset area; a second splits it side by
 * side; a third splits the second (a tall box) top and bottom.
 */
static int rect_is(const struct anx_wm_rect *r, int32_t x, int32_t y,
		   uint32_t w, uint32_t h)
{
	return r->x == x && r->y == y && r->w == w && r->h == h;
}

int test_wm_tile(void)
{
	struct anx_wm_tiling_config cfg = anx_wm_tiling;
	struct anx_wm_tile_tree t;
	struct anx_wm_rect area = { 0, 34, 1000, 600 };
	struct anx_wm_rect box[8];
	anx_oid_t ids[8], a = { 0, 1 }, b = { 0, 2 }, c = { 0, 3 };
	anx_oid_t heir;
	uint32_t n;

	cfg.gaps_in = 5;
	cfg.gaps_out = 10;
	anx_wm_tile_init(&t);
	ASSERT(anx_wm_tile_layout(&t, &area, &cfg, ids, box, 8) == 0, -1);

	ASSERT(anx_wm_tile_insert(&t, &a, NULL) == ANX_OK, -2);
	ASSERT(anx_wm_tile_insert(&t, &a, NULL) == ANX_EEXIST, -3);
	n = anx_wm_tile_layout(&t, &area, &cfg, ids, box, 8);
	ASSERT(n == 1 && rect_is(&box[0], 10, 44, 980, 580), -4);

	/* Wider than tall: side by side, 10px apart, newcomer on the right. */
	ASSERT(anx_wm_tile_insert(&t, &b, &a) == ANX_OK, -5);
	n = anx_wm_tile_layout(&t, &area, &cfg, ids, box, 8);
	ASSERT(n == 2, -6);
	ASSERT(ids[0].lo == 1 && rect_is(&box[0], 10, 44, 485, 580), -7);
	ASSERT(ids[1].lo == 2 && rect_is(&box[1], 505, 44, 485, 580), -8);

	/* b's box is taller than wide: c goes below it. */
	ASSERT(anx_wm_tile_insert(&t, &c, &b) == ANX_OK, -9);
	n = anx_wm_tile_layout(&t, &area, &cfg, ids, box, 8);
	ASSERT(n == 3, -10);
	ASSERT(ids[1].lo == 2 && rect_is(&box[1], 505, 44, 485, 285), -11);
	ASSERT(ids[2].lo == 3 && rect_is(&box[2], 505, 339, 485, 285), -12);

	/* Swap exchanges places, not sizes. */
	ASSERT(anx_wm_tile_swap(&t, &a, &c) == ANX_OK, -13);
	n = anx_wm_tile_layout(&t, &area, &cfg, ids, box, 8);
	ASSERT(ids[0].lo == 3 && ids[2].lo == 1, -14);
	ASSERT(anx_wm_tile_swap(&t, &a, &c) == ANX_OK, -15);

	/* Growing a's width moves the root split right (sway resize). */
	ASSERT(anx_wm_tile_resize(&t, &a, false, 100) == ANX_OK, -16);
	n = anx_wm_tile_layout(&t, &area, &cfg, ids, box, 8);
	ASSERT(box[0].w == 582 && box[1].x == 602, -17);
	/* No split divides a vertically, so that resize has nothing to move. */
	ASSERT(anx_wm_tile_resize(&t, &a, true, 100) == ANX_ENOENT, -18);
	ASSERT(anx_wm_tile_resize(&t, &a, false, -100) == ANX_OK, -19);

	/* Closing b: c takes b's whole box and is the heir to focus. */
	ASSERT(anx_wm_tile_remove(&t, &b, &heir) == ANX_OK, -20);
	ASSERT(heir.lo == 3, -21);
	n = anx_wm_tile_layout(&t, &area, &cfg, ids, box, 8);
	ASSERT(n == 2 && ids[1].lo == 3 &&
	       rect_is(&box[1], 505, 44, 485, 580), -22);
	ASSERT(anx_wm_tile_remove(&t, &b, NULL) == ANX_ENOENT, -23);

	/* Removing the rest empties the tree; it can be reused. */
	ASSERT(anx_wm_tile_remove(&t, &a, &heir) == ANX_OK && heir.lo == 3,
	       -24);
	ASSERT(anx_wm_tile_remove(&t, &c, &heir) == ANX_OK &&
	       heir.lo == 0 && heir.hi == 0, -25);
	ASSERT(anx_wm_tile_layout(&t, &area, &cfg, ids, box, 8) == 0, -26);
	ASSERT(anx_wm_tile_insert(&t, &b, &a) == ANX_OK, -27);
	ASSERT(anx_wm_tile_contains(&t, &b) && !anx_wm_tile_contains(&t, &a),
	       -28);

	/* A zeroed tree (never initialised) behaves as empty. */
	anx_memset(&t, 0, sizeof(t));
	ASSERT(anx_wm_tile_insert(&t, &a, &b) == ANX_OK, -29);
	ASSERT(anx_wm_tile_layout(&t, &area, &cfg, ids, box, 8) == 1, -30);
	return 0;
}

/*
 * Keyboard focus. The WM raises the menu bar and taskbar after each
 * focus change; raising must not move the keyboard to them, or typed
 * keys and Meta+Q stop reaching the window the user clicked.
 */
static struct anx_surface *make_surface(const char *title)
{
	struct anx_content_node *cn = anx_alloc(sizeof(*cn));
	struct anx_surface *s = NULL;

	if (!cn)
		return NULL;
	anx_memset(cn, 0, sizeof(*cn));
	cn->type = ANX_CONTENT_CANVAS;
	cn->data = anx_alloc(8 * 8 * 4);
	cn->data_len = 8 * 8 * 4;
	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS, cn,
				     100, 100, 8, 8, &s) != ANX_OK)
		return NULL;
	s->on_destroy = count_destroy;
	if (title)
		anx_iface_surface_set_title(s, title);
	anx_iface_surface_map(s);
	return s;
}

int test_wm_focus(void)
{
	struct anx_surface *panel, *w1, *w2;
	anx_oid_t f;

	ASSERT(anx_iface_init() == ANX_OK, -1);
	/* Mapping needs a renderer; without one the windows never turn
	 * visible and cannot take focus. */
	ASSERT(anx_renderer_headless_register() == ANX_OK, -17);
	panel = make_surface(NULL);
	w1 = make_surface("one");
	w2 = make_surface("two");
	ASSERT(panel && w1 && w2, -2);
	panel->no_focus = true;

	/* Two tiled windows; the newest has the keyboard. */
	ASSERT(anx_wm_window_open(w1) == ANX_OK, -3);
	ASSERT(anx_wm_window_open(w2) == ANX_OK, -4);
	ASSERT(anx_wm_window_is_tiled(w1) && anx_wm_window_is_tiled(w2), -5);
	ASSERT(anx_wm_focused_window() == w2, -6);

	/* Clicking w1 focuses it; the panel raise that follows must not
	 * take the keyboard. */
	ASSERT(anx_wm_window_focus(w1) == ANX_OK, -7);
	ASSERT(anx_iface_surface_raise(panel) == ANX_OK, -8);
	f = anx_input_focus_get();
	ASSERT(f.hi == w1->oid.hi && f.lo == w1->oid.lo, -9);
	ASSERT(anx_wm_focused_window() == w1, -10);

	/* Meta+Q path: closing the focused window hands focus to its
	 * layout heir, never to the panel. */
	ASSERT(anx_wm_window_close(anx_wm_focused_window()) == ANX_OK, -11);
	ASSERT(anx_wm_focused_window() == w2, -12);
	f = anx_input_focus_get();
	ASSERT(f.hi == w2->oid.hi && f.lo == w2->oid.lo, -13);

	/* Float toggle takes a window out of the layout and back. */
	ASSERT(anx_wm_window_float_toggle(w2) == ANX_ENOENT ||
	       !anx_wm_window_is_tiled(w2), -14);

	/* A plain destroy of the focused window skips no_focus panels. */
	anx_input_focus_set(w2->oid);
	ASSERT(anx_wm_window_close(w2) == ANX_OK, -15);
	f = anx_input_focus_get();
	ASSERT(!(f.hi == panel->oid.hi && f.lo == panel->oid.lo), -16);

	anx_iface_surface_destroy(panel);
	return 0;
}

/*
 * Stacking. A commit paints a whole surface, so after a lower window
 * updates, the windows above it that it touched must be painted again,
 * and nothing else. Uses a renderer that only counts commits.
 */
static uint32_t g_commits[3];
static struct anx_surface *g_stack[3];

static int count_map(struct anx_surface *s) { (void)s; return ANX_OK; }
static void count_unmap(struct anx_surface *s) { (void)s; }
static void count_damage(struct anx_surface *s, int32_t x, int32_t y,
			 uint32_t w, uint32_t h)
{
	(void)s; (void)x; (void)y; (void)w; (void)h;
}
static int count_commit(struct anx_surface *s)
{
	uint32_t i;

	for (i = 0; i < 3; i++)
		if (g_stack[i] == s)
			g_commits[i]++;
	return ANX_OK;
}

static const struct anx_renderer_ops count_ops = {
	.map = count_map, .commit = count_commit,
	.damage = count_damage, .unmap = count_unmap,
};

int test_wm_restack(void)
{
	int32_t xs[3] = { 0, 100, 2000 };
	uint32_t i;

	/* No anx_iface_init(): other subsystems still hold surfaces. */
	ASSERT(anx_iface_renderer_register(ANX_ENGINE_RENDERER_ROBOT,
					   &count_ops, "count") == ANX_OK, -2);
	/* 0: bottom window; 1: overlaps it, above; 2: above, far away. */
	for (i = 0; i < 3; i++) {
		ASSERT(anx_iface_surface_create(ANX_ENGINE_RENDERER_ROBOT, NULL,
						xs[i], 100, 300, 300,
						&g_stack[i]) == ANX_OK, -3);
		ASSERT(anx_iface_surface_map(g_stack[i]) == ANX_OK, -4);
		ASSERT(anx_iface_surface_raise(g_stack[i]) == ANX_OK, -5);
	}
	anx_memset(g_commits, 0, sizeof(g_commits));

	ASSERT(anx_iface_surface_commit(g_stack[0]) == ANX_OK, -6);
	ASSERT(g_commits[0] == 1, -7);
	ASSERT(g_commits[1] == 1, -8);	/* repainted over the update */
	ASSERT(g_commits[2] == 0, -9);	/* untouched */

	/* The top window's own commit repaints nothing else. */
	ASSERT(anx_iface_surface_commit(g_stack[1]) == ANX_OK, -10);
	ASSERT(g_commits[0] == 1 && g_commits[1] == 2 && g_commits[2] == 0,
	       -11);

	/* The flat variant, for ordered repaints, touches only itself. */
	ASSERT(anx_iface_surface_commit_flat(g_stack[0]) == ANX_OK, -12);
	ASSERT(g_commits[0] == 2 && g_commits[1] == 2, -13);

	for (i = 0; i < 3; i++)
		anx_iface_surface_destroy(g_stack[i]);
	return 0;
}
