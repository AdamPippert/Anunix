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
#include <anx/string.h>
#include <anx/kprintf.h>

static void
print_usage(void)
{
	kprintf("usage: theme <pretty|boring|status|set key=value[;...]>\n");
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
	kprintf("  scale:        %u\n",  (uint32_t)t->font.scale);
	kprintf("  antialiased:  %s\n",  t->font.antialiased ? "yes" : "no");
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
		kprintf("theme: switched to pretty mode\n");
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "boring") == 0) {
		anx_theme_set_mode(ANX_THEME_BORING);
		kprintf("theme: switched to boring mode\n");
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "status") == 0) {
		print_status();
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "set") == 0) {
		if (argc < 3) {
			kprintf("theme set: missing config string\n");
			return ANX_EINVAL;
		}
		anx_theme_apply_config(argv[2]);
		kprintf("ok\n");
		return ANX_OK;
	}

	print_usage();
	return ANX_EINVAL;
}
