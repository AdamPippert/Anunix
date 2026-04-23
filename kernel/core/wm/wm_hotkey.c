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
 *
 * Meta maps to Super (x86) and Cmd (Apple) via ANX_MOD_META.
 * anx_wm_hotkey_dispatch() is called by anx_input_ps2_key /
 * anx_input_key_down before the event is posted to the event queue,
 * so hotkeys fire even when a surface has focus.
 */

#include <anx/wm.h>
#include <anx/input.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/spinlock.h>

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

/* ------------------------------------------------------------------ */
/* Default action callbacks                                            */
/* ------------------------------------------------------------------ */

static void hk_workspace(uint32_t mods, uint32_t key, void *arg)
{
	uint32_t ws = (uint32_t)(uintptr_t)arg;
	(void)mods; (void)key;
	anx_wm_workspace_switch(ws);
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

static void hk_cycle(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	anx_wm_focus_cycle();
}

static void hk_shell(uint32_t mods, uint32_t key, void *arg)
{
	(void)mods; (void)key; (void)arg;
	/* Shell window launched by the WM as a surface wrapping a terminal */
	kprintf("[wm] shell window requested\n");
	/* Phase 2: spawn a terminal surface */
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

/* ------------------------------------------------------------------ */
/* Register Omarchy defaults                                           */
/* ------------------------------------------------------------------ */

void anx_wm_hotkeys_init(void)
{
	uint32_t ws;

	anx_spin_init(&g_hk_lock);
	g_hotkey_count = 0;

	/* Meta+1..9 → switch workspace */
	for (ws = 1; ws <= 9; ws++) {
		/* ANX_KEY_1 = 0x1E, ANX_KEY_2 = 0x1F, ... */
		uint32_t key = ANX_KEY_1 + (ws - 1);
		anx_wm_hotkey_register(ANX_MOD_META, key, hk_workspace,
				       (void *)(uintptr_t)ws);
	}

	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_Q,      hk_close,              NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_F,      hk_fullscreen,         NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_TAB,    hk_cycle,              NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_ENTER,  hk_shell,              NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_SPACE,  hk_search,             NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_W,      hk_workflow_designer,  NULL);
	anx_wm_hotkey_register(ANX_MOD_META, ANX_KEY_O,      hk_object_viewer,      NULL);

	kprintf("[wm] hotkeys registered (%u bindings)\n", g_hotkey_count);
}
