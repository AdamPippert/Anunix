#include <anx/window_chrome.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/fb.h>
#include <anx/string.h>
#include <anx/wm.h>

#define CHECK(expr) do { if (!(expr)) return -__LINE__; } while (0)

static int test_geometry(void)
{
	struct anx_window_chrome chrome;
	uint32_t style, width, height, i, j;

	anx_window_chrome_layout(ANX_CONTROLS_WINDOWS, 640, 28, &chrome);
	CHECK(chrome.draw[0].x == 594 && chrome.draw[0].w == 46);
	CHECK(chrome.draw[1].x == 502 && chrome.draw[2].x == 548);
	CHECK(anx_window_chrome_hit(&chrome, 617, 14) == ANX_WINDOW_BUTTON_CLOSE);
	CHECK(anx_window_chrome_hit(&chrome, 525, 14) == ANX_WINDOW_BUTTON_MINIMIZE);
	CHECK(anx_window_chrome_hit(&chrome, 571, 14) == ANX_WINDOW_BUTTON_MAXIMIZE);
	CHECK(anx_window_chrome_hit(&chrome, 15, 14) == ANX_WINDOW_BUTTON_NONE);
	anx_window_chrome_layout(ANX_CONTROLS_SIGNATURE, 640, 28, &chrome);
	CHECK(chrome.draw[0].x == 8 && chrome.draw[0].y == 7);
	CHECK(chrome.draw[1].x == 27 && chrome.draw[2].x == 46);
	CHECK(anx_window_chrome_hit(&chrome, 15, 14) == ANX_WINDOW_BUTTON_CLOSE);
	CHECK(anx_window_chrome_hit(&chrome, 34, 0) == ANX_WINDOW_BUTTON_MINIMIZE);
	CHECK(anx_window_chrome_hit(&chrome, 53, 27) == ANX_WINDOW_BUTTON_MAXIMIZE);
	CHECK(anx_window_chrome_hit(&chrome, 617, 14) == ANX_WINDOW_BUTTON_NONE);

	/* Tiny captions may omit controls; no painted or clickable region may escape. */
	for (style = 0; style <= ANX_CONTROLS_WINDOWS; style++)
		for (width = 0; width <= 256; width++)
			for (height = 0; height <= 35; height++) {
				anx_window_chrome_layout((enum anx_window_controls)style,
							width, height, &chrome);
				CHECK(chrome.title_x <= width);
				CHECK(chrome.title_width <= width - chrome.title_x);
				CHECK(anx_window_chrome_hit(&chrome, -1, 0) == ANX_WINDOW_BUTTON_NONE);
				CHECK(anx_window_chrome_hit(&chrome, 0, -1) == ANX_WINDOW_BUTTON_NONE);
				CHECK(anx_window_chrome_hit(&chrome, (int32_t)width, 0) == ANX_WINDOW_BUTTON_NONE);
				CHECK(anx_window_chrome_hit(&chrome, 0, (int32_t)height) == ANX_WINDOW_BUTTON_NONE);
				for (i = 0; i < 3; i++) {
					const struct anx_chrome_rect *d = &chrome.draw[i];
					const struct anx_chrome_rect *r = &chrome.hit[i];

					CHECK(d->x <= width && d->w <= width - d->x);
					CHECK(d->y <= height && d->h <= height - d->y);
					CHECK(r->x <= width && r->w <= width - r->x);
					CHECK(r->y <= height && r->h <= height - r->y);
					if (!d->w || !d->h) {
						CHECK(!r->w || !r->h);
						continue;
					}
					CHECK(anx_window_chrome_hit(&chrome, (int32_t)(d->x + d->w / 2),
						(int32_t)(d->y + d->h / 2)) == (enum anx_window_button)(i + 1));
					CHECK(!chrome.title_width || chrome.title_x + chrome.title_width <= r->x ||
						chrome.title_x >= r->x + r->w);
					for (j = i + 1; j < 3; j++) {
						const struct anx_chrome_rect *other = &chrome.hit[j];

						CHECK(!other->w || r->x + r->w <= other->x || other->x + other->w <= r->x);
					}
				}
			}
	return 0;
}

static uint32_t pixels[400 * 120];

static int test_rendering(void)
{
	struct anx_fb_info saved_fb = *anx_fb_get_info();
	struct anx_theme saved_theme = *anx_theme_get();
	struct anx_fb_info fb = { .addr = (uintptr_t)pixels, .width = 400,
		.height = 120, .pitch = 400 * 4, .bpp = 32, .available = true };
	const struct anx_renderer_ops *ops;
	struct anx_surface surf;
	struct anx_window_chrome chrome;
	static const uint32_t widths[] = { 1, 8, 11, 12, 23, 24, 35, 36, 137, 138, 139, 300 };
	uint32_t i, x, y, style;
	int rc = 0;

#undef CHECK
#define CHECK(expr) do { if (!(expr)) { rc = -__LINE__; goto out; } } while (0)
	CHECK(anx_fb_init(&fb) == ANX_OK);
	CHECK(anx_iface_init() == ANX_OK);
	CHECK(anx_renderer_gpu_register() == ANX_OK);
	ops = anx_iface_renderer_ops(ANX_ENGINE_RENDERER_GPU);
	CHECK(ops != NULL);
	anx_memset(&surf, 0, sizeof(surf));
	surf.x = 20; surf.y = 68; surf.width = 300; surf.height = 30;
	surf.oid = anx_input_focus_get();
	anx_strlcpy(surf.title, "Caption with controls", sizeof(surf.title));
	CHECK(anx_theme_apply_config_checked("mode=pretty;scheme=windows;corners=square;shadow=false") == ANX_OK);
	anx_fb_clear(0xFEDCBA);
	CHECK(ops->commit(&surf) == ANX_OK);
	anx_window_chrome_layout(ANX_CONTROLS_WINDOWS, surf.width, 28, &chrome);
	for (i = 0; i < 3; i++) {
		const struct anx_chrome_rect *r = &chrome.draw[i];
		uint32_t gx = 20 + r->x + (r->w - 10) / 2;
		uint32_t gy = 40 + (r->h - 10) / 2;

		if (i == 1) gy += 5;
		CHECK(pixels[gy * 400 + gx] == anx_theme_get()->palette.btn_glyph);
		CHECK(anx_window_chrome_hit(&chrome, (int32_t)(gx - 20), (int32_t)(gy - 40)) ==
		      (enum anx_window_button)(i + 1));
	}
	CHECK(pixels[54 * 400 + 35] == anx_theme_get()->palette.title_from);
	/* Caption fills and glyphs stay within actual titlebar bounds at every width. */
	for (style = 0; style <= ANX_CONTROLS_WINDOWS; style++) {
		CHECK(anx_theme_apply_config_checked(style ? "controls=windows" : "controls=signature") == ANX_OK);
		for (i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
			surf.width = widths[i];
			anx_fb_clear(0xFEDCBA);
			CHECK(ops->commit(&surf) == ANX_OK);
			for (y = 0; y < 120; y++)
				for (x = 0; x < 400; x++)
					if (x < 20 || x >= 20 + surf.width || y < 40 || y >= 68)
						CHECK(pixels[y * 400 + x] == 0xFEDCBA);
		}
	}
out:
	anx_theme_restore(&saved_theme);
	anx_fb_init(&saved_fb);
	return rc;
}

int test_window_chrome(void)
{
	int rc = test_geometry();

	return rc ? rc : test_rendering();
}
