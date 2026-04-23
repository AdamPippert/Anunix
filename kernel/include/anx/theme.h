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

/* Visual presentation modes */
enum anx_theme_mode {
	ANX_THEME_PRETTY,	/* rounded corners, shadows, gradients, animations */
	ANX_THEME_BORING,	/* monochrome, flat borders, TUI-optimized */
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
};

/* Window decoration style */
struct anx_deco_style {
	uint32_t corner_radius;		/* pixels; 0 = square corners */
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
};

/* Typography */
struct anx_font_style {
	uint8_t  scale;		/* 1-4; multiplier for the bitmap font */
	bool     antialiased;	/* Phase 2: subpixel rendering */
};

/* The complete theme configuration */
struct anx_theme {
	enum anx_theme_mode      mode;
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

/* Apply settings from a serialized config string (kickstart integration).
 * Format: "key=value" pairs separated by semicolons.
 * Keys: mode, corner_radius, shadow, animation, opacity, font_scale */
int anx_theme_apply_config(const char *config_str);

/* Restore a previously captured theme snapshot into the active theme. */
void anx_theme_restore(const struct anx_theme *snapshot);

#endif /* ANX_THEME_H */
