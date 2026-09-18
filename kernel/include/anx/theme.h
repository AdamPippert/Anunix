/*
 * anx/theme.h — Visual theme subsystem (RFC-0019).
 *
 * One-toggle switch between Pretty (GPU-accelerated decorations) and
 * Boring (high-performance TUI) visual modes. All renderers query
 * anx_theme_get() before drawing decorations.
 */

#ifndef ANX_THEME_H
#define ANX_THEME_H

#include <anx/types.h>
#include <anx/fb.h>
#include <anx/font.h>

/* Visual presentation modes */
enum anx_theme_mode {
	ANX_THEME_PRETTY,	/* rounded corners, shadows, gradients, animations */
	ANX_THEME_BORING,	/* monochrome, flat borders, TUI-optimized */
};

/* How the desktop behind the windows is painted */
enum anx_wallpaper_mode {
	ANX_WALLPAPER_SOLID = 0,	/* palette.background */
	ANX_WALLPAPER_GRADIENT,		/* wallpaper_from -> wallpaper_to */
	ANX_WALLPAPER_IMAGE,		/* a State Object, gradient if absent */
};

/* Titlebar control placement and glyph style. */
enum anx_window_controls {
	ANX_CONTROLS_SIGNATURE = 0,
	ANX_CONTROLS_WINDOWS,
};

/* Color palette — 32-bit 0x00RRGGBB format */
struct anx_color_palette {
	uint32_t background;	/* main bg */
	uint32_t surface;	/* window/card bg */
	uint32_t border;	/* window border */
	uint32_t accent;	/* highlight/selection */
	uint32_t text_primary;	/* main text */
	uint32_t text_dim;	/* secondary text */
	uint32_t shadow;	/* drop shadow */
	uint32_t success;	/* positive indicator */
	uint32_t warning;	/* caution indicator */
	uint32_t error;		/* error indicator */

	/* Chrome gradients: titlebars, the taskbar and its tabs */
	uint32_t title_from;	/* focused titlebar, top */
	uint32_t title_to;	/* focused titlebar, bottom */
	uint32_t title_idle;	/* unfocused titlebar */
	uint32_t bar_from;	/* menu bar and taskbar, top */
	uint32_t bar_to;	/* menu bar and taskbar, bottom */
	uint32_t tab_from;	/* taskbar tab, top */
	uint32_t tab_to;	/* taskbar tab, bottom */
	uint32_t wallpaper_from;	/* desktop gradient, top */
	uint32_t wallpaper_to;		/* desktop gradient, bottom */

	/*
	 * Window buttons. Most themes make these tints of their own
	 * colors rather than a traffic light, lightest at the left: the
	 * close button is almost white and carries a dusty-red mark. A
	 * theme that imitates macOS sets the familiar red, amber, green.
	 */
	uint32_t btn_close;
	uint32_t btn_min;
	uint32_t btn_max;
	uint32_t btn_glyph;	/* the mark on the close button */
};

/* Window decoration style */
struct anx_deco_style {
	uint32_t corner_radius;		/* pixels; 0 = square corners */
	bool     signature_corners;	/* round UL and LR, mitre UR and LL */
	bool     shadow_enabled;
	uint32_t shadow_offset_x;	/* pixels */
	uint32_t shadow_offset_y;
	uint32_t shadow_blur;		/* 0 = hard shadow */
	bool     animation_enabled;
	uint32_t animation_ms;		/* transition duration */
	bool     transparency_enabled;
	uint8_t  window_opacity;	/* 0-255; 255 = fully opaque */
	uint32_t titlebar_height;	/* pixels */
	bool     show_titlebar;
	enum anx_wallpaper_mode wallpaper;
	uint8_t  bar_opacity;		/* menu bar and taskbar, 0-255 */
	enum anx_window_controls controls;
};

/* Typography */
struct anx_font_style {
	enum anx_font_family family;	/* embedded face used by GUI text */
	uint8_t  scale;		/* 1-4; multiplier for the bitmap font */
	bool     antialiased;	/* blend glyph coverage; false thresholds it */
};

/* The complete theme configuration */
struct anx_theme {
	enum anx_theme_mode      mode;
	char                     scheme[24];	/* name of the color scheme */
	struct anx_color_palette palette;
	struct anx_deco_style    deco;
	struct anx_font_style    font;
};

/* Initialize theme subsystem with defaults for the given mode. */
int anx_theme_init(enum anx_theme_mode mode);

/* Switch to a different mode (Pretty/Boring). Updates all active settings. */
int anx_theme_set_mode(enum anx_theme_mode mode);

/* Return current mode. */
enum anx_theme_mode anx_theme_get_mode(void);

/* Return a const pointer to the active theme (never NULL after init). */
const struct anx_theme *anx_theme_get(void);

/* Set an individual color in the palette. */
void anx_theme_set_color(uint32_t *slot, uint32_t color);

/*
 * Named color schemes. anx_theme_scheme_name() walks them from 0 until
 * it returns NULL; anx_theme_set_scheme() loads one by name and selects its
 * control style and font family. "aether" remains an alias for the canonical "default".
 */
const char *anx_theme_scheme_name(uint32_t index);
int anx_theme_set_scheme(const char *name);
const char *anx_theme_current_scheme(void);

/*
 * Look up a palette entry by name ("accent", "title_from", ...) so the
 * shell and the color editor can read and write colors without knowing
 * the struct layout. Returns NULL when the name is unknown.
 */
uint32_t *anx_theme_color_slot(const char *name);

/* Mark the palette as hand-edited rather than a named scheme. */
void anx_theme_mark_custom(void);

/* Parse a hex color with an optional 0x or # prefix. */
uint32_t anx_theme_parse_color(const char *text);
const char *anx_theme_color_name(uint32_t index);

/* The window shape the deco style asks for, ready for anx_fb_fill_shape. */
struct anx_shape anx_theme_window_shape(uint32_t radius_override);

/* Apply settings from a serialized config string (kickstart integration).
 * Format: "key=value" pairs separated by semicolons.
 * Keys: mode, scheme, corner_radius, corners, shadow, shadow_blur,
 * animation, opacity, bar_opacity, transparency, titlebar_height,
 * font_family, font_scale, antialiased, wallpaper, controls (signature/windows), and any palette color by name
 * (background, surface, border, accent, text_primary, text_dim,
 * shadow_color, success, warning, error, title_from, title_to,
 * title_idle, bar_from,
 * bar_to, tab_from, tab_to, wallpaper_from, wallpaper_to). */
int anx_theme_apply_config(const char *config_str);

/* Validate every key/value before changing the active theme; supports newlines. */
int anx_theme_apply_config_checked(const char *text);

/* Serialize all settings as key=value lines; return bytes or a negative error. */
int anx_theme_serialize(char *buf, uint32_t cap);

/* Parse exactly six hex digits (optional #/0x prefix), leaving output on error. */
int anx_theme_parse_color_checked(const char *text, uint32_t *color);

/* Restore a previously captured theme snapshot into the active theme. */
void anx_theme_restore(const struct anx_theme *snapshot);

#endif /* ANX_THEME_H */
