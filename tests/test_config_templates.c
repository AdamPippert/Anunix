#include <anx/config_templates.h>
#include <anx/theme.h>
#include <anx/wm.h>
#include <anx/string.h>

#define CHECK(expr) do { if (!(expr)) return -__LINE__; } while (0)

static int test_theme_text(void)
{
	static const char *bad[] = {
		"accent=12345", "accent=1234567", "accent=12345z",
		"font_family=missing", "font_family=Cascadia", "font_family=",
		"shadow=maybe", "shadow=1", "font_scale=0", "font_scale=5",
		"opacity=256", "bar_opacity=-1", "corner_radius=42949672960",
		"shadow_blur=99999", "show_titlebar=yes", "antialiased=off",
		"titlebar_height=15", "scheme=missing", "mode=oops",
		"corners=triangular", "wallpaper=missing", "unknown=1",
		"accent", "=123456", "accent=", "", "\n;\n",
	};
	struct anx_theme before;
	char text[2048];
	uint32_t i, color = 0xABCDEF;
	int len;

	CHECK(anx_theme_apply_config_checked("mode=pretty;scheme=macos") == ANX_OK);
	before = *anx_theme_get();
	len = anx_theme_serialize(text, sizeof(text));
	CHECK(len > 0 && (uint32_t)len == anx_strlen(text));
	CHECK(anx_theme_serialize(text, (uint32_t)len) == ANX_EFULL);
	CHECK(anx_theme_serialize(text, (uint32_t)len + 1) == len);
	anx_theme_set_mode(ANX_THEME_BORING);
	CHECK(anx_theme_apply_config_checked(text) == ANX_OK);
	CHECK(anx_memcmp(&before, anx_theme_get(), sizeof(before)) == 0);

	CHECK(anx_theme_apply_config_checked(
		"accent=#123ABC\nshadow_offset_x=17\nshadow_offset_y=23\n"
		"animation_ms=321;show_titlebar=false;antialiased=true;"
		"corners=signature;corner_radius=0;opacity=210;bar_opacity=212;"
		"transparency=true;wallpaper=solid;font_scale=3;font_family=spleen") == ANX_OK);
	CHECK(anx_strcmp(anx_theme_current_scheme(), "custom") == 0);
	before = *anx_theme_get();
	CHECK(anx_theme_serialize(text, sizeof(text)) > 0);
	anx_theme_set_mode(ANX_THEME_BORING);
	CHECK(anx_theme_apply_config_checked(text) == ANX_OK);
	CHECK(anx_memcmp(&before, anx_theme_get(), sizeof(before)) == 0);

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(anx_theme_apply_config_checked(bad[i]) < 0);
		CHECK(anx_memcmp(&before, anx_theme_get(), sizeof(before)) == 0);
	}
	CHECK(anx_theme_apply_config_checked("scheme=windows;opacity=oops") < 0);
	CHECK(anx_memcmp(&before, anx_theme_get(), sizeof(before)) == 0);
	CHECK(anx_theme_apply_config_checked("mode=boring;bogus=true") < 0);
	CHECK(anx_memcmp(&before, anx_theme_get(), sizeof(before)) == 0);
	CHECK(anx_theme_apply_config_checked(NULL) == ANX_EINVAL);
	anx_memset(text, 'x', 100);
	text[100] = '\0';
	CHECK(anx_theme_apply_config_checked(text) == ANX_EINVAL);
	CHECK(anx_memcmp(&before, anx_theme_get(), sizeof(before)) == 0);
	CHECK(anx_theme_serialize(text, 2) == ANX_EFULL);
	CHECK(anx_theme_serialize(text, 0) == ANX_EINVAL);
	CHECK(anx_theme_serialize(NULL, 10) == ANX_EINVAL);
	CHECK(anx_theme_parse_color_checked("#00ff12", &color) == ANX_OK);
	CHECK(color == 0x00FF12);
	CHECK(anx_theme_parse_color_checked("0xABCDEF", &color) == ANX_OK);
	CHECK(color == 0xABCDEF);
	CHECK(anx_theme_parse_color_checked("0x12345z", &color) == ANX_EINVAL);
	CHECK(color == 0xABCDEF);
	return 0;
}

int test_config_templates(void)
{
	struct anx_theme original = *anx_theme_get(), before;
	struct anx_wm_tiling_config old_tiling = anx_wm_tiling, previous;
	const struct anx_theme *t;
	struct anx_wm_tile_tree tree;
	struct anx_wm_rect area = { .x = 0, .y = 0, .w = 1000, .h = 700 };
	struct anx_wm_rect boxes[2];
	anx_oid_t one = { .hi = 1, .lo = 1 }, two = { .hi = 1, .lo = 2 }, oids[2];
	char text[2048];
	uint32_t i;
	int rc;

	CHECK(anx_config_template_name(3) == NULL);
	CHECK(anx_config_template_name(0xFFFFFFFF) == NULL);
	CHECK(anx_config_template_describe("missing") == NULL);
	CHECK(anx_config_template_describe(NULL) == NULL);
	for (i = 0; i < 3; i++) {
		const char *name = anx_config_template_name(i);

		CHECK(name && anx_config_template_describe(name));
		CHECK(anx_config_template_apply(name) == ANX_OK);
		CHECK(anx_strcmp(anx_theme_current_scheme(), name) == 0);
		before = *anx_theme_get();
		previous = anx_wm_tiling;
		CHECK(anx_config_template_apply(name) == ANX_OK);
		CHECK(anx_memcmp(&before, anx_theme_get(), sizeof(before)) == 0);
		CHECK(anx_memcmp(&previous, &anx_wm_tiling, sizeof(previous)) == 0);
		CHECK(anx_theme_serialize(text, sizeof(text)) > 0);
		anx_theme_set_mode(ANX_THEME_BORING);
		CHECK(anx_theme_apply_config_checked(text) == ANX_OK);
		CHECK(anx_memcmp(&before, anx_theme_get(), sizeof(before)) == 0);
	}
	CHECK(anx_config_template_apply("macos") == ANX_OK);
	t = anx_theme_get();
	CHECK(!anx_wm_tiling.enabled && anx_wm_tiling.gaps_out == 12);
	CHECK(t->deco.corner_radius == 16 && !t->deco.signature_corners);
	CHECK(t->deco.bar_opacity == 225 && t->deco.window_opacity == 255);
	CHECK(t->palette.btn_close == 0xFF5F57 && t->palette.btn_max == 0x28C840);
	CHECK(t->palette.surface == 0xECECEC);
	CHECK(t->font.family == ANX_FONT_CASCADIA && t->font.antialiased);
	CHECK(anx_config_template_apply("windows") == ANX_OK);
	t = anx_theme_get();
	CHECK(!anx_wm_tiling.enabled && anx_wm_tiling.gaps_in == 0);
	CHECK(anx_wm_tiling.gaps_out == 0 && t->deco.corner_radius == 0);
	CHECK(t->deco.bar_opacity == 255 && t->palette.accent == 0x0078D4);
	CHECK(t->deco.controls == ANX_CONTROLS_WINDOWS);
	CHECK(t->palette.surface == 0xFFFFFF && t->palette.text_primary == 0x191919);
	CHECK(t->palette.title_from == t->palette.title_to);
	CHECK(t->font.family == ANX_FONT_CASCADIA && t->font.antialiased);
	CHECK(anx_config_template_apply("omarchy") == ANX_OK);
	t = anx_theme_get();
	CHECK(anx_wm_tiling.enabled && anx_wm_tiling.border_w == 1);
	CHECK(anx_wm_tiling.gaps_in == 5 && anx_wm_tiling.gaps_out == 10);
	CHECK(t->deco.corner_radius == 6 && t->palette.accent == 0xCBA6F7);
	CHECK(t->deco.controls == ANX_CONTROLS_SIGNATURE);
	CHECK(t->deco.wallpaper == ANX_WALLPAPER_IMAGE);
	CHECK(t->palette.wallpaper_from != t->palette.wallpaper_to);
	CHECK(t->deco.bar_opacity == 245);
	CHECK(t->font.family == ANX_FONT_JETBRAINS && t->font.antialiased);
	anx_wm_tile_init(&tree);
	CHECK(anx_wm_tile_insert(&tree, &one, NULL) == ANX_OK);
	CHECK(anx_wm_tile_insert(&tree, &two, &one) == ANX_OK);
	CHECK(anx_wm_tile_layout(&tree, &area, &anx_wm_tiling, oids, boxes, 2) == 2);
	CHECK(boxes[0].x == 10 && boxes[0].y == 10);
	CHECK(boxes[1].x - (boxes[0].x + (int32_t)boxes[0].w) == 10);
	before = *t;
	previous = anx_wm_tiling;
	CHECK(anx_config_template_apply("missing") == ANX_ENOENT);
	CHECK(anx_config_template_apply(NULL) == ANX_ENOENT);
	CHECK(anx_memcmp(&before, anx_theme_get(), sizeof(before)) == 0);
	CHECK(anx_memcmp(&previous, &anx_wm_tiling, sizeof(previous)) == 0);
	rc = test_theme_text();
	anx_theme_restore(&original);
	anx_wm_tiling = old_tiling;
	return rc;
}
