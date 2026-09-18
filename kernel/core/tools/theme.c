/*
 * tools/theme.c — Shell builtin for theme control (RFC-0019).
 *
 * USAGE
 *   theme pretty               Switch to Pretty (GPU-accelerated) mode
 *   theme boring               Switch to Boring (TUI-optimized) mode
 *   theme status               Print current theme settings
 *   theme set key=value[;...]  Apply config string (kickstart format)
 */

#include <anx/types.h>
#include <anx/theme.h>
#include <anx/config.h>
#include <anx/wm.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/string.h>
#include <anx/kprintf.h>

static void
print_usage(void)
{
	kprintf("usage: theme <subcommand>\n");
	kprintf("  status | pretty | boring\n");
	kprintf("  list                      named color schemes\n");
	kprintf("  use <scheme>              load one\n");
	kprintf("  colors                    every palette entry\n");
	kprintf("  fonts                     available font families\n");
	kprintf("  font [family]             show or select a font\n");
	kprintf("  color <name> [hex]        read or set one color\n");
	kprintf("  set key=value[;...]       any setting (see 'help system')\n");
	kprintf("  save | load               keep it in the object store\n");
}

static void
print_status(void)
{
	const struct anx_theme *t = anx_theme_get();

	kprintf("mode:           %s\n",
		t->mode == ANX_THEME_PRETTY ? "pretty" : "boring");

	kprintf("\npalette:\n");
	kprintf("  background:   0x%08x\n", t->palette.background);
	kprintf("  surface:      0x%08x\n", t->palette.surface);
	kprintf("  border:       0x%08x\n", t->palette.border);
	kprintf("  accent:       0x%08x\n", t->palette.accent);
	kprintf("  text_primary: 0x%08x\n", t->palette.text_primary);
	kprintf("  text_dim:     0x%08x\n", t->palette.text_dim);
	kprintf("  shadow:       0x%08x\n", t->palette.shadow);
	kprintf("  success:      0x%08x\n", t->palette.success);
	kprintf("  warning:      0x%08x\n", t->palette.warning);
	kprintf("  error:        0x%08x\n", t->palette.error);

	kprintf("\ndecorations:\n");
	kprintf("  corner_radius:    %u px\n",  t->deco.corner_radius);
	kprintf("  shadow:           %s\n",     t->deco.shadow_enabled ? "on" : "off");
	kprintf("  shadow_offset:    %u,%u px\n",
		t->deco.shadow_offset_x, t->deco.shadow_offset_y);
	kprintf("  shadow_blur:      %u px\n",  t->deco.shadow_blur);
	kprintf("  animation:        %s\n",     t->deco.animation_enabled ? "on" : "off");
	kprintf("  animation_ms:     %u\n",     t->deco.animation_ms);
	kprintf("  transparency:     %s\n",     t->deco.transparency_enabled ? "on" : "off");
	kprintf("  window_opacity:   %u\n",     (uint32_t)t->deco.window_opacity);
	kprintf("  titlebar_height:  %u px\n",  t->deco.titlebar_height);
	kprintf("  show_titlebar:    %s\n",     t->deco.show_titlebar ? "yes" : "no");

	kprintf("\nfont:\n");
	kprintf("  family:       %s\n", anx_font_family_name(t->font.family));
	kprintf("  scale:        %u\n",  (uint32_t)t->font.scale);
	kprintf("  antialiased:  %s\n",  t->font.antialiased ? "yes" : "no");
}

/*
 * The appearance lives in one State Object, so it can be read, copied
 * and edited like anything else in the store, and it comes back after a
 * reboot. `theme save` writes it; the desktop loads it at start-up.
 */
#define THEME_PATH	"system:config/theme"

int anx_theme_save(void)
{
	return anx_config_save("theme");
}

int anx_theme_load(void)
{
	return anx_config_load("theme");
}

int
cmd_theme(int argc, char **argv)
{
	if (argc < 2) {
		print_usage();
		return ANX_EINVAL;
	}

	if (anx_strcmp(argv[1], "pretty") == 0) {
		anx_theme_set_mode(ANX_THEME_PRETTY);
		anx_wm_repaint_all();
		kprintf("theme: switched to pretty mode\n");
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "boring") == 0) {
		anx_theme_set_mode(ANX_THEME_BORING);
		anx_wm_repaint_all();
		kprintf("theme: switched to boring mode\n");
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "status") == 0) {
		print_status();
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "fonts") == 0 && argc == 2) {
		uint32_t i;
		for (i = 0; i < ANX_FONT_FAMILY_COUNT; i++)
			kprintf("  %s%s\n", anx_font_family_name((enum anx_font_family)i),
				anx_theme_get()->font.family == (enum anx_font_family)i
				? " (current)" : "");
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "font") == 0) {
		enum anx_font_family family;
		char setting[80];
		int rc;
		if (argc == 2) {
			kprintf("%s\n", anx_font_family_name(anx_theme_get()->font.family));
			return ANX_OK;
		}
		if (argc != 3) {
			kprintf("usage: theme font [family]\n");
			return ANX_EINVAL;
		}
		rc = anx_font_family_parse(argv[2], &family);
		if (rc != ANX_OK) {
			kprintf("theme: unknown font '%s' (try 'theme fonts')\n", argv[2]);
			return rc;
		}
		anx_snprintf(setting, sizeof(setting), "font_family=%s",
			     anx_font_family_name(family));
		rc = anx_theme_apply_config_checked(setting);
		if (rc != ANX_OK)
			return rc;
		anx_wm_repaint_all();
		kprintf("theme: font %s (theme save to keep)\n", anx_font_family_name(family));
		return ANX_OK;
	}

	/* theme list — the named color schemes */
	if (anx_strcmp(argv[1], "list") == 0) {
		const char *name;
		uint32_t i;

		for (i = 0; (name = anx_theme_scheme_name(i)) != NULL; i++)
			kprintf("  %-10s%s\n", name,
				anx_strcmp(name, anx_theme_current_scheme()) == 0
				? "  (current)" : "");
		return ANX_OK;
	}

	/* theme use <scheme> */
	if (anx_strcmp(argv[1], "use") == 0) {
		if (argc < 3) {
			kprintf("usage: theme use <scheme>\n");
			return ANX_EINVAL;
		}
		if (anx_theme_set_scheme(argv[2]) != ANX_OK) {
			kprintf("theme: no scheme '%s' (try 'theme list')\n",
				argv[2]);
			return ANX_ENOENT;
		}
		kprintf("theme: scheme %s\n", argv[2]);
		anx_wm_repaint_all();
		return ANX_OK;
	}

	/* theme colors — every palette entry and its hex value */
	if (anx_strcmp(argv[1], "colors") == 0) {
		const char *name;
		uint32_t i;

		for (i = 0; (name = anx_theme_color_name(i)) != NULL; i++) {
			uint32_t *slot = anx_theme_color_slot(name);

			kprintf("  %-14s %06x\n", name, slot ? *slot : 0);
		}
		return ANX_OK;
	}

	/* theme color <name> [hex] */
	if (anx_strcmp(argv[1], "color") == 0) {
		uint32_t *slot;

		if (argc < 3) {
			kprintf("usage: theme color <name> [hex]\n");
			return ANX_EINVAL;
		}
		slot = anx_theme_color_slot(argv[2]);
		if (!slot) {
			kprintf("theme: no color '%s' (try 'theme colors')\n",
				argv[2]);
			return ANX_ENOENT;
		}
		if (argc < 4) {
			kprintf("%06x\n", *slot);
			return ANX_OK;
		}
		*slot = anx_theme_parse_color(argv[3]);
		anx_theme_mark_custom();
		kprintf("theme: %s = %06x\n", argv[2], *slot);
		anx_wm_repaint_all();
		return ANX_OK;
	}

	/* theme save | load — the appearance as a State Object */
	if (anx_strcmp(argv[1], "save") == 0) {
		int rc = anx_theme_save();

		if (rc != ANX_OK) {
			kprintf("theme save: failed (%d)\n", rc);
			return rc;
		}
		kprintf("theme: saved to %s\n", THEME_PATH);
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "load") == 0) {
		int rc = anx_theme_load();

		if (rc != ANX_OK) {
			kprintf("theme load: nothing saved (%d)\n", rc);
			return rc;
		}
		kprintf("theme: loaded %s\n", THEME_PATH);
		anx_wm_repaint_all();
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "set") == 0) {
		if (argc < 3) {
			kprintf("theme set: missing config string\n");
			return ANX_EINVAL;
		}
		int rc = anx_theme_apply_config_checked(argv[2]);
		if (rc != ANX_OK) {
			kprintf("theme set: invalid configuration (%d)\n", rc);
			return rc;
		}
		anx_wm_repaint_all();
		kprintf("ok\n");
		return ANX_OK;
	}

	print_usage();
	return ANX_EINVAL;
}

/*
 * wallpaper — point the desktop at an image object, or drop back to the
 * theme's gradient. The object holds "ANWP", width, height, then ARGB
 * pixels, so it can be produced by anything that can write an object.
 */
int
cmd_wallpaper(int argc, char **argv)
{
	int rc;

	if (argc < 2) {
		kprintf("usage: wallpaper <ns:path>|default|none|gradient|solid\n");
		const struct anx_theme *theme = anx_theme_get();
		kprintf("  current: %s\n", theme->deco.wallpaper == ANX_WALLPAPER_SOLID
			? "solid" : theme->deco.wallpaper == ANX_WALLPAPER_GRADIENT
			? "gradient" : anx_wm_wallpaper_ready() ? "image" : "image unavailable");
		return ANX_EINVAL;
	}

	if (anx_strcmp(argv[1], "gradient") == 0 ||
	    anx_strcmp(argv[1], "solid") == 0) {
		anx_theme_apply_config(anx_strcmp(argv[1], "solid") == 0
				       ? "wallpaper=solid" : "wallpaper=gradient");
		anx_wm_repaint_all();
		kprintf("wallpaper: %s\n", argv[1]);
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "default") == 0) {
		anx_wm_wallpaper_set(NULL);
		anx_theme_apply_config("wallpaper=image");
		anx_wm_repaint_all();
		kprintf("wallpaper: default photograph\n");
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "none") == 0) {
		anx_wm_wallpaper_set(NULL);
		anx_theme_apply_config("wallpaper=gradient");
		anx_wm_repaint_all();
		kprintf("wallpaper: cleared\n");
		return ANX_OK;
	}

	rc = anx_wm_wallpaper_set(argv[1]);
	if (rc != ANX_OK) {
		kprintf("wallpaper: '%s' is not an ANWP image (%d)\n",
			argv[1], rc);
		return rc;
	}
	anx_theme_apply_config("wallpaper=image");
	anx_wm_repaint_all();
	kprintf("wallpaper: %s\n", argv[1]);
	return ANX_OK;
}
