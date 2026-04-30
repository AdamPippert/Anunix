/*
 * wm_hotkey.c — Global hotkey registry and Omarchy default bindings.
 *
 * Omarchy (Hyprland) defaults adapted for Anunix:
 *
 *   Meta+1..9    switch workspace
 *   Meta+Q       close focused window
 *   Meta+Return  open shell window
 *   Meta+Space   command search
 *   Meta+F       fullscreen toggle
 *   Meta+Tab     cycle window focus
 *   Meta+W       open workflow designer
 *   Meta+O       open object viewer
 *   Meta+[       tile left
 *   Meta+]       tile right
 *   Meta+Shift+F float (restore from tile)
 *
 * Meta maps to Super (x86) and Cmd (Apple) via ANX_MOD_META.
 * anx_wm_hotkey_dispatch() is called by anx_input_ps2_key /
 * anx_input_key_down before the event is posted to the event queue,
 * so hotkeys fire even when a surface has focus.
 */

#include <anx/wm.h>
#include <anx/input.h>
#include <anx/interface_plane.h>
#include <anx/clipboard.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/spinlock.h>
#include <anx/arch.h>

/* CID used by the WM for clipboard operations — a well-known sentinel. */
#define WM_CID		((anx_cid_t){.hi = 0, .lo = 0xFFFF0001u})

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

static struct anx_hotkey  g_hotkeys[ANX_WM_HOTKEYS];
static uint32_t           g_hotkey_count;
static struct anx_spinlock g_hk_lock;

int anx_wm_hotkey_register(uint32_t mods, uint32_t key,
			    anx_hotkey_fn fn, void *arg)
{
	bool flags;

	if (!fn)
		return ANX_EINVAL;

	anx_spin_lock_irqsave(&g_hk_lock, &flags);
	if (g_hotkey_count >= ANX_WM_HOTKEYS) {
		anx_spin_unlock_irqrestore(&g_hk_lock, flags);
		return ANX_ENOMEM;
	}
	g_hotkeys[g_hotkey_count].modifiers = mods;
	g_hotkeys[g_hotkey_count].keycode   = key;
	g_hotkeys[g_hotkey_count].fn        = fn;
	g_hotkeys[g_hotkey_count].arg       = arg;
	g_hotkey_count++;
	anx_spin_unlock_irqrestore(&g_hk_lock, flags);
	return ANX_OK;
}

bool anx_wm_hotkey_dispatch(uint32_t mods, uint32_t key)
{
	uint32_t i;
	bool flags;
	anx_hotkey_fn fn = NULL;
	void *arg = NULL;

	anx_spin_lock_irqsave(&g_hk_lock, &flags);
	for (i = 0; i < g_hotkey_count; i++) {
		if (g_hotkeys[i].modifiers == mods &&
		    g_hotkeys[i].keycode   == key) {
			fn  = g_hotkeys[i].fn;
			arg = g_hotkeys[i].arg;
			break;
		}
	}
	anx_spin_unlock_irqrestore(&g_hk_lock, flags);

	if (fn) {
		fn(mods, key, arg);
		return true;
	}
	return false;
}

bool anx_wm_app_key_route(uint32_t key, uint32_t mods, uint32_t unicode)
{
	anx_oid_t focused = anx_input_focus_get();
	struct anx_surface *s;
	struct anx_key_event kev;

	kev.keycode   = key;
	kev.modifiers = mods;
	kev.unicode   = unicode;

	/* Priority 1: switcher */
	if (anx_wm_switcher_active()) {
		anx_wm_switcher_key_event(&kev);
		return true;
	}

	if (focused.hi == 0 && focused.lo == 0)
		return false;

	/* Priority 2: search overlay */
	s = anx_wm_search_surface();
	if (s && s->oid.hi == focused.hi && s->oid.lo == focused.lo) {
		anx_wm_search_key_event(key, mods, unicode);
		return true;
	}

	/* Priority 3: app menu */
	if (anx_wm_app_menu_active()) {
		anx_wm_app_menu_key_event(&kev);
		return true;
	}

	/* Priority 4: terminal */
	s = anx_wm_terminal_surface();
	if (s && s->oid.hi == focused.hi && s->oid.lo == focused.lo) {
		anx_wm_terminal_key_event(key, mods, unicode);
		return true;
	}

	/* Priority 5: agent */
	s = anx_wm_agent_surface();
	if (s && s->oid.hi == focused.hi && s->oid.lo == focused.lo) {
		anx_wm_agent_key_event(key, mods, unicode);
		return true;
	}

	return false;
}

/* ------------------------------------------------------------------ */
/* Default action callbacks                                            */
/* ------------------------------------------------------------------ */

static void hk_workspace(uint32_t mods, uint32_t key, void *arg)
{
	uint32_t ws = (uint32_t)(uintptr_t)arg;
	(void)mods; (void)key;
	anx_wm_workspace_switch(ws);
}

static void hk_send_to_workspace(uint32_t mods, uint32_t key, void *arg)
{
	uint32_t ws_id = (uint32_t)(uintptr_t)arg;
	anx_oid_t focused;
	struct anx_surface *surf = NULL;
	(void)mods; (void)key;

	focused = anx_input_focus_get();
	anx_iface_surface_lookup(focused, &surf);
	if (surf)
		anx_wm_window_send_to_workspace(surf, ws_id);
}

/* ---- Keyboard-driven window move/resize (Meta+Arrow / Meta+Shift+Arrow) */

#define WIN_MOVE_STEP   20   /* px per keypress */
#define WIN_RESIZE_STEP 16   /* px per keypress */

static void hk_win_move(uint32_t mods, uint32_t key, void *arg)
{
	anx_oid_t foc;
	struct anx_surface *surf = NULL;
	int32_t dx = 0, dy = 0;
	(void)mods; (void)arg;

	foc = anx_input_focus_get();
	anx_iface_surface_lookup(foc, &surf);
	if (!surf || surf->state != ANX_SURF_VISIBLE)
		return;

	switch (key) {
	case ANX_KEY_LEFT:  dx = -WIN_MOVE_STEP; break;
	case ANX_KEY_RIGHT: dx =  WIN_MOVE_STEP; break;
	case ANX_KEY_UP:    dy = -WIN_MOVE_STEP; break;
	case ANX_KEY_DOWN:  dy =  WIN_MOVE_STEP; break;
	default: return;
	}
	anx_iface_surface_move(surf, surf->x + dx, surf->y + dy);
	anx_iface_surface_commit(surf);
}

static void hk_win_resize(uint32_t mods, uint32_t key, void *arg)
{
	anx_oid_t foc;
	struct anx_surface *surf = NULL;
	(void)mods; (void)arg;

	foc = anx_input_focus_get();
	anx_iface_surface_lookup(foc, &surf);
	if (!surf || surf->state != ANX_SURF_VISIBLE)
		return;

	switch (key) {
	case ANX_KEY_LEFT:
		if (surf->width > WIN_RESIZE_STEP)
			surf->width -= WIN_RESIZE_STEP;
		break;
	case ANX_KEY_RIGHT:
		surf->width += WIN_RESIZE_STEP;
		break;
	case ANX_KEY_UP:
		if (surf->height > WIN_RESIZE_STEP)
			surf->height -= WIN_RESIZE_STEP;
		break;
	case ANX_KEY_DOWN:
		surf->height += WIN_RESIZE_STEP;
		break;
	default: return;
	}
	anx_iface_surface_commit(surf);
}

static void hk_minimize(uint32_t mods, uint32_t key, void *arg)
{
	anx_oid_t focused;
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	focused = anx_input_focus_get();
	anx_iface_surface_lookup(focused, &surf);
	if (!surf)
		return;

	/* Toggle: restore if already minimized, otherwise minimize */
	if (surf->state == ANX_SURF_MINIMIZED)
		anx_wm_window_restore(surf);
	else
		anx_wm_window_minimize(surf);
}

static void hk_close(uint32_t mods, uint32_t key, void *arg)
{
	anx_oid_t focused;
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	focused = anx_input_focus_get();
	if (focused.hi == 0 && focused.lo == 0)
		return;
	anx_iface_surface_lookup(focused, &surf);
	if (surf)
		anx_wm_window_close(surf);
}

static void hk_fullscreen(uint32_t mods, uint32_t key, void *arg)
{
	anx_oid_t focused;
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	focused = anx_input_focus_get();
	if (focused.hi == 0 && focused.lo == 0)
		return;
	anx_iface_surface_lookup(focused, &surf);
	if (surf)
		anx_wm_window_fullscreen_toggle(surf);
}

static void hk_switcher(uint32_t mods, uint32_t key, void *arg)
{
	anx_oid_t foc;
	struct anx_surface *s = NULL;
	(void)mods; (void)key; (void)arg;
	anx_wm_focus_cycle();
	foc = anx_input_focus_get();
	if (anx_iface_surface_lookup(foc, &s) == ANX_OK && s && s->title[0])
		anx_wm_notify(s->title);
}

static void hk_copy(uint32_t mods, uint32_t key, void *arg)
{
	struct anx_surface *s;
	anx_oid_t focused;
	(void)mods; (void)key; (void)arg;

	anx_clipboard_grant(WM_CID, ANX_CLIPBOARD_FLAG_WRITE);
	focused = anx_input_focus_get();
	s = anx_wm_terminal_surface();
	if (s && s->oid.hi == focused.hi && s->oid.lo == focused.lo) {
		/* Terminal selection: no selection API yet; write empty. */
		anx_clipboard_write(WM_CID, "text/plain", "", 0);
	}
}

static void hk_paste(uint32_t mods, uint32_t key, void *arg)
{
	char    mime[64];
	char    buf[ANX_CLIPBOARD_MAX_SIZE];
	uint32_t len = 0;
	(void)mods; (void)key; (void)arg;

	anx_clipboard_grant(WM_CID, ANX_CLIPBOARD_FLAG_READ);
	if (anx_clipboard_read(WM_CID, mime, sizeof(mime),
			       buf, sizeof(buf) - 1, &len) != ANX_OK)
		return;
	buf[len] = '\0';

	if (anx_wm_terminal_surface() != NULL)
		anx_wm_terminal_paste(buf, len);
}

static void hk_shell(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	anx_wm_launch_terminal();
}

static void hk_search(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	anx_wm_launch_command_search();
}

static void hk_workflow_designer(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	anx_wm_launch_workflow_designer();
}

static void hk_object_viewer(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	anx_wm_launch_object_viewer();
}

static void hk_undo(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	anx_wm_terminal_clear_input();
}

static void hk_cut(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	anx_wm_terminal_cut_input();
}

static void hk_tile_left(uint32_t mods, uint32_t key, void *arg)
{
	anx_oid_t focused;
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	focused = anx_input_focus_get();
	anx_iface_surface_lookup(focused, &surf);
	if (surf)
		anx_wm_window_tile_left(surf);
}

static void hk_tile_right(uint32_t mods, uint32_t key, void *arg)
{
	anx_oid_t focused;
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	focused = anx_input_focus_get();
	anx_iface_surface_lookup(focused, &surf);
	if (surf)
		anx_wm_window_tile_right(surf);
}

static void hk_float(uint32_t mods, uint32_t key, void *arg)
{
	anx_oid_t focused;
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	focused = anx_input_focus_get();
	anx_iface_surface_lookup(focused, &surf);
	if (surf)
		anx_wm_window_float(surf);
}

static void hk_halt(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	kprintf("[wm] system halt requested\n");
	arch_halt();
}

/* ------------------------------------------------------------------ */
/* Register Omarchy defaults                                           */
/* ------------------------------------------------------------------ */

void anx_wm_hotkeys_init(void)
{
	uint32_t ws;

	anx_spin_init(&g_hk_lock);
	g_hotkey_count = 0;

	/* Meta+1..9 → switch workspace; Meta+Shift+1..9 → send window to ws */
	for (ws = 1; ws <= 9; ws++) {
		/* ANX_KEY_1 = 0x1E, ANX_KEY_2 = 0x1F, ... */
		uint32_t key = ANX_KEY_1 + (ws - 1);
		anx_wm_hotkey_register(ANX_MOD_META, key, hk_workspace,
				       (void *)(uintptr_t)ws);
		anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, key,
				       hk_send_to_workspace,
				       (void *)(uintptr_t)ws);
	}

	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_Q,        hk_close,              NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_F,        hk_fullscreen,         NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_TAB,      hk_switcher,           NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_ENTER,    hk_shell,              NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_SPACE,    hk_search,             NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_W,        hk_workflow_designer,  NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_O,        hk_object_viewer,      NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_C,        hk_copy,               NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_V,        hk_paste,              NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_Z,        hk_undo,               NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_X,        hk_cut,                NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_LBRACKET, hk_tile_left,          NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_RBRACKET, hk_tile_right,         NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_F,    hk_float,              NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_H,    hk_halt,               NULL);
	anx_wm_hotkey_register(ANX_MOD_META,                  ANX_KEY_M,    hk_minimize,           NULL);

	/* Keyboard-driven window move (Meta+Arrow) */
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_LEFT,  hk_win_move, NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_RIGHT, hk_win_move, NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_UP,    hk_win_move, NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_DOWN,  hk_win_move, NULL);

	/* Keyboard-driven window resize (Meta+Shift+Arrow) */
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_LEFT,  hk_win_resize, NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_RIGHT, hk_win_resize, NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_UP,    hk_win_resize, NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_DOWN,  hk_win_resize, NULL);

	kprintf("[wm] hotkeys registered (%u bindings)\n", g_hotkey_count);
}
