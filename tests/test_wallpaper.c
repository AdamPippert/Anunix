#include <anx/wallpaper.h>
#include <anx/wm.h>
#include <anx/theme.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/string.h>

static uint32_t pixels[100 * 40];

int test_wallpaper(void)
{
	struct anx_fb_info old_fb = *anx_fb_get_info();
	struct anx_theme old_theme = *anx_theme_get();
	struct anx_fb_info fb = { .addr = (uintptr_t)pixels, .width = 4, .height = 4,
		.pitch = 4 * 4, .bpp = 32, .available = true };
	struct {
		struct anx_wallpaper_header header;
		uint32_t data[8];
	} image = { { ANX_WALLPAPER_MAGIC, 4, 2, 0 },
		{ 0x110000, 0x220000, 0x330000, 0x440000, 0x001100, 0x002200, 0x003300, 0x004400 } };
	struct anx_so_create_params cp;
	struct anx_state_object *obj = NULL, *bad_obj = NULL;
	struct anx_object_handle handle;
	struct anx_wallpaper_header invalid = { ANX_WALLPAPER_MAGIC, 0, 1, 0 };
	uint32_t full[16], default_pixels[16], x, y;
	int rc = 0;
	bool bound = false, bad_bound = false, opened = false;

#define CHECK(expr) do { if (!(expr)) { rc = -__LINE__; goto out; } } while (0)
	CHECK(anx_fb_init(&fb) == ANX_OK);
	CHECK(anx_theme_init(ANX_THEME_PRETTY) == ANX_OK);
	CHECK(anx_theme_get()->deco.wallpaper == ANX_WALLPAPER_IMAGE);
	CHECK(anx_wm_wallpaper_set(NULL) == ANX_OK);
	CHECK(anx_wm_wallpaper_ready());
	anx_wm_desktop_paint(0, 0, 4, 4);
	anx_memcpy(default_pixels, pixels, sizeof(default_pixels));

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type = ANX_OBJ_BYTE_DATA;
	cp.payload = &image;
	cp.payload_size = sizeof(image);
	CHECK(anx_so_create(&cp, &obj) == ANX_OK);
	CHECK(anx_ns_bind("default", "wallpaper-test", &obj->oid) == ANX_OK);
	bound = true;
	CHECK(anx_wm_wallpaper_set("default:wallpaper-test") == ANX_OK);
	anx_wm_desktop_paint(0, 0, 4, 4);
	/* A wide image on a square screen crops equally at both sides. */
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++)
			CHECK(pixels[y * 4 + x] == image.data[(y / 2) * 4 + 1 + x / 2]);
	anx_memcpy(full, pixels, sizeof(full));
	anx_fb_clear(0xABCDEF);
	anx_wm_desktop_paint(1, 1, 2, 2);
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++)
			CHECK(pixels[y * 4 + x] == (x >= 1 && x < 3 && y >= 1 && y < 3
				? full[y * 4 + x] : 0xABCDEF));
	anx_fb_clear(0xABCDEF);
	anx_wm_desktop_paint(3, 3, 0xFFFFFFFF, 0xFFFFFFFF);
	CHECK(pixels[15] == full[15]);
	CHECK(pixels[14] == 0xABCDEF && pixels[11] == 0xABCDEF);
	anx_wm_desktop_paint(0xFFFFFFFF, 0, 10, 10);
	CHECK(pixels[0] == 0xABCDEF);

	/* Invalid selection keeps the previous image. */
	cp.payload = &invalid;
	cp.payload_size = sizeof(invalid);
	CHECK(anx_so_create(&cp, &bad_obj) == ANX_OK);
	CHECK(anx_ns_bind("default", "wallpaper-bad", &bad_obj->oid) == ANX_OK);
	bad_bound = true;
	CHECK(anx_wm_wallpaper_set("default:wallpaper-bad") == ANX_EINVAL);
	anx_wm_desktop_paint(0, 0, 4, 4);
	CHECK(anx_memcmp(pixels, full, sizeof(full)) == 0);
	CHECK(anx_so_open(&bad_obj->oid, ANX_OPEN_WRITE, &handle) == ANX_OK);
	opened = true;
	invalid.width = 1; /* Valid dimensions, but no pixel payload. */
	CHECK(anx_so_write_payload(&handle, 0, &invalid, sizeof(invalid)) == (int)sizeof(invalid));
	anx_so_close(&handle); opened = false;
	CHECK(anx_wm_wallpaper_set("default:wallpaper-bad") == ANX_EINVAL);
	CHECK(anx_so_open(&bad_obj->oid, ANX_OPEN_WRITE, &handle) == ANX_OK);
	opened = true;
	CHECK(anx_so_replace_payload(&handle, &invalid, 3) == ANX_OK);
	anx_so_close(&handle); opened = false;
	CHECK(anx_wm_wallpaper_set("default:wallpaper-bad") == ANX_EINVAL);

	/* Mutating a selected object's header cannot create a divide-by-zero or OOB read. */
	CHECK(anx_so_open(&obj->oid, ANX_OPEN_WRITE, &handle) == ANX_OK);
	opened = true;
	invalid.width = 0xFFFFFFFF;
	CHECK(anx_so_write_payload(&handle, 0, &invalid, sizeof(invalid)) == (int)sizeof(invalid));
	anx_so_close(&handle); opened = false;
	anx_wm_desktop_paint(0, 0, 4, 4);
	CHECK(anx_memcmp(pixels, default_pixels, sizeof(default_pixels)) == 0);

	/* A tall image crops equally above and below the center instead. */
	image.header.width = 2; image.header.height = 4;
	CHECK(anx_so_open(&obj->oid, ANX_OPEN_WRITE, &handle) == ANX_OK);
	opened = true;
	CHECK(anx_so_write_payload(&handle, 0, &image, sizeof(image)) == (int)sizeof(image));
	anx_so_close(&handle); opened = false;
	anx_wm_desktop_paint(0, 0, 4, 4);
	for (y = 0; y < 4; y++)
		for (x = 0; x < 4; x++)
			CHECK(pixels[y * 4 + x] == image.data[(1 + y / 2) * 2 + x / 2]);

	/* A one-pixel object covers a much wider repaint without repeated-row overruns. */
	image.header.width = 1; image.header.height = 1;
	CHECK(anx_so_open(&obj->oid, ANX_OPEN_WRITE, &handle) == ANX_OK);
	opened = true;
	CHECK(anx_so_write_payload(&handle, 0, &image, sizeof(image)) == (int)sizeof(image));
	anx_so_close(&handle); opened = false;
	fb.width = 100; fb.height = 40; fb.pitch = 100 * 4;
	CHECK(anx_fb_init(&fb) == ANX_OK);
	anx_wm_desktop_paint(0, 0, 100, 40);
	for (x = 0; x < 100 * 40; x++) CHECK(pixels[x] == image.data[0]);

	/* Explicit modes override both the built-in photo and a selected object. */
	CHECK(anx_theme_apply_config_checked("wallpaper=solid;background=123456") == ANX_OK);
	anx_wm_desktop_paint(0, 0, 100, 40);
	CHECK(pixels[0] == 0x123456 && pixels[3999] == 0x123456);
	CHECK(anx_theme_apply_config_checked("wallpaper=gradient;wallpaper_from=000000;wallpaper_to=ffffff") == ANX_OK);
	anx_wm_desktop_paint(0, 0, 100, 40);
	CHECK(pixels[0] == 0 && pixels[3999] == 0xFFFFFF);
out:
	if (opened) anx_so_close(&handle);
	anx_wm_wallpaper_set(NULL);
	if (bound) anx_ns_unbind("default", "wallpaper-test");
	if (bad_bound) anx_ns_unbind("default", "wallpaper-bad");
	if (obj) anx_objstore_release(obj);
	if (bad_obj) anx_objstore_release(bad_obj);
	anx_theme_restore(&old_theme);
	anx_fb_init(&old_fb);
	return rc;
}
