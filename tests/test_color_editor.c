/* Exercise editor input through the real surface and theme APIs. */
#include <anx/color_editor.h>
#include <anx/fb.h>
#include <anx/input.h>
#include <anx/interface_plane.h>
#include <anx/mock_blk.h>
#include <anx/objstore_disk.h>
#include <anx/namespace.h>
#include <anx/font.h>
#include <anx/theme.h>
#include <anx/tools.h>
#include <anx/wm.h>

#define CHECK(test, code) do { if (!(test)) { rc = (code); goto out; } } while (0)

static uint32_t color_test_fb[640 * 480];

static void type_hex(const char *text)
{
	while (*text)
		anx_wm_color_editor_key(ANX_KEY_NONE, 0, (uint8_t)*text++);
}

int test_color_editor(void)
{
	struct anx_fb_info old_fb = *anx_fb_get_info();
	struct anx_fb_info fb = {
		.addr = (uint64_t)(uintptr_t)color_test_fb,
		.width = 640, .height = 480, .pitch = 640 * 4,
		.bpp = 32, .available = true,
	};
	struct anx_wm_tiling_config old_tiling = anx_wm_tiling;
	struct anx_surface *surf;
	anx_oid_t saved_oid, current_oid;
	uint32_t *slot, original, count = 0;
	int rc = 0;

	CHECK(anx_fb_init(&fb) == ANX_OK, -1);
	CHECK(anx_iface_init() == ANX_OK, -2);
	CHECK(anx_renderer_gpu_register() == ANX_OK, -3);
	test_mock_blk_init(16384);
	CHECK(anx_disk_format("color-editor") == ANX_OK, -19);
	anx_theme_init(ANX_THEME_PRETTY);
	anx_wm_tiling.enabled = false;
	anx_wm_launch_color_editor();
	surf = anx_wm_color_editor_surface();
	CHECK(surf && surf->content_root && surf->content_root->data, -4);
	anx_wm_launch_color_editor();
	CHECK(anx_wm_color_editor_surface() == surf, -5);
	slot = anx_theme_color_slot(anx_theme_color_name(0));
	CHECK(slot != NULL, -6);
	original = *slot;

	/* Incomplete input must not change the running palette. */
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	type_hex("12345");
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	CHECK(*slot == original, -7);
	/* Invalid character is rejected; the same edit remains incomplete. */
	type_hex("G");
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	CHECK(*slot == original, -8);
	type_hex("a");
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	CHECK(*slot == 0x12345a, -9);

	/* Escape cancels the draft, preserving an earlier live application. */
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	type_hex("FFFFFF");
	anx_wm_color_editor_key(ANX_KEY_ESC, 0, 0);
	CHECK(*slot == 0x12345a, -10);
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	type_hex("000000");
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	CHECK(*slot == 0, -11);
	/* Save is explicit: S records the live palette, then load restores it. */
	anx_wm_color_editor_key(ANX_KEY_S, 0, 's');
	*slot = 0x112233;
	CHECK(anx_theme_load() == ANX_OK, -20);
	CHECK(*slot == 0, -21);
	CHECK(((uint32_t *)surf->content_root->data)[(ANX_FONT_HEIGHT + 8) * surf->width] == 0x205c3bu, -28);
	CHECK(anx_ns_resolve("system", "config/theme", &saved_oid) == ANX_OK, -29);

	/* Arrow keys edit one selected nibble, previewing without a disk write. */
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	anx_wm_color_editor_key(ANX_KEY_UP, 0, 0);
	CHECK(*slot == 0x100000, -30);
	anx_wm_color_editor_key(ANX_KEY_RIGHT, 0, 0);
	anx_wm_color_editor_key(ANX_KEY_DOWN, 0, 0);
	CHECK(*slot == 0x1f0000, -31);
	anx_wm_color_editor_key(ANX_KEY_UP, 0, 0);
	CHECK(*slot == 0x100000, -32); /* f wraps to 0 without carrying. */
	anx_wm_color_editor_key(ANX_KEY_LEFT, 0, 0);
	anx_wm_color_editor_key(ANX_KEY_DOWN, 0, 0);
	CHECK(*slot == 0, -33);
	anx_wm_color_editor_key(ANX_KEY_DOWN, 0, 0);
	CHECK(*slot == 0xf00000, -34);
	CHECK(anx_ns_resolve("system", "config/theme", &current_oid) == ANX_OK, -35);
	CHECK(saved_oid.hi == current_oid.hi && saved_oid.lo == current_oid.lo, -36);
	/* The bottom swatches show the initial color beside the live preview. */
	{
		uint32_t *pixels = surf->content_root->data;
		uint32_t y = surf->height - 3 * (ANX_FONT_HEIGHT + 8) + 5;
		CHECK(pixels[y * surf->width + 10] == 0, -37);
		CHECK(pixels[y * surf->width + surf->width / 2 + 2] == 0xf00000, -38);
	}
	anx_wm_color_editor_key(ANX_KEY_ESC, 0, 0);
	CHECK(*slot == 0, -39);
	/* Enter accepts a preview; a failed save leaves it live and reports red. */
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	anx_wm_color_editor_key(ANX_KEY_UP, 0, 0);
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	CHECK(*slot == 0x100000, -40);
	{
		struct anx_blk_dev *disk = anx_blk_active();
		anx_blk_set_active(NULL);
		anx_wm_color_editor_key(ANX_KEY_S, 0, 's');
		anx_blk_set_active(disk);
		CHECK(*slot == 0x100000, -41);
		CHECK(((uint32_t *)surf->content_root->data)[(ANX_FONT_HEIGHT + 8) * surf->width] == 0x852e35u, -42);
		CHECK(anx_theme_load() == ANX_OK && *slot == 0, -43);
	}
	/* S also explicitly accepts and saves a complete preview. */
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	anx_wm_color_editor_key(ANX_KEY_UP, 0, 0);
	anx_wm_color_editor_key(ANX_KEY_S, 0, 's');
	*slot = 0;
	CHECK(anx_theme_load() == ANX_OK && *slot == 0x100000, -44);

	/* End must reach colors beyond the visible list; resize keeps selection. */
	while (anx_theme_color_name(count)) count++;
	anx_wm_color_editor_key(ANX_KEY_END, 0, 0);
	surf->width = 400;
	surf->height = 240;
	surf->on_resize(surf);
	CHECK(surf->buf_w == 400 && surf->buf_h == 240, -12);
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	type_hex("ABCDEF");
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	CHECK(*anx_theme_color_slot(anx_theme_color_name(count - 1)) == 0xabcdef, -13);
	/* Tiny geometry must not underflow the list or write beyond the canvas. */
	surf->width = 1;
	surf->height = 1;
	surf->on_resize(surf);
	CHECK(surf->buf_w == 1 && surf->buf_h == 1, -14);
	anx_wm_color_editor_redraw();
	anx_wm_color_editor_key(ANX_KEY_PAGEUP, 0, 0);

	/* Meta+Q destroys the editor and discards an unaccepted live preview. */
	anx_wm_color_editor_key(ANX_KEY_HOME, 0, 0);
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	anx_wm_color_editor_key(ANX_KEY_UP, 0, 0);
	CHECK(*slot == 0x200000, -45);
	current_oid = surf->oid;
	anx_wm_hotkeys_init();
	CHECK(anx_wm_hotkey_dispatch(ANX_MOD_META, ANX_KEY_Q), -15);
	CHECK(anx_wm_color_editor_surface() == NULL, -16);
	CHECK(*slot == 0x100000, -46);
	CHECK(anx_iface_surface_lookup(current_oid, &surf) == ANX_ENOENT, -47);
	anx_wm_color_editor_key(ANX_KEY_ENTER, 0, 0);
	anx_wm_launch_color_editor();
	CHECK(anx_wm_color_editor_surface() != NULL, -17);
	anx_wm_color_editor_key(ANX_KEY_ESC, 0, 0);
	CHECK(anx_wm_color_editor_surface() == NULL, -18);
	/* The reachable View menu launches Colors and clears itself on close. */
	{
		struct anx_key_event key = {0};
		anx_oid_t nil = {0, 0};
		anx_wm_app_menu_open(2, nil);
		CHECK(anx_wm_app_menu_active(), -22);
		surf = anx_wm_focused_window();
		CHECK(surf != NULL, -23);
		CHECK(anx_wm_window_close(surf) == ANX_OK, -24);
		CHECK(!anx_wm_app_menu_active(), -25);
		anx_wm_app_menu_open(2, nil);
		key.keycode = ANX_KEY_DOWN;
		anx_wm_app_menu_key_event(&key);
		anx_wm_app_menu_key_event(&key);
		key.keycode = ANX_KEY_ENTER;
		anx_wm_app_menu_key_event(&key);
		CHECK(!anx_wm_app_menu_active(), -26);
		CHECK(anx_wm_color_editor_surface() != NULL, -27);
	}
out:
	if (anx_wm_color_editor_surface())
		anx_wm_window_close(anx_wm_color_editor_surface());
	anx_wm_tiling = old_tiling;
	anx_fb_init(&old_fb);
	return rc;
}
