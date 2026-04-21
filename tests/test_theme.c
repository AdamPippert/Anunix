/*
 * test_theme.c — Host-native tests for the visual theme subsystem (RFC-0019).
 *
 * Verifies Pretty/Boring mode switching, palette values, decoration
 * fields, and config string parsing.
 */

#include <anx/types.h>
#include <anx/theme.h>
#include <anx/string.h>

int test_theme(void)
{
	const struct anx_theme *t;

	/* Test 1: init pretty */
	anx_theme_init(ANX_THEME_PRETTY);
	if (anx_theme_get_mode() != ANX_THEME_PRETTY) return -1;
	t = anx_theme_get();
	if (!t) return -2;
	if (t->deco.corner_radius == 0) return -3;
	if (!t->deco.shadow_enabled) return -4;

	/* Test 2: switch to boring */
	anx_theme_set_mode(ANX_THEME_BORING);
	if (anx_theme_get_mode() != ANX_THEME_BORING) return -5;
	t = anx_theme_get();
	if (t->deco.corner_radius != 0) return -6;
	if (t->deco.shadow_enabled) return -7;

	/* Test 3: switch back */
	anx_theme_set_mode(ANX_THEME_PRETTY);
	if (anx_theme_get_mode() != ANX_THEME_PRETTY) return -8;

	/* Test 4: apply config string */
	anx_theme_apply_config("corner_radius=12;shadow=false;font_scale=3");
	t = anx_theme_get();
	if (t->deco.corner_radius != 12) return -9;
	if (t->deco.shadow_enabled) return -10;
	if (t->font.scale != 3) return -11;

	/* Test 5: boring has no shadow */
	anx_theme_set_mode(ANX_THEME_BORING);
	t = anx_theme_get();
	if (t->palette.background != 0x00000000) return -12;

	return 0;
}
