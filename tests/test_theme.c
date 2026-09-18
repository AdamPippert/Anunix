/*
 * test_theme.c — Host-native tests for the visual theme subsystem (RFC-0019).
 *
 * Verifies Pretty/Boring mode switching, palette values, decoration
 * fields, and config string parsing.
 */

#include <anx/types.h>
#include <anx/theme.h>
#include <anx/string.h>
#include <anx/tools.h>
#include <anx/mock_blk.h>
#include <anx/objstore_disk.h>

#define CHECK_FONT(expr) do { if (!(expr)) return -__LINE__; } while (0)

static int test_font_selection(void)
{
	static const struct {
		const char *scheme;
		enum anx_font_family family;
	} defaults[] = {
		{ "default", ANX_FONT_ATKINSON }, { "aether", ANX_FONT_ATKINSON },
		{ "macos", ANX_FONT_CASCADIA }, { "windows", ANX_FONT_CASCADIA },
		{ "omarchy", ANX_FONT_JETBRAINS }, { "nord", ANX_FONT_ATKINSON },
	};
	struct anx_theme saved;
	char text[2048], setting[96];
	uint32_t i;
	char *font[] = { "theme", "font", "spleen" };
	char *invalid[] = { "theme", "font", "not-a-font" };
	char *invalid_set[] = { "theme", "set", "font_family=not-a-font" };
	char *list[] = { "theme", "fonts" };

	anx_theme_init(ANX_THEME_PRETTY);
	CHECK_FONT(anx_theme_get()->font.family == ANX_FONT_ATKINSON);
	CHECK_FONT(anx_theme_get()->font.antialiased);
	anx_theme_set_mode(ANX_THEME_BORING);
	CHECK_FONT(anx_theme_get()->font.family == ANX_FONT_ATKINSON);
	CHECK_FONT(anx_theme_get()->font.antialiased);

	/* Old documents without a font key inherit their selected scheme. */
	for (i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
		CHECK_FONT(anx_theme_apply_config_checked("font_family=spleen") == ANX_OK);
		anx_snprintf(setting, sizeof(setting), "scheme=%s", defaults[i].scheme);
		CHECK_FONT(anx_theme_apply_config_checked(setting) == ANX_OK);
		CHECK_FONT(anx_theme_get()->font.family == defaults[i].family);
	}

	/* A hand-edited palette preserves each explicit family across text load. */
	for (i = 0; i < ANX_FONT_FAMILY_COUNT; i++) {
		anx_snprintf(setting, sizeof(setting), "mode=pretty;scheme=windows;font_family=%s;accent=123456",
			     anx_font_family_name((enum anx_font_family)i));
		CHECK_FONT(anx_theme_apply_config_checked(setting) == ANX_OK);
		CHECK_FONT(anx_theme_get()->font.family == (enum anx_font_family)i);
		CHECK_FONT(!anx_strcmp(anx_theme_current_scheme(), "custom"));
		saved = *anx_theme_get();
		CHECK_FONT(anx_theme_serialize(text, sizeof(text)) > 0);
		CHECK_FONT(anx_strstr(text, "font_family="));
		anx_theme_set_mode(ANX_THEME_BORING);
		CHECK_FONT(anx_theme_apply_config_checked(text) == ANX_OK);
		CHECK_FONT(!anx_memcmp(&saved, anx_theme_get(), sizeof(saved)));
		anx_theme_set_scheme("omarchy");
		anx_theme_restore(&saved);
		CHECK_FONT(!anx_memcmp(&saved, anx_theme_get(), sizeof(saved)));
	}
	CHECK_FONT(anx_theme_apply_config_checked("scheme=windows;font_family=missing") < 0);
	CHECK_FONT(!anx_memcmp(&saved, anx_theme_get(), sizeof(saved)));
	CHECK_FONT(anx_theme_apply_config_checked("font_family=spleen;opacity=999") < 0);
	CHECK_FONT(!anx_memcmp(&saved, anx_theme_get(), sizeof(saved)));
	CHECK_FONT(anx_theme_apply_config_checked("font_family=") < 0);
	CHECK_FONT(!anx_memcmp(&saved, anx_theme_get(), sizeof(saved)));
	CHECK_FONT(cmd_theme(3, invalid) < 0);
	CHECK_FONT(cmd_theme(3, invalid_set) < 0);
	CHECK_FONT(!anx_memcmp(&saved, anx_theme_get(), sizeof(saved)));
	CHECK_FONT(cmd_theme(2, list) == ANX_OK);
	CHECK_FONT(cmd_theme(3, font) == ANX_OK);
	CHECK_FONT(anx_theme_get()->font.family == ANX_FONT_SPLEEN);
	CHECK_FONT(cmd_theme(2, font) == ANX_OK);

	/* Existing antialiased=false documents remain explicit user choices. */
	CHECK_FONT(anx_theme_apply_config_checked(
		"mode=pretty;scheme=custom;antialiased=false;accent=abcdef") == ANX_OK);
	CHECK_FONT(anx_theme_get()->font.family == ANX_FONT_ATKINSON);
	CHECK_FONT(!anx_theme_get()->font.antialiased);

	/* Persist the override through the same named object as the shell. */
	test_mock_blk_init(16384);
	CHECK_FONT(anx_disk_format("font-config") == ANX_OK);
	CHECK_FONT(anx_theme_apply_config_checked(
		"scheme=omarchy;font_family=cascadia-mono;accent=234567") == ANX_OK);
	saved = *anx_theme_get();
	CHECK_FONT(anx_theme_save() == ANX_OK);
	anx_theme_set_scheme("default");
	anx_uobj_load();
	CHECK_FONT(anx_theme_load() == ANX_OK);
	CHECK_FONT(!anx_memcmp(&saved, anx_theme_get(), sizeof(saved)));
	anx_theme_set_mode(ANX_THEME_BORING);
	return ANX_OK;
}

int test_theme(void)
{
	const struct anx_theme *t;
	struct anx_theme blue, snapshot;
	char text[2048];
	uint32_t i;

	/* Test 1: init pretty */
	anx_theme_init(ANX_THEME_PRETTY);
	if (anx_theme_get_mode() != ANX_THEME_PRETTY) return -1;
	t = anx_theme_get();
	if (!t) return -2;
	if (t->deco.corner_radius == 0) return -3;
	if (!t->deco.shadow_enabled) return -4;
	if (anx_strcmp(anx_theme_current_scheme(), "default")) return -13;
	if (anx_strcmp(anx_theme_scheme_name(0), "default")) return -14;
	blue = *t;
	if (anx_theme_set_scheme("aether") != ANX_OK) return -15;
	if (anx_memcmp(&blue, anx_theme_get(), sizeof(blue))) return -16;
	for (i = 0; anx_theme_scheme_name(i); i++)
		if (!anx_strcmp(anx_theme_scheme_name(i), "aether")) return -17;
	if (anx_theme_apply_config_checked("scheme=aether") != ANX_OK) return -18;
	if (anx_strcmp(anx_theme_current_scheme(), "default")) return -19;
	snapshot = blue;
	anx_strlcpy(snapshot.scheme, "aether", sizeof(snapshot.scheme));
	anx_theme_restore(&snapshot);
	if (anx_strcmp(anx_theme_current_scheme(), "default")) return -20;
	if (anx_theme_serialize(text, sizeof(text)) <= 0) return -21;
	if (!anx_strstr(text, "scheme=default\n")) return -22;

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

	/* A custom palette must retain its selected caption layout after load. */
	if (anx_theme_apply_config_checked("mode=pretty;scheme=windows;accent=123456") != ANX_OK)
		return -23;
	if (anx_theme_get()->deco.controls != ANX_CONTROLS_WINDOWS) return -24;
	if (anx_strcmp(anx_theme_current_scheme(), "custom")) return -25;
	snapshot = *anx_theme_get();
	if (anx_theme_serialize(text, sizeof(text)) <= 0) return -26;
	anx_theme_set_mode(ANX_THEME_BORING);
	if (anx_theme_apply_config_checked(text) != ANX_OK) return -27;
	if (anx_memcmp(&snapshot, anx_theme_get(), sizeof(snapshot))) return -28;
	if (anx_theme_apply_config_checked("controls=invalid") != ANX_EINVAL) return -29;
	if (anx_memcmp(&snapshot, anx_theme_get(), sizeof(snapshot))) return -30;
	if (anx_theme_set_scheme("default") != ANX_OK) return -31;
	if (anx_theme_get()->deco.controls != ANX_CONTROLS_SIGNATURE) return -32;
	anx_theme_set_mode(ANX_THEME_BORING);

	return test_font_selection();
}
