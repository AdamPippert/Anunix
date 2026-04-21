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

static struct anx_theme g_theme;

/*
 * apply_pretty_defaults — load the Pretty preset into g_theme.
 *
 * Aether design language: deep navy-to-teal gradient palette pulled from
 * the Anunix logo. Floating panels, 14 px corner radius, E17-style bevel
 * highlights. Targets GPU-accelerated display paths.
 */
static void
apply_pretty_defaults(void)
{
	g_theme.mode = ANX_THEME_PRETTY;

	/* Navy-to-teal brand palette */
	g_theme.palette.background   = 0x000B1A2B;  /* deep navy: desktop */
	g_theme.palette.surface      = 0x00163454;  /* navy-800: panels */
	g_theme.palette.border       = 0x003A94A6;  /* teal-500: accent border */
	g_theme.palette.accent       = 0x004FB0BF;  /* teal-400: active elements */
	g_theme.palette.text_primary = 0x00F7F5F1;  /* warm paper-white */
	g_theme.palette.text_dim     = 0x007FC9D3;  /* teal-300: muted */
	g_theme.palette.shadow       = 0xCC0B1A2B;  /* deep navy with alpha */
	g_theme.palette.success      = 0x002D8A3E;  /* traffic-light green */
	g_theme.palette.warning      = 0x00D69420;  /* traffic-light amber */
	g_theme.palette.error        = 0x00CC3A2A;  /* traffic-light red */

	g_theme.deco.corner_radius       = 14;    /* --ax-radius: 14 px */
	g_theme.deco.shadow_enabled      = true;
	g_theme.deco.shadow_offset_x     = 4;
	g_theme.deco.shadow_offset_y     = 8;
	g_theme.deco.shadow_blur         = 12;
	g_theme.deco.animation_enabled   = true;
	g_theme.deco.animation_ms        = 150;   /* 0.15 s transitions */
	g_theme.deco.transparency_enabled = false;
	g_theme.deco.window_opacity      = 255;
	g_theme.deco.titlebar_height     = 34;    /* --ax-title-h: 34 px */
	g_theme.deco.show_titlebar       = true;

	g_theme.font.scale       = 2;
	g_theme.font.antialiased = false;
}

/*
 * apply_boring_defaults — load the Boring preset into g_theme.
 *
 * Monochrome, zero-radius, no shadows, no animation. Optimized for
 * serial TUI consoles and minimal-resource environments.
 */
static void
apply_boring_defaults(void)
{
	g_theme.mode = ANX_THEME_BORING;

	g_theme.palette.background   = 0x00000000;
	g_theme.palette.surface      = 0x00000000;
	g_theme.palette.border       = 0x00AAAAAA;
	g_theme.palette.accent       = 0x00FFFFFF;
	g_theme.palette.text_primary = 0x00FFFFFF;
	g_theme.palette.text_dim     = 0x00888888;
	g_theme.palette.shadow       = 0x00000000;
	g_theme.palette.success      = 0x00FFFFFF;
	g_theme.palette.warning      = 0x00FFFFFF;
	g_theme.palette.error        = 0x00FFFFFF;

	g_theme.deco.corner_radius       = 0;
	g_theme.deco.shadow_enabled      = false;
	g_theme.deco.shadow_offset_x     = 0;
	g_theme.deco.shadow_offset_y     = 0;
	g_theme.deco.shadow_blur         = 0;
	g_theme.deco.animation_enabled   = false;
	g_theme.deco.animation_ms        = 0;
	g_theme.deco.transparency_enabled = false;
	g_theme.deco.window_opacity      = 255;
	g_theme.deco.titlebar_height     = 16;
	g_theme.deco.show_titlebar       = true;

	g_theme.font.scale       = 1;
	g_theme.font.antialiased = false;
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
		apply_pretty_defaults();
	else
		apply_boring_defaults();

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

/*
 * anx_theme_apply_config — parse semicolon-separated key=value config string.
 *
 * Used by kickstart provisioning to configure the theme from /boot/kickstart.cfg.
 * Modifies g_theme in place; unknown keys are silently skipped.
 */
int
anx_theme_apply_config(const char *config_str)
{
	/* Work through the string one pair at a time. We copy each pair into a
	 * small stack buffer to avoid modifying the caller's string. */
	const char *p = config_str;

	if (!p)
		return ANX_EINVAL;

	while (*p) {
		char pair[64];
		const char *semi;
		const char *eq;
		size_t pair_len;
		const char *key;
		const char *val;

		/* Find end of this pair */
		semi = p;
		while (*semi && *semi != ';')
			semi++;

		pair_len = (size_t)(semi - p);
		if (pair_len == 0) {
			/* Empty segment — skip */
			if (*semi == ';')
				p = semi + 1;
			else
				break;
			continue;
		}
		if (pair_len >= sizeof(pair))
			pair_len = sizeof(pair) - 1;

		anx_memcpy(pair, p, pair_len);
		pair[pair_len] = '\0';

		/* Split on '=' */
		eq = pair;
		while (*eq && *eq != '=')
			eq++;
		if (*eq != '=') {
			/* No '=' — skip malformed pair */
			goto next;
		}

		/* Temporarily terminate key, point val past '=' */
		*((char *)eq) = '\0';
		key = pair;
		val = eq + 1;

		if (anx_strcmp(key, "mode") == 0) {
			if (anx_strcmp(val, "pretty") == 0)
				anx_theme_set_mode(ANX_THEME_PRETTY);
			else if (anx_strcmp(val, "boring") == 0)
				anx_theme_set_mode(ANX_THEME_BORING);
		} else if (anx_strcmp(key, "corner_radius") == 0) {
			g_theme.deco.corner_radius =
				anx_strtoul(val, NULL, 10);
		} else if (anx_strcmp(key, "shadow") == 0) {
			g_theme.deco.shadow_enabled =
				(anx_strcmp(val, "true") == 0);
		} else if (anx_strcmp(key, "animation") == 0) {
			g_theme.deco.animation_enabled =
				(anx_strcmp(val, "true") == 0);
		} else if (anx_strcmp(key, "opacity") == 0) {
			uint32_t v = anx_strtoul(val, NULL, 10);
			g_theme.deco.window_opacity = (uint8_t)(v > 255 ? 255 : v);
		} else if (anx_strcmp(key, "font_scale") == 0) {
			uint32_t v = anx_strtoul(val, NULL, 10);
			if (v < 1) v = 1;
			if (v > 4) v = 4;
			g_theme.font.scale = (uint8_t)v;
		}
		/* Unknown keys are silently skipped. */

next:
		if (*semi == ';')
			p = semi + 1;
		else
			break;
	}

	return ANX_OK;
}
