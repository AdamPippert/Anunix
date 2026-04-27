/*
 * anx/wm.h — Anunix window manager and desktop session.
 *
 * Provides:
 *   - 9 virtual workspaces (Omarchy-style)
 *   - Global hotkey dispatch (Meta+key on both Apple and x86)
 *   - Menu bar surface (workspace dots, clock, network, power)
 *   - Mouse pointer hit-testing and click-to-focus
 *   - Window open/close/raise/minimize lifecycle
 */
#ifndef ANX_WM_H
#define ANX_WM_H

#include <anx/types.h>
#include <anx/interface_plane.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define ANX_WM_WORKSPACES	9	/* number of virtual workspaces */
#define ANX_WM_WS_SURFS		32	/* max surfaces per workspace */
#define ANX_WM_HOTKEYS		64	/* max registered hotkeys */
#define ANX_WM_MENUBAR_H	34	/* menu bar height in pixels */
#define ANX_WM_DECOR_H		18	/* window titlebar decoration height */

/* ------------------------------------------------------------------ */
/* Workspace                                                           */
/* ------------------------------------------------------------------ */

struct anx_wm_workspace {
	uint32_t	id;				/* 1-based, 1..9 */
	anx_oid_t	surfs[ANX_WM_WS_SURFS];	/* surfaces on this ws */
	uint32_t	surf_count;
	anx_oid_t	focused;			/* focused surface OID */
};

/* ------------------------------------------------------------------ */
/* Hotkey                                                              */
/* ------------------------------------------------------------------ */

/* Callback receives the hotkey modifiers and key as context. */
typedef void (*anx_hotkey_fn)(uint32_t mods, uint32_t key, void *arg);

struct anx_hotkey {
	uint32_t	modifiers;	/* ANX_MOD_* bitmask */
	uint32_t	keycode;	/* ANX_KEY_* */
	anx_hotkey_fn	fn;
	void		*arg;
};

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Initialize the window manager (call after anx_iface_init). */
int  anx_wm_init(void);

/* Internal: called by wm.c during init */
void anx_wm_hotkeys_init(void);
int  anx_wm_menubar_create(void);

/* Enter the desktop event loop (replaces anx_shell_run on FB hardware). */
void anx_wm_run(void);

/* ---- Workspace management ---- */
int  anx_wm_workspace_switch(uint32_t ws_id);		/* 1-9 */
uint32_t anx_wm_workspace_active(void);
bool     anx_wm_workspace_occupied(uint32_t ws_id);	/* true if ws has windows */

/* Open a surface on the current workspace (maps + raises + focuses). */
int  anx_wm_window_open(struct anx_surface *surf);

/* Close and destroy a surface; focus previous window on workspace. */
int  anx_wm_window_close(struct anx_surface *surf);

/* Raise and focus a surface. */
int  anx_wm_window_focus(struct anx_surface *surf);

/* Minimize a surface (MINIMIZED state, removed from view). */
int  anx_wm_window_minimize(struct anx_surface *surf);

/* Toggle fullscreen for a surface. */
int  anx_wm_window_fullscreen_toggle(struct anx_surface *surf);

/* Snap a surface to the left or right half of the screen. */
int  anx_wm_window_tile_left(struct anx_surface *surf);
int  anx_wm_window_tile_right(struct anx_surface *surf);

/* Restore a tiled surface to its pre-tile floating bounds. */
int  anx_wm_window_float(struct anx_surface *surf);

/* Move a surface to a different workspace (1-based). */
int  anx_wm_window_send_to_workspace(struct anx_surface *surf, uint32_t ws_id);

/* Cycle keyboard focus to the next window on the active workspace. */
void anx_wm_focus_cycle(void);

/* ---- Hotkey registry ---- */
int  anx_wm_hotkey_register(uint32_t mods, uint32_t key,
			     anx_hotkey_fn fn, void *arg);

/* Called by input subsystem before forwarding key to focused surface. */
bool anx_wm_hotkey_dispatch(uint32_t mods, uint32_t key);

/* Route key to focused WM-managed app (terminal/viewer/designer). */
bool anx_wm_app_key_route(uint32_t key, uint32_t mods, uint32_t unicode);

/* ---- Menu bar ---- */
void anx_wm_menubar_refresh(void);	/* Redraw and commit menu bar */

/* ---- Toast notification (auto-dismisses after ~3 seconds) ---- */
void anx_wm_notify(const char *msg);

/* ---- Built-in applications ---- */
void anx_wm_launch_workflow_designer(void);
void anx_wm_launch_object_viewer(void);
void anx_wm_launch_command_search(void);

/* ---- Terminal surface ---- */
void anx_wm_terminal_open(void);
void anx_wm_terminal_key_event(uint32_t key, uint32_t mods, uint32_t unicode);
struct anx_surface *anx_wm_terminal_surface(void);

/* ---- Command search overlay ---- */
void anx_wm_search_key_event(uint32_t key, uint32_t mods, uint32_t unicode);
struct anx_surface *anx_wm_search_surface(void);

/* ---- Key event struct (used by switcher / app_menu) ---- */
struct anx_key_event {
	uint32_t keycode;
	uint32_t modifiers;
	uint32_t unicode;
};

/* ---- Terminal paste ---- */
void anx_wm_terminal_paste(const char *text, uint32_t len);

/* Print a line of text to the terminal history (opens terminal if needed). */
void anx_wm_terminal_print(const char *text);

/* Clear the current input line (undo). */
void anx_wm_terminal_clear_input(void);

/* Cut current input line to clipboard. */
void anx_wm_terminal_cut_input(void);

/* ---- App switcher (Meta+Tab) ---- */
void anx_wm_switcher_open(void);
void anx_wm_switcher_key_event(struct anx_key_event *ev);
void anx_wm_switcher_meta_released(void);
bool anx_wm_switcher_active(void);

/* Internal: record surface focus timestamp for switcher ordering. */
void anx_wm_activity_touch(anx_oid_t oid);

/* ---- App menu panels ---- */
void anx_wm_app_menu_open(uint32_t menu_index, anx_oid_t invocation_oid);
void anx_wm_app_menu_key_event(struct anx_key_event *ev);
bool anx_wm_app_menu_active(void);

#endif /* ANX_WM_H */
