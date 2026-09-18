/*
 * theme.c — Visual theme subsystem (RFC-0019).
 *
 * Manages the global theme configuration and provides the one-toggle
 * switch between Pretty and Boring visual modes. All palette, decoration,
 * and typography defaults live here.
 */

#include <anx/theme.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/types.h>

#ifndef offsetof
#define offsetof(t, m)	__builtin_offsetof(t, m)
#endif

static struct anx_theme g_theme;

/* ------------------------------------------------------------------ */
/* Color schemes                                                        */
/* ------------------------------------------------------------------ */

/*
 * A scheme selects its palette, titlebar controls, and font family. Other decoration
 * settings (corners, shadow, opacity) survive a scheme change.
 */
struct scheme {
	const char *name;
	const char *about;
	enum anx_font_family font_family;
	struct anx_color_palette palette;
};

static const struct scheme schemes[] = {
	{ "default", "Anunix navy and teal", ANX_FONT_ATKINSON, {
		.background = 0x000B1A2B, .surface = 0x00163454,
		.border = 0x003A94A6, .accent = 0x004FB0BF,
		.text_primary = 0x00F7F5F1, .text_dim = 0x007FC9D3,
		.shadow = 0x00050D18,
		.success = 0x002D8A3E, .warning = 0x00D69420,
		.error = 0x00CC3A2A,
		.title_from = 0x001D4470, .title_to = 0x00163454,
		.title_idle = 0x00122B45,
		.bar_from = 0x00163454, .bar_to = 0x000E2338,
		.tab_from = 0x001D4470, .tab_to = 0x00163454,
		.wallpaper_from = 0x000E2338, .wallpaper_to = 0x002F7A8C,
		.btn_close = 0x00EAF4F7, .btn_min = 0x00A9D6DE,
		.btn_max = 0x006FB3C2, .btn_glyph = 0x00A85248 } },
	{ "paper", "Warm daylight, dark text", ANX_FONT_ATKINSON, {
		.background = 0x00E4E0D8, .surface = 0x00FDFCF9,
		.border = 0x00CFCAC0, .accent = 0x003A94A6,
		.text_primary = 0x001A2733, .text_dim = 0x006A7683,
		.shadow = 0x008A95A2,
		.success = 0x002D8A3E, .warning = 0x00C47B10,
		.error = 0x00CC3A2A,
		.title_from = 0x00F7F5F1, .title_to = 0x00E4E0D8,
		.title_idle = 0x00EFECE6,
		.bar_from = 0x00EFECE6, .bar_to = 0x00DAD5CC,
		.tab_from = 0x00FDFCF9, .tab_to = 0x00E4E0D8,
		.wallpaper_from = 0x00CFCAC0, .wallpaper_to = 0x00F7F5F1,
		.btn_close = 0x00FFFFFF, .btn_min = 0x00D7D1C6,
		.btn_max = 0x00AFA89B, .btn_glyph = 0x00A85248 } },
	{ "blackout", "Black, grey and silver", ANX_FONT_ATKINSON, {
		.background = 0x00090909, .surface = 0x00161616,
		.border = 0x004A4A4A, .accent = 0x00C0C0C0,
		.text_primary = 0x00F0F0F0, .text_dim = 0x009A9A9A,
		.shadow = 0x00000000,
		.success = 0x00B8B8B8, .warning = 0x00D0D0D0,
		.error = 0x00A85248,
		.title_from = 0x002C2C2C, .title_to = 0x00161616,
		.title_idle = 0x00121212,
		.bar_from = 0x001E1E1E, .bar_to = 0x000C0C0C,
		.tab_from = 0x002C2C2C, .tab_to = 0x00161616,
		.wallpaper_from = 0x000C0C0C, .wallpaper_to = 0x002A2A2A,
		.btn_close = 0x00DADADA, .btn_min = 0x008C8C8C,
		.btn_max = 0x004E4E4E, .btn_glyph = 0x00A85248 } },
	{ "obsidian", "Near-black with amber accents", ANX_FONT_ATKINSON, {
		.background = 0x00101014, .surface = 0x001A1A20,
		.border = 0x003A3A46, .accent = 0x00E0A030,
		.text_primary = 0x00EDEDF0, .text_dim = 0x009A9AA6,
		.shadow = 0x00000000,
		.success = 0x004CAF50, .warning = 0x00E0A030,
		.error = 0x00E05252,
		.title_from = 0x0026262E, .title_to = 0x001A1A20,
		.title_idle = 0x0018181E,
		.bar_from = 0x001A1A20, .bar_to = 0x00101014,
		.tab_from = 0x0026262E, .tab_to = 0x001A1A20,
		.wallpaper_from = 0x00101014, .wallpaper_to = 0x002A2A34,
		.btn_close = 0x00F2E0C0, .btn_min = 0x00C09A5A,
		.btn_max = 0x007A6236, .btn_glyph = 0x00A85248 } },
	{ "nord", "Cool arctic blues", ANX_FONT_ATKINSON, {
		.background = 0x002E3440, .surface = 0x003B4252,
		.border = 0x00616E88, .accent = 0x0088C0D0,
		.text_primary = 0x00ECEFF4, .text_dim = 0x00A3BE8C,
		.shadow = 0x00121418,
		.success = 0x00A3BE8C, .warning = 0x00EBCB8B,
		.error = 0x00BF616A,
		.title_from = 0x00434C5E, .title_to = 0x003B4252,
		.title_idle = 0x00353B49,
		.bar_from = 0x003B4252, .bar_to = 0x002E3440,
		.tab_from = 0x00434C5E, .tab_to = 0x003B4252,
		.wallpaper_from = 0x002E3440, .wallpaper_to = 0x005E81AC,
		.btn_close = 0x00E5E9F0, .btn_min = 0x00A6BBD0,
		.btn_max = 0x005E7392, .btn_glyph = 0x00A85248 } },
	{ "sunset", "Warm purple to orange", ANX_FONT_ATKINSON, {
		.background = 0x001B1024, .surface = 0x002A1836,
		.border = 0x00E0683C, .accent = 0x00F2994A,
		.text_primary = 0x00FBF1E6, .text_dim = 0x00C9A0B8,
		.shadow = 0x00120A18,
		.success = 0x0057A773, .warning = 0x00F2C94C,
		.error = 0x00EB5757,
		.title_from = 0x003D2148, .title_to = 0x002A1836,
		.title_idle = 0x00241430,
		.bar_from = 0x002A1836, .bar_to = 0x001B1024,
		.tab_from = 0x003D2148, .tab_to = 0x002A1836,
		.wallpaper_from = 0x001B1024, .wallpaper_to = 0x00A24E5E,
		.btn_close = 0x00F8E3EC, .btn_min = 0x00C98CA6,
		.btn_max = 0x008A5570, .btn_glyph = 0x00A85248 } },
	{ "macos", "Light grey chrome, traffic lights", ANX_FONT_CASCADIA, {
		.background = 0x00FFFFFF, .surface = 0x00ECECEC,
		.border = 0x00C8C8C8, .accent = 0x000A84FF,
		.text_primary = 0x001D1D1F, .text_dim = 0x006E6E73,
		.shadow = 0x00505050,
		.success = 0x0028C840, .warning = 0x00FEBC2E,
		.error = 0x00FF5F57,
		.title_from = 0x00F6F6F6, .title_to = 0x00E4E4E4,
		.title_idle = 0x00F0F0F0,
		.bar_from = 0x00F6F6F6, .bar_to = 0x00E8E8E8,
		.tab_from = 0x00FFFFFF, .tab_to = 0x00E8E8E8,
		.wallpaper_from = 0x002B4A73, .wallpaper_to = 0x006E92B8,
		/* The one theme that keeps the traffic light */
		.btn_close = 0x00FF5F57, .btn_min = 0x00FEBC2E,
		.btn_max = 0x0028C840, .btn_glyph = 0x004D0000 } },
	{ "windows", "Light neutral chrome, blue accents, right-side controls", ANX_FONT_CASCADIA, {
		.background = 0x00F3F3F3, .surface = 0x00FFFFFF,
		.border = 0x00909090, .accent = 0x000078D4,
		.text_primary = 0x00191919, .text_dim = 0x00606060,
		.shadow = 0x00202020,
		.success = 0x0010893E, .warning = 0x00F7630C,
		.error = 0x00C42B1C,
		.title_from = 0x00F3F3F3, .title_to = 0x00F3F3F3,
		.title_idle = 0x00EBEBEB,
		.bar_from = 0x00E8E8E8, .bar_to = 0x00E8E8E8,
		.tab_from = 0x00FFFFFF, .tab_to = 0x00FFFFFF,
		.wallpaper_from = 0x00004F9B, .wallpaper_to = 0x0025A8F2,
		.btn_close = 0x00F3F3F3, .btn_min = 0x00F3F3F3,
		.btn_max = 0x00F3F3F3, .btn_glyph = 0x00191919 } },
	{ "omarchy", "Hyprland-style dark with violet accents", ANX_FONT_JETBRAINS, {
		.background = 0x0011111B, .surface = 0x001E1E2E,
		.border = 0x00585B70, .accent = 0x00CBA6F7,
		.text_primary = 0x00CDD6F4, .text_dim = 0x00A6ADC8,
		.shadow = 0x00080810,
		.success = 0x00A6E3A1, .warning = 0x00F9E2AF,
		.error = 0x00F38BA8,
		.title_from = 0x00313244, .title_to = 0x001E1E2E,
		.title_idle = 0x00181825,
		.bar_from = 0x001E1E2E, .bar_to = 0x0011111B,
		.tab_from = 0x00313244, .tab_to = 0x001E1E2E,
		.wallpaper_from = 0x0011111B, .wallpaper_to = 0x00584A7A,
		.btn_close = 0x00EFE3FB, .btn_min = 0x00C0A6E0,
		.btn_max = 0x007E6BA0, .btn_glyph = 0x00A85248 } },
};

#define SCHEME_COUNT (sizeof(schemes) / sizeof(schemes[0]))

const char *anx_theme_scheme_name(uint32_t index)
{
	return index < SCHEME_COUNT ? schemes[index].name : NULL;
}

const char *anx_theme_current_scheme(void)
{
	return g_theme.scheme[0] ? g_theme.scheme : "custom";
}

static int theme_set_scheme(struct anx_theme *theme, const char *name)
{
	uint32_t i;

	if (!name)
		return ANX_EINVAL;
	if (anx_strcmp(name, "aether") == 0)
		name = "default";
	for (i = 0; i < SCHEME_COUNT; i++) {
		if (anx_strcmp(name, schemes[i].name) != 0)
			continue;
		theme->palette = schemes[i].palette;
		theme->font.family = schemes[i].font_family;
		anx_strlcpy(theme->scheme, name, sizeof(theme->scheme));
		theme->deco.controls = anx_strcmp(name, "windows") == 0
			? ANX_CONTROLS_WINDOWS : ANX_CONTROLS_SIGNATURE;
		return ANX_OK;
	}
	return ANX_ENOENT;
}

int anx_theme_set_scheme(const char *name)
{
	return theme_set_scheme(&g_theme, name);
}

/* ------------------------------------------------------------------ */
/* Palette entries by name                                              */
/* ------------------------------------------------------------------ */

static const struct {
	const char *name;
	uint32_t    offset;
} color_slots[] = {
	{ "background",     offsetof(struct anx_color_palette, background) },
	{ "surface",        offsetof(struct anx_color_palette, surface) },
	{ "border",         offsetof(struct anx_color_palette, border) },
	{ "accent",         offsetof(struct anx_color_palette, accent) },
	{ "text_primary",   offsetof(struct anx_color_palette, text_primary) },
	{ "text_dim",       offsetof(struct anx_color_palette, text_dim) },
	/* "shadow" alone is the on/off switch, so the color is explicit */
	{ "shadow_color",   offsetof(struct anx_color_palette, shadow) },
	{ "success",        offsetof(struct anx_color_palette, success) },
	{ "warning",        offsetof(struct anx_color_palette, warning) },
	{ "error",          offsetof(struct anx_color_palette, error) },
	{ "title_from",     offsetof(struct anx_color_palette, title_from) },
	{ "title_to",       offsetof(struct anx_color_palette, title_to) },
	{ "title_idle",     offsetof(struct anx_color_palette, title_idle) },
	{ "bar_from",       offsetof(struct anx_color_palette, bar_from) },
	{ "bar_to",         offsetof(struct anx_color_palette, bar_to) },
	{ "tab_from",       offsetof(struct anx_color_palette, tab_from) },
	{ "tab_to",         offsetof(struct anx_color_palette, tab_to) },
	{ "wallpaper_from", offsetof(struct anx_color_palette, wallpaper_from) },
	{ "wallpaper_to",   offsetof(struct anx_color_palette, wallpaper_to) },
	{ "btn_close",      offsetof(struct anx_color_palette, btn_close) },
	{ "btn_min",        offsetof(struct anx_color_palette, btn_min) },
	{ "btn_max",        offsetof(struct anx_color_palette, btn_max) },
	{ "btn_glyph",      offsetof(struct anx_color_palette, btn_glyph) },
};

#define COLOR_SLOT_COUNT (sizeof(color_slots) / sizeof(color_slots[0]))

/* Colors are hex, with or without a 0x or # prefix: accent=4FB0BF. */
uint32_t anx_theme_parse_color(const char *text)
{
	if (!text)
		return 0;
	if (text[0] == '#')
		text++;
	else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
		text += 2;
	return (uint32_t)anx_strtoull(text, NULL, 16) & 0x00FFFFFF;
}

/* A hand-edited palette is no longer the scheme it came from. */
void anx_theme_mark_custom(void)
{
	anx_strlcpy(g_theme.scheme, "custom", sizeof(g_theme.scheme));
}

uint32_t *anx_theme_color_slot(const char *name)
{
	uint32_t i;

	if (!name)
		return NULL;
	for (i = 0; i < COLOR_SLOT_COUNT; i++) {
		if (anx_strcmp(name, color_slots[i].name) == 0)
			return (uint32_t *)((uint8_t *)&g_theme.palette +
					    color_slots[i].offset);
	}
	return NULL;
}

const char *anx_theme_color_name(uint32_t index)
{
	return index < COLOR_SLOT_COUNT ? color_slots[index].name : NULL;
}

struct anx_shape anx_theme_window_shape(uint32_t radius_override)
{
	uint32_t r = radius_override ? radius_override
				     : g_theme.deco.corner_radius;

	if (r == 0)
		return anx_fb_shape_uniform(0, ANX_CORNER_SQUARE);
	if (g_theme.deco.signature_corners)
		return anx_fb_shape_signature(r);
	return anx_fb_shape_uniform(r, ANX_CORNER_ROUND);
}

/*
 * apply_pretty_defaults — load the Pretty preset into g_theme.
 *
 * Aether design language: deep navy-to-teal gradient palette pulled from
 * the Anunix logo. Floating panels, 14 px corner radius, E17-style bevel
 * highlights. Targets GPU-accelerated display paths.
 */
static void
apply_pretty_defaults(struct anx_theme *theme)
{
	theme->mode = ANX_THEME_PRETTY;

	/* The palette is a named scheme; everything below is the mode */
	theme_set_scheme(theme, "default");

	theme->deco.corner_radius       = 14;    /* --ax-radius: 14 px */
	theme->deco.signature_corners   = true;
	theme->deco.shadow_enabled      = true;
	theme->deco.shadow_offset_x     = 4;
	theme->deco.shadow_offset_y     = 8;
	theme->deco.shadow_blur         = 12;
	theme->deco.animation_enabled   = true;
	theme->deco.animation_ms        = 150;   /* 0.15 s transitions */
	theme->deco.transparency_enabled = false;
	theme->deco.window_opacity      = 255;
	theme->deco.titlebar_height     = 34;    /* --ax-title-h: 34 px */
	theme->deco.show_titlebar       = true;
	theme->deco.wallpaper           = ANX_WALLPAPER_IMAGE;
	theme->deco.bar_opacity         = 235;

	theme->font.scale       = 2;
	theme->font.antialiased = true;
}

/*
 * apply_boring_defaults — load the Boring preset into theme->
 *
 * Monochrome, zero-radius, no shadows, no animation. Optimized for
 * serial TUI consoles and minimal-resource environments.
 */
static void
apply_boring_defaults(struct anx_theme *theme)
{
	theme->mode = ANX_THEME_BORING;
	anx_strlcpy(theme->scheme, "custom", sizeof(theme->scheme));
	theme->deco.controls = ANX_CONTROLS_SIGNATURE;

	theme->palette.background   = 0x00000000;
	theme->palette.surface      = 0x00000000;
	theme->palette.border       = 0x00AAAAAA;
	theme->palette.accent       = 0x00FFFFFF;
	theme->palette.text_primary = 0x00FFFFFF;
	theme->palette.text_dim     = 0x00888888;
	theme->palette.shadow       = 0x00000000;
	theme->palette.success      = 0x00FFFFFF;
	theme->palette.warning      = 0x00FFFFFF;
	theme->palette.error        = 0x00FFFFFF;

	theme->palette.title_from     = 0x00000000;
	theme->palette.title_to       = 0x00000000;
	theme->palette.title_idle     = 0x00000000;
	theme->palette.bar_from       = 0x00000000;
	theme->palette.bar_to         = 0x00000000;
	theme->palette.tab_from       = 0x00000000;
	theme->palette.tab_to         = 0x00000000;
	theme->palette.wallpaper_from = 0x00000000;
	theme->palette.wallpaper_to   = 0x00000000;
	theme->palette.btn_close      = 0x00FFFFFF;
	theme->palette.btn_min        = 0x00AAAAAA;
	theme->palette.btn_max        = 0x00666666;
	theme->palette.btn_glyph      = 0x00000000;

	theme->deco.signature_corners   = false;
	theme->deco.wallpaper           = ANX_WALLPAPER_SOLID;
	theme->deco.bar_opacity         = 255;
	theme->deco.corner_radius       = 0;
	theme->deco.shadow_enabled      = false;
	theme->deco.shadow_offset_x     = 0;
	theme->deco.shadow_offset_y     = 0;
	theme->deco.shadow_blur         = 0;
	theme->deco.animation_enabled   = false;
	theme->deco.animation_ms        = 0;
	theme->deco.transparency_enabled = false;
	theme->deco.window_opacity      = 255;
	theme->deco.titlebar_height     = 16;
	theme->deco.show_titlebar       = true;

	theme->font.family      = ANX_FONT_ATKINSON;
	theme->font.scale       = 1;
	theme->font.antialiased = true;
}

/* Initialize theme subsystem with defaults for the given mode. */
int
anx_theme_init(enum anx_theme_mode mode)
{
	anx_memset(&g_theme, 0, sizeof(g_theme));
	anx_theme_set_mode(mode);
	kprintf("theme: initialized %s\n",
		mode == ANX_THEME_PRETTY ? "pretty" : "boring");
	return ANX_OK;
}

/* Switch to a different mode (Pretty/Boring). Updates all active settings. */
int
anx_theme_set_mode(enum anx_theme_mode mode)
{
	if (mode == ANX_THEME_PRETTY)
		apply_pretty_defaults(&g_theme);
	else
		apply_boring_defaults(&g_theme);

	kprintf("theme: switched to %s\n",
		mode == ANX_THEME_PRETTY ? "pretty" : "boring");
	return ANX_OK;
}

/* Return current mode. */
enum anx_theme_mode
anx_theme_get_mode(void)
{
	return g_theme.mode;
}

/* Return a const pointer to the active theme (never NULL after init). */
const struct anx_theme *
anx_theme_get(void)
{
	return &g_theme;
}

/* Set an individual color in the palette. */
void
anx_theme_set_color(uint32_t *slot, uint32_t color)
{
	*slot = color;
}

/* Restore a previously captured theme snapshot. */
void
anx_theme_restore(const struct anx_theme *snapshot)
{
	g_theme = *snapshot;
	if (anx_strcmp(g_theme.scheme, "aether") == 0)
		anx_strlcpy(g_theme.scheme, "default", sizeof(g_theme.scheme));
}

enum theme_value_type { THEME_U32, THEME_U8, THEME_BOOL };

static const struct {
	const char *name;
	uint32_t offset;
	enum theme_value_type type;
	uint32_t min, max;
} theme_fields[] = {
#define FIELD(name, member, type, min, max) \
	{ name, offsetof(struct anx_theme, member), type, min, max }
	FIELD("corner_radius", deco.corner_radius, THEME_U32, 0, 64),
	FIELD("shadow", deco.shadow_enabled, THEME_BOOL, 0, 1),
	FIELD("shadow_offset_x", deco.shadow_offset_x, THEME_U32, 0, 64),
	FIELD("shadow_offset_y", deco.shadow_offset_y, THEME_U32, 0, 64),
	FIELD("shadow_blur", deco.shadow_blur, THEME_U32, 0, 64),
	FIELD("animation", deco.animation_enabled, THEME_BOOL, 0, 1),
	FIELD("animation_ms", deco.animation_ms, THEME_U32, 0, 10000),
	FIELD("transparency", deco.transparency_enabled, THEME_BOOL, 0, 1),
	FIELD("opacity", deco.window_opacity, THEME_U8, 0, 255),
	FIELD("bar_opacity", deco.bar_opacity, THEME_U8, 0, 255),
	FIELD("titlebar_height", deco.titlebar_height, THEME_U32, 16, 64),
	FIELD("show_titlebar", deco.show_titlebar, THEME_BOOL, 0, 1),
	FIELD("font_scale", font.scale, THEME_U8, 1, 4),
	FIELD("antialiased", font.antialiased, THEME_BOOL, 0, 1),
#undef FIELD
};

#define THEME_FIELD_COUNT (sizeof(theme_fields) / sizeof(theme_fields[0]))

int anx_theme_parse_color_checked(const char *text, uint32_t *color)
{
	uint32_t value = 0, digits = 0;

	if (!text || !color)
		return ANX_EINVAL;
	if (text[0] == '#')
		text++;
	else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
		text += 2;
	while (*text) {
		uint32_t digit;
		char c = *text++;

		if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
		else return ANX_EINVAL;
		if (++digits > 6)
			return ANX_EINVAL;
		value = value * 16 + digit;
	}
	if (digits != 6)
		return ANX_EINVAL;
	*color = value;
	return ANX_OK;
}

static int theme_parse_number(const char *text, uint32_t max, uint32_t *value)
{
	uint32_t v = 0;

	if (!*text)
		return ANX_EINVAL;
	for (; *text; text++) {
		uint32_t digit = (uint32_t)(*text - '0');

		if (digit > 9 || digit > max || v > (max - digit) / 10)
			return ANX_EINVAL;
		v = v * 10 + digit;
	}
	*value = v;
	return ANX_OK;
}

static int theme_apply_pair(struct anx_theme *theme, const char *key,
			    const char *value)
{
	uint32_t i, v;

	if (anx_strcmp(key, "mode") == 0) {
		if (anx_strcmp(value, "pretty") == 0)
			apply_pretty_defaults(theme);
		else if (anx_strcmp(value, "boring") == 0)
			apply_boring_defaults(theme);
		else
			return ANX_EINVAL;
		return ANX_OK;
	}
	if (anx_strcmp(key, "scheme") == 0) {
		if (anx_strcmp(value, "custom") != 0)
			return theme_set_scheme(theme, value);
		anx_strlcpy(theme->scheme, "custom", sizeof(theme->scheme));
		return ANX_OK;
	}
	if (anx_strcmp(key, "font_family") == 0)
		return anx_font_family_parse(value, &theme->font.family);
	if (anx_strcmp(key, "corners") == 0) {
		if (anx_strcmp(value, "signature") == 0)
			theme->deco.signature_corners = true;
		else if (anx_strcmp(value, "round") == 0)
			theme->deco.signature_corners = false;
		else if (anx_strcmp(value, "square") == 0) {
			theme->deco.signature_corners = false;
			theme->deco.corner_radius = 0;
		} else
			return ANX_EINVAL;
		return ANX_OK;
	}
	if (anx_strcmp(key, "controls") == 0) {
		if (anx_strcmp(value, "signature") == 0)
			theme->deco.controls = ANX_CONTROLS_SIGNATURE;
		else if (anx_strcmp(value, "windows") == 0)
			theme->deco.controls = ANX_CONTROLS_WINDOWS;
		else
			return ANX_EINVAL;
		return ANX_OK;
	}
	if (anx_strcmp(key, "wallpaper") == 0) {
		if (anx_strcmp(value, "solid") == 0)
			theme->deco.wallpaper = ANX_WALLPAPER_SOLID;
		else if (anx_strcmp(value, "gradient") == 0)
			theme->deco.wallpaper = ANX_WALLPAPER_GRADIENT;
		else if (anx_strcmp(value, "image") == 0)
			theme->deco.wallpaper = ANX_WALLPAPER_IMAGE;
		else
			return ANX_EINVAL;
		return ANX_OK;
	}
	for (i = 0; i < THEME_FIELD_COUNT; i++) {
		uint8_t *slot;

		if (anx_strcmp(key, theme_fields[i].name) != 0)
			continue;
		if (theme_fields[i].type == THEME_BOOL) {
			if (anx_strcmp(value, "true") == 0) v = 1;
			else if (anx_strcmp(value, "false") == 0) v = 0;
			else return ANX_EINVAL;
		} else if (theme_parse_number(value, theme_fields[i].max, &v)
			   != ANX_OK || v < theme_fields[i].min)
			return ANX_EINVAL;
		slot = (uint8_t *)theme + theme_fields[i].offset;
		if (theme_fields[i].type == THEME_U32)
			*(uint32_t *)slot = v;
		else if (theme_fields[i].type == THEME_BOOL)
			*(bool *)slot = v != 0;
		else
			*slot = (uint8_t)v;
		return ANX_OK;
	}
	for (i = 0; i < COLOR_SLOT_COUNT; i++) {
		uint32_t *slot;

		if (anx_strcmp(key, color_slots[i].name) != 0)
			continue;
		if (anx_theme_parse_color_checked(value, &v) != ANX_OK)
			return ANX_EINVAL;
		slot = (uint32_t *)((uint8_t *)&theme->palette + color_slots[i].offset);
		if (*slot != v)
			anx_strlcpy(theme->scheme, "custom", sizeof(theme->scheme));
		*slot = v;
		return ANX_OK;
	}
	return ANX_EINVAL;
}

int anx_theme_apply_config_checked(const char *text)
{
	struct anx_theme next = g_theme;
	bool any = false;

	if (!text)
		return ANX_EINVAL;
	while (*text) {
		char pair[96], *eq;
		uint32_t len = 0;
		int rc;

		while (*text == ';' || *text == '\n' || *text == '\r')
			text++;
		if (!*text)
			break;
		while (*text && *text != ';' && *text != '\n' && *text != '\r') {
			if (len + 1 == sizeof(pair))
				return ANX_EINVAL;
			pair[len++] = *text++;
		}
		pair[len] = '\0';
		eq = pair;
		while (*eq && *eq != '=')
			eq++;
		if (!*eq || eq == pair || !eq[1])
			return ANX_EINVAL;
		*eq++ = '\0';
		rc = theme_apply_pair(&next, pair, eq);
		if (rc != ANX_OK)
			return rc;
		any = true;
	}
	if (!any)
		return ANX_EINVAL;
	g_theme = next;
	return ANX_OK;
}

int anx_theme_apply_config(const char *config_str)
{
	return anx_theme_apply_config_checked(config_str);
}

static int theme_append(char *buf, uint32_t cap, uint32_t *used,
			const char *key, const char *value)
{
	uint32_t len = (uint32_t)(anx_strlen(key) + anx_strlen(value) + 2);

	/* anx_snprintf reports bytes written, so check room before formatting. */
	if (len >= cap - *used)
		return ANX_EFULL;
	anx_snprintf(buf + *used, cap - *used, "%s=%s\n", key, value);
	*used += len;
	return ANX_OK;
}

int anx_theme_serialize(char *buf, uint32_t cap)
{
	const struct anx_theme *t = &g_theme;
	uint32_t i, used = 0;
	char value[16];

	if (!buf || cap == 0)
		return ANX_EINVAL;
	buf[0] = '\0';
#define APPEND(key, text) do { \
	if (theme_append(buf, cap, &used, key, text) != ANX_OK) \
		return ANX_EFULL; \
} while (0)
	APPEND("mode", t->mode == ANX_THEME_PRETTY ? "pretty" : "boring");
	APPEND("scheme", anx_theme_current_scheme());
	APPEND("font_family", anx_font_family_name(t->font.family));
	APPEND("controls", t->deco.controls == ANX_CONTROLS_WINDOWS
		? "windows" : "signature");
	APPEND("corners", t->deco.signature_corners ? "signature" : "round");
	APPEND("wallpaper", t->deco.wallpaper == ANX_WALLPAPER_IMAGE ? "image" :
		t->deco.wallpaper == ANX_WALLPAPER_SOLID ? "solid" : "gradient");
	for (i = 0; i < THEME_FIELD_COUNT; i++) {
		const uint8_t *slot = (const uint8_t *)t + theme_fields[i].offset;
		uint32_t v;

		if (theme_fields[i].type == THEME_U32)
			v = *(const uint32_t *)slot;
		else if (theme_fields[i].type == THEME_BOOL)
			v = *(const bool *)slot;
		else
			v = *slot;
		if (theme_fields[i].type == THEME_BOOL)
			anx_strlcpy(value, v ? "true" : "false", sizeof(value));
		else
			anx_snprintf(value, sizeof(value), "%u", v);
		APPEND(theme_fields[i].name, value);
	}
	for (i = 0; i < COLOR_SLOT_COUNT; i++) {
		const uint32_t *slot = (const uint32_t *)
			((const uint8_t *)&t->palette + color_slots[i].offset);

		anx_snprintf(value, sizeof(value), "%06x", *slot);
		APPEND(color_slots[i].name, value);
	}
#undef APPEND
	return (int)used;
}
