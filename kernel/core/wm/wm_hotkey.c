/*
 * wm_hotkey.c — Global hotkey registry and Omarchy default bindings.
 *
 * Omarchy (Hyprland) defaults adapted for Anunix:
 *
 *   Meta+1..9          switch workspace
 *   Meta+Shift+1..9    send window to workspace
 *   Meta+Q, Meta+W     close focused window
 *   Meta+Return        open shell window
 *   Meta+Space         command search
 *   Meta+F             fullscreen toggle
 *   Meta+Shift+F, +T   toggle floating
 *   Meta+arrows / HJKL focus in a direction
 *   Meta+Shift+arrows  swap a tiled window (floating: move)
 *   Meta+Ctrl+arrows   resize (tiled: move the split)
 *   Meta+[ / Meta+]    swap left / right (floating: snap to a half)
 *   Meta+Tab           cycle window focus
 *   Meta+Shift+W       open workflow designer
 *   Meta+O             open object viewer
 *   Meta+M             minimize
 *
 * Meta maps to Super (x86) and Cmd (Apple) via ANX_MOD_META.
 * anx_wm_hotkey_dispatch() is called by anx_input_ps2_key /
 * anx_input_key_down before the event is posted to the event queue,
 * so hotkeys fire even when a surface has focus.
 */

#include <anx/wm.h>
#include <anx/config.h>
#include <anx/color_editor.h>
#include <anx/fb.h>
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
static uint32_t g_action_key[ANX_WM_HOTKEYS];
static char g_action_name[ANX_WM_HOTKEYS][32];
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
	g_action_key[g_hotkey_count] = key;
	anx_snprintf(g_action_name[g_hotkey_count], 32, "binding_%u", g_hotkey_count);
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
	/* While the power dialog is open it owns the keyboard, except for
	 * the shortcuts that opened it, which simply re-assert it. */
	if (anx_wm_power_active()) {
		anx_wm_power_key(key);
		return true;
	}

	uint32_t i;
	bool flags;
	anx_hotkey_fn fn = NULL;
	void *arg = NULL;
	uint32_t action_key = key;
	/*
	 * Held modifiers must match exactly, so Meta+Shift+W is not Meta+W.
	 * Lock states must not take part: with CapsLock on, an exact match
	 * failed for every binding and all hotkeys went dead.
	 */
	uint32_t held = mods & ~ANX_MOD_LOCKS;

	anx_spin_lock_irqsave(&g_hk_lock, &flags);
	for (i = 0; i < g_hotkey_count; i++) {
		if ((g_hotkeys[i].modifiers & ~ANX_MOD_LOCKS) == held &&
		    g_hotkeys[i].keycode   == key) {
			fn  = g_hotkeys[i].fn;
			arg = g_hotkeys[i].arg;
			action_key = g_action_key[i];
			break;
		}
	}
	anx_spin_unlock_irqrestore(&g_hk_lock, flags);

	if (fn) {
		fn(mods, action_key, arg);
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

	/* Priority 0: the power dialog is modal */
	if (anx_wm_power_active()) {
		anx_wm_power_key(key);
		return true;
	}

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

	s = anx_wm_color_editor_surface();
	if (s && s->oid.hi == focused.hi && s->oid.lo == focused.lo) {
		anx_wm_color_editor_key(key, mods, unicode);
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
	struct anx_surface *surf = NULL;
	(void)mods; (void)key;

	surf = anx_wm_focused_window();
	if (surf)
		anx_wm_window_send_to_workspace(surf, ws_id);
}

/* ---- Keyboard-driven window move/resize (Meta+Arrow / Meta+Shift+Arrow) */

#define WIN_MOVE_STEP   20   /* px per keypress */
#define WIN_RESIZE_STEP 16   /* px per keypress */

static void hk_win_move(uint32_t mods, uint32_t key, void *arg)
{
	struct anx_surface *surf;
	int32_t dx = 0, dy = 0;
	(void)mods; (void)arg;

	surf = anx_wm_focused_window();
	if (!surf || surf->state != ANX_SURF_VISIBLE)
		return;

	switch (key) {
	case ANX_KEY_LEFT:  dx = -1; break;
	case ANX_KEY_RIGHT: dx =  1; break;
	case ANX_KEY_UP:    dy = -1; break;
	case ANX_KEY_DOWN:  dy =  1; break;
	default: return;
	}
	/* Tiled: swap with the neighbour (sway "move left"). Floating: nudge. */
	if (anx_wm_window_is_tiled(surf)) {
		anx_wm_window_swap_dir(surf, dx, dy);
		return;
	}
	anx_wm_window_set_geometry(surf, surf->x + dx * WIN_MOVE_STEP,
				   surf->y + dy * WIN_MOVE_STEP,
				   surf->width, surf->height);
}

static void hk_win_resize(uint32_t mods, uint32_t key, void *arg)
{
	struct anx_surface *surf;
	uint32_t w, h;
	int32_t step = (int32_t)anx_wm_tiling.resize_step;
	(void)mods; (void)arg;

	surf = anx_wm_focused_window();
	if (!surf || surf->state != ANX_SURF_VISIBLE)
		return;

	/* Tiled: move the nearest split (sway "resize grow/shrink"). */
	if (anx_wm_window_is_tiled(surf)) {
		switch (key) {
		case ANX_KEY_LEFT:  anx_wm_window_resize_tiled(surf, false, -step); break;
		case ANX_KEY_RIGHT: anx_wm_window_resize_tiled(surf, false,  step); break;
		case ANX_KEY_UP:    anx_wm_window_resize_tiled(surf, true,  -step); break;
		case ANX_KEY_DOWN:  anx_wm_window_resize_tiled(surf, true,   step); break;
		default: break;
		}
		return;
	}

	w = surf->width;
	h = surf->height;
	switch (key) {
	case ANX_KEY_LEFT:
		if (w > WIN_RESIZE_STEP)
			w -= WIN_RESIZE_STEP;
		break;
	case ANX_KEY_RIGHT:
		w += WIN_RESIZE_STEP;
		break;
	case ANX_KEY_UP:
		if (h > WIN_RESIZE_STEP)
			h -= WIN_RESIZE_STEP;
		break;
	case ANX_KEY_DOWN:
		h += WIN_RESIZE_STEP;
		break;
	default: return;
	}
	anx_wm_window_set_geometry(surf, surf->x, surf->y, w, h);
}

static void hk_minimize(uint32_t mods, uint32_t key, void *arg)
{
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	surf = anx_wm_focused_window();
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
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	surf = anx_wm_focused_window();
	if (surf)
		anx_wm_window_close(surf);
}

static void hk_fullscreen(uint32_t mods, uint32_t key, void *arg)
{
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	surf = anx_wm_focused_window();
	if (surf)
		anx_wm_window_fullscreen_toggle(surf);
}

static void hk_switcher(uint32_t mods, uint32_t key, void *arg)
{
	struct anx_surface *s;
	(void)mods; (void)key; (void)arg;
	anx_wm_focus_cycle();
	s = anx_wm_focused_window();
	if (s && s->title[0])
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
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	surf = anx_wm_focused_window();
	if (surf)
		anx_wm_window_tile_left(surf);
}

static void hk_tile_right(uint32_t mods, uint32_t key, void *arg)
{
	struct anx_surface *surf = NULL;
	(void)mods; (void)key; (void)arg;

	surf = anx_wm_focused_window();
	if (surf)
		anx_wm_window_tile_right(surf);
}

static void hk_float(uint32_t mods, uint32_t key, void *arg)
{
	struct anx_surface *surf;
	(void)mods; (void)key; (void)arg;

	surf = anx_wm_focused_window();
	if (surf)
		anx_wm_window_float_toggle(surf);
}

/* Meta+arrows and Meta+HJKL move focus; the switcher uses the same axes. */
static void hk_focus_dir(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)arg;

	switch (key) {
	case ANX_KEY_LEFT:  case ANX_KEY_H: anx_wm_window_focus_dir(-1, 0); break;
	case ANX_KEY_RIGHT: case ANX_KEY_L: anx_wm_window_focus_dir(1, 0);  break;
	case ANX_KEY_UP:    case ANX_KEY_K: anx_wm_window_focus_dir(0, -1); break;
	case ANX_KEY_DOWN:  case ANX_KEY_J: anx_wm_window_focus_dir(0, 1);  break;
	default: break;
	}
}

/* Meta+T toggles floating, like Meta+Shift+F (Hyprland togglefloating). */
static void hk_tile_toggle(uint32_t mods, uint32_t key, void *arg)
{
	struct anx_surface *surf;
	(void)mods; (void)key; (void)arg;

	surf = anx_wm_focused_window();
	if (surf)
		anx_wm_window_float_toggle(surf);
}

static void hk_power(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	anx_wm_power_open();
}

static void hk_colors(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	anx_wm_launch_color_editor();
}

int anx_wm_hotkeys_snapshot(struct anx_config_hotkey *out, uint32_t capacity)
{
	uint32_t i, count;
	bool flags;
	if (!out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&g_hk_lock, &flags);
	count = g_hotkey_count;
	if (!count || capacity < count) {
		anx_spin_unlock_irqrestore(&g_hk_lock, flags);
		return ANX_EINVAL;
	}
	for (i = 0; i < count; i++) {
		out[i].modifiers = g_hotkeys[i].modifiers;
		out[i].keycode = g_hotkeys[i].keycode;
	}
	anx_spin_unlock_irqrestore(&g_hk_lock, flags);
	return (int)count;
}

const char *anx_wm_hotkey_name(uint32_t index)
{
	return index < g_hotkey_count ? g_action_name[index] : NULL;
}

int anx_wm_hotkeys_apply(const struct anx_config_hotkey *keys, uint32_t count)
{
	uint32_t i, j;
	bool flags;
	if (!keys || !count || count > ANX_WM_HOTKEYS) return ANX_EINVAL;
	for (i = 0; i < count; i++) {
		if (keys[i].modifiers > 15 || keys[i].keycode < 4 || keys[i].keycode > 0xDF)
			return ANX_EINVAL;
		for (j = 0; j < i; j++)
			if (keys[j].modifiers == keys[i].modifiers &&
			    keys[j].keycode == keys[i].keycode) return ANX_EEXIST;
	}
	anx_spin_lock_irqsave(&g_hk_lock, &flags);
	if (count != g_hotkey_count) {
		anx_spin_unlock_irqrestore(&g_hk_lock, flags);
		return ANX_EBUSY;
	}
	for (i = 0; i < count; i++) {
		g_hotkeys[i].modifiers = keys[i].modifiers;
		g_hotkeys[i].keycode = keys[i].keycode;
	}
	anx_spin_unlock_irqrestore(&g_hk_lock, flags);
	return ANX_OK;
}

static void name_actions(void)
{
	static const struct { anx_hotkey_fn fn; const char *name; } actions[] = {
		{hk_close, "close"}, {hk_fullscreen, "fullscreen"},
		{hk_shell, "shell"}, {hk_search, "search"}, {hk_colors, "colors"},
		{hk_switcher, "switcher"}, {hk_workflow_designer, "workflow"},
		{hk_tile_toggle, "tile_toggle"}, {hk_object_viewer, "objects"},
		{hk_copy, "copy"}, {hk_paste, "paste"}, {hk_undo, "undo"},
		{hk_cut, "cut"}, {hk_tile_left, "tile_left"}, {hk_tile_right, "tile_right"},
		{hk_float, "float"}, {hk_power, "power"}, {hk_minimize, "minimize"},
		{hk_focus_dir, "focus"}, {hk_win_move, "move"}, {hk_win_resize, "resize"}
	};
	uint32_t i, j, k;
	for (i = 0; i < g_hotkey_count; i++) {
		if (g_hotkeys[i].fn == hk_workspace || g_hotkeys[i].fn == hk_send_to_workspace) {
			anx_snprintf(g_action_name[i], 32, "%s_%u",
				g_hotkeys[i].fn == hk_workspace ? "workspace" : "send_workspace",
				(uint32_t)(uintptr_t)g_hotkeys[i].arg);
			continue;
		}
		for (j = 0; j < sizeof(actions) / sizeof(actions[0]); j++) {
			if (g_hotkeys[i].fn != actions[j].fn) continue;
			for (k = 0; k < i; k++)
				if (g_hotkeys[k].fn == actions[j].fn) break;
			if (k == i) anx_strlcpy(g_action_name[i], actions[j].name, 32);
			else anx_snprintf(g_action_name[i], 32, "%s_%u", actions[j].name, g_action_key[i]);
			break;
		}
	}
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
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_W,        hk_close,              NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_W,     hk_workflow_designer,  NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_T,        hk_tile_toggle,        NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_O,        hk_object_viewer,      NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_C,        hk_copy,               NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_V,        hk_paste,              NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_Z,        hk_undo,               NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_X,        hk_cut,                NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_LBRACKET, hk_tile_left,          NULL);
	anx_wm_hotkey_register(ANX_MOD_META,              ANX_KEY_RBRACKET, hk_tile_right,         NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_F,    hk_float,              NULL);
	anx_wm_hotkey_register(ANX_MOD_META,                 ANX_KEY_ESC,  hk_power,              NULL);
	anx_wm_hotkey_register(ANX_MOD_CTRL | ANX_MOD_ALT,   ANX_KEY_DELETE, hk_power,            NULL);
	anx_wm_hotkey_register(ANX_MOD_META,                  ANX_KEY_M,    hk_minimize,           NULL);

	/* Focus follows the arrows or HJKL (Meta) */
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_LEFT,  hk_focus_dir, NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_RIGHT, hk_focus_dir, NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_UP,    hk_focus_dir, NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_DOWN,  hk_focus_dir, NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_H,     hk_focus_dir, NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_L,     hk_focus_dir, NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_J,     hk_focus_dir, NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_K,     hk_focus_dir, NULL);

	/* Move the window (Meta+Shift) */
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_LEFT,  hk_win_move, NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_RIGHT, hk_win_move, NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_UP,    hk_win_move, NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_DOWN,  hk_win_move, NULL);

	/* Resize the window (Meta+Ctrl) */
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_CTRL, ANX_KEY_LEFT,  hk_win_resize, NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_CTRL, ANX_KEY_RIGHT, hk_win_resize, NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_CTRL, ANX_KEY_UP,    hk_win_resize, NULL);
	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_CTRL, ANX_KEY_DOWN,  hk_win_resize, NULL);

	anx_wm_hotkey_register(ANX_MOD_META | ANX_MOD_SHIFT, ANX_KEY_C, hk_colors, NULL);
	name_actions();

	kprintf("[wm] hotkeys registered (%u bindings)\n", g_hotkey_count);
}
