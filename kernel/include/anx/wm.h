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
#define ANX_WM_TASKBAR_H	22	/* taskbar height in pixels */
#define ANX_WM_DECOR_H		28	/* window titlebar decoration height */

/*
 * Titlebar buttons: close, minimize, maximize, left to right from the
 * window's left edge. The renderer draws them and the WM hit-tests them
 * from these same numbers, so the two cannot drift apart again.
 */
#define ANX_WM_BTN_D		14	/* button diameter */
#define ANX_WM_BTN_LEFT		8	/* window edge to first button */
#define ANX_WM_BTN_GAP		5	/* space between buttons */

/*
 * Largest pixel buffer one window may own. A large allocation takes a
 * power-of-two run of pages, so a full-screen 2560x1600 buffer needs an
 * order-13 (32 MiB) block. That is the ceiling when the page heap comes
 * from the firmware memory map; on the 16 MiB linker heap (QEMU multiboot)
 * a window gets one order-10 block, 4 MiB including the allocator header.
 */
#define ANX_WM_WINDOW_BYTES_MAX		((32u << 20) - 16u)
#define ANX_WM_WINDOW_BYTES_SMALL	((4u << 20) - 16u)
#define ANX_WM_LARGE_HEAP_BYTES		(512ull << 20)

/* ------------------------------------------------------------------ */
/* Tiling                                                              */
/* ------------------------------------------------------------------ */

/*
 * Tiling settings, modelled on Hyprland's dwindle layout. Each field names
 * its Hyprland option and the sway equivalent. Change them here, or at run
 * time through anx_wm_tiling before windows open.
 *
 * Gaps follow Hyprland: gaps_out separates windows from the screen edges,
 * and each window keeps gaps_in on every side, so two neighbours sit
 * 2 * gaps_in apart. Hyprland's defaults are 5 and 20; sway's are 0 and 0.
 * 5 and 10 suit both a 2560x1600 panel and a smaller VM display.
 *
 * Border colours come from the theme: palette.accent for the focused
 * window (col.active_border / sway client.focused) and palette.surface for
 * the rest (col.inactive_border / sway client.unfocused).
 */
struct anx_wm_tiling_config {
	bool     enabled;	/* new windows tile (Hyprland default; sway: always) */
	uint32_t gaps_in;	/* general:gaps_in      / sway: gaps inner */
	uint32_t gaps_out;	/* general:gaps_out     / sway: gaps outer */
	uint32_t border_w;	/* general:border_size  / sway: default_border pixel N */
	uint32_t split_ratio;	/* dwindle:default_split_ratio, in permille of the
				 * first child (1.0 in Hyprland = 500 here) */
	uint32_t resize_step;	/* permille per Meta+Ctrl+arrow (sway: resize ... 10 ppt) */
};

extern struct anx_wm_tiling_config anx_wm_tiling;

/* anx_surface.wm_flags */
#define ANX_WM_SF_FLOATING	(1u << 0)	/* kept out of the layout */
#define ANX_WM_SF_WAS_TILED	(1u << 1)	/* minimized out of the layout */

#define ANX_WM_TILE_NODES	(2 * ANX_WM_WS_SURFS)

struct anx_wm_rect {
	int32_t  x, y;
	uint32_t w, h;
};

/* One node of a workspace's dwindle tree: a window or a two-way split. */
struct anx_wm_tile_node {
	anx_oid_t		leaf;		/* the window, when is_leaf */
	int16_t			parent;		/* -1 at the root */
	int16_t			child[2];	/* first, second (splits only) */
	uint16_t		ratio;		/* first child's share, permille */
	bool			used;
	bool			is_leaf;
	bool			vertical;	/* last layout stacked the children */
	struct anx_wm_rect	box;		/* last layout's box for this node */
};

struct anx_wm_tile_tree {
	struct anx_wm_tile_node	nodes[ANX_WM_TILE_NODES];
	int16_t			root;		/* -1 when empty */
	uint32_t		count;		/* windows in the tree */
};

/* Empty a tree. */
void anx_wm_tile_init(struct anx_wm_tile_tree *t);

/* True if oid is tiled in t. */
bool anx_wm_tile_contains(const struct anx_wm_tile_tree *t,
			  const anx_oid_t *oid);

/* Add oid by splitting target's box (dwindle); NULL or unknown target
 * splits the most recently added window. */
int  anx_wm_tile_insert(struct anx_wm_tile_tree *t, const anx_oid_t *oid,
			const anx_oid_t *target);

/* Remove oid; its sibling takes the parent's box. Returns ANX_ENOENT if
 * oid is not tiled. *heir, when not NULL, gets a window from that sibling
 * (the natural next focus) or a nil OID. */
int  anx_wm_tile_remove(struct anx_wm_tile_tree *t, const anx_oid_t *oid,
			anx_oid_t *heir);

/* Exchange the positions of two tiled windows. */
int  anx_wm_tile_swap(struct anx_wm_tile_tree *t, const anx_oid_t *a,
		      const anx_oid_t *b);

/* Grow (delta > 0) or shrink oid along one axis by moving the nearest
 * split on that axis, as sway's "resize grow width" does. Uses the
 * orientation from the last layout. */
int  anx_wm_tile_resize(struct anx_wm_tile_tree *t, const anx_oid_t *oid,
			bool vertical, int32_t delta_permille);

/* Lay the tree out in area. Writes each window's outer box (title bar and
 * border included, gaps excluded) and returns the number written. */
uint32_t anx_wm_tile_layout(struct anx_wm_tile_tree *t,
			    const struct anx_wm_rect *area,
			    const struct anx_wm_tiling_config *cfg,
			    anx_oid_t *oids, struct anx_wm_rect *boxes,
			    uint32_t max);

/* ------------------------------------------------------------------ */
/* Workspace                                                           */
/* ------------------------------------------------------------------ */

struct anx_wm_workspace {
	uint32_t	id;				/* 1-based, 1..9 */
	anx_oid_t	surfs[ANX_WM_WS_SURFS];	/* surfaces on this ws, oldest first */
	uint32_t	surf_count;
	anx_oid_t	focused;			/* focused surface OID */
	struct anx_wm_tile_tree tiles;		/* tiled windows */
	anx_oid_t	fullscreen;			/* nil, or the fullscreen window */
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

/* Open a surface on the current workspace and focus it. It tiles unless
 * tiling is off; transient panels use anx_wm_window_open_floating(). */
int  anx_wm_window_open(struct anx_surface *surf);

/* Open a surface on the current workspace as a floating window. */
int  anx_wm_window_open_floating(struct anx_surface *surf);

/* The active workspace's focused window, or NULL. Hotkeys act on this. */
struct anx_surface *anx_wm_focused_window(void);

/* True if surf is part of its workspace's tiling layout. */
bool anx_wm_window_is_tiled(const struct anx_surface *surf);

/* Move a tiled window out of the layout, or a floating one into it. */
int  anx_wm_window_float_toggle(struct anx_surface *surf);

/* Swap a tiled window with its neighbour in a direction (sway "move"). */
int  anx_wm_window_swap_dir(struct anx_surface *surf, int32_t dx, int32_t dy);

/* Grow or shrink a tiled window (sway "resize"). */
int  anx_wm_window_resize_tiled(struct anx_surface *surf, bool vertical,
				int32_t delta_permille);

/* Recompute and apply the active workspace's layout. */
void anx_wm_retile(void);

/* Close and destroy a surface; focus previous window on workspace. */
int  anx_wm_window_close(struct anx_surface *surf);

/* Raise and focus a surface. */
int  anx_wm_window_focus(struct anx_surface *surf);

/* Minimize a surface (MINIMIZED state, removed from view). */
int  anx_wm_window_minimize(struct anx_surface *surf);

/* Restore a minimized surface to VISIBLE and focus it. */
int  anx_wm_window_restore(struct anx_surface *surf);

/* Toggle fullscreen for a surface. */
int  anx_wm_window_fullscreen_toggle(struct anx_surface *surf);

/* Snap a surface to the left or right half of the screen. */
int  anx_wm_window_tile_left(struct anx_surface *surf);
int  anx_wm_window_tile_right(struct anx_surface *surf);

/* Restore a half-snapped floating surface to its earlier bounds. */
int  anx_wm_window_float(struct anx_surface *surf);

/* Focus the nearest window in a direction: (-1,0) left, (1,0) right,
 * (0,-1) up, (0,1) down. */
int  anx_wm_window_focus_dir(int32_t dx, int32_t dy);

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
void anx_wm_menubar_refresh(void);

/* Power dialog: confirms restart or halt before anything happens. */
void anx_wm_power_open(void);
void anx_wm_power_close(void);
bool anx_wm_power_active(void);
bool anx_wm_power_key(uint32_t key);
bool anx_wm_power_pointer(int32_t x, int32_t y, uint32_t buttons,
			  bool move_only);	/* Redraw and commit menu bar */

/* ---- Taskbar (minimized window dock, bottom strip) ---- */
int  anx_wm_taskbar_create(void);
void anx_wm_taskbar_refresh(void);
void anx_wm_taskbar_raise(void);
bool anx_wm_taskbar_pointer(int32_t x, int32_t y, uint32_t buttons,
			     bool move_only);

/* Return OIDs of minimized windows on the active workspace. */
uint32_t anx_wm_minimized_list(anx_oid_t *out, uint32_t max);

/* Repaint the desktop and every visible window within a screen rectangle. */
void anx_wm_expose(int32_t x, int32_t y, uint32_t w, uint32_t h);

/* Move and resize a window, repainting whatever it uncovers. */
int anx_wm_window_set_geometry(struct anx_surface *surf, int32_t x,
			       int32_t y, uint32_t w, uint32_t h);

/* Destroy an unmanaged overlay (menu, dialog, toast) and repaint beneath it. */
void anx_wm_overlay_destroy(struct anx_surface *surf);

/* Free a canvas surface's content node and pixel buffer (for on_destroy). */
void anx_wm_canvas_free(struct anx_surface *surf);

/* Give a canvas surface a new w x h pixel buffer and free the old one.
 * Returns the new buffer, or NULL with the old one still in place. */
uint32_t *anx_wm_canvas_realloc(struct anx_surface *surf, uint32_t w,
				uint32_t h);

/* Shrink *w x *h, keeping its shape, until its pixels fit one window buffer. */
void anx_wm_window_fit(uint32_t *w, uint32_t *h);

/* ---- Toast notification (auto-dismisses after ~3 seconds) ---- */
void anx_wm_notify(const char *msg);

/* ---- Built-in applications ---- */
void anx_wm_launch_terminal(void);
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

/* Open a state object for interactive editing in the terminal.
 * ns_name: namespace (e.g. "default"), path: object path. */
void anx_wm_terminal_edit(const char *ns_name, const char *path);

/* Flush terminal pixel buffer to framebuffer if dirty (call from main WM loop). */
void anx_wm_terminal_flush_if_dirty(void);

/* Force a full repaint of the terminal surface (used by amacs). */
void anx_wm_terminal_redraw(void);

/* ---- Boot-time AI agent surface ---- */
void anx_wm_agent_open(void);
void anx_wm_agent_key_event(uint32_t key, uint32_t mods, uint32_t unicode);
void anx_wm_agent_flush_if_dirty(void);
struct anx_surface *anx_wm_agent_surface(void);

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

/* ---- Keyboard shortcut help overlay (F1) ---- */
void anx_wm_help_toggle(void);
void anx_wm_help_close(void);
bool anx_wm_help_active(void);
bool anx_wm_help_key(uint32_t key);

/* ---- Right-click context menu ---- */
/* Open a window context menu for surf at screen position (x, y). */
void anx_wm_ctx_menu_open(struct anx_surface *surf, int32_t x, int32_t y);
void anx_wm_ctx_menu_close(void);
bool anx_wm_ctx_menu_active(void);
/* Pass pointer events; returns true if consumed by the menu. */
bool anx_wm_ctx_menu_pointer(int32_t x, int32_t y, uint32_t buttons,
			      bool move_only);

/*
 * Take the cursor sprite off the screen before painting [x, x+w) x [y, y+h),
 * if the two overlap. The WM loop draws it again afterwards. The GPU
 * renderer calls this for every commit, so surface code need not.
 */
void anx_wm_cursor_hide_rect(int32_t x, int32_t y, uint32_t w, uint32_t h);

/*
 * Tell the window manager that something has already painted over the mouse
 * cursor, so it repaints the sprite and forgets its stale save-under pixels.
 * Only direct framebuffer writers that bypass the renderer need this.
 */
void anx_wm_cursor_invalidate(void);

/* Same, but only when [x, x+w) x [y, y+h) overlaps the cursor. */
void anx_wm_cursor_invalidate_rect(int32_t x, int32_t y,
				   uint32_t w, uint32_t h);

#endif /* ANX_WM_H */
