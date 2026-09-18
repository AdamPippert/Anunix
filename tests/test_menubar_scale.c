/* Real scaled menubar rendering and the click map used by the WM. */
#include <anx/wm.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/input.h>
#include <anx/interface_plane.h>
#include <anx/theme.h>
#include <anx/string.h>
#include <anx/kprintf.h>

extern struct anx_surface *g_menubar;
extern uint32_t *g_menubar_pixels;

#define CHECK(c) do { if (!(c)) { rc = -__LINE__; goto out; } } while (0)
#define INK 0xffffffu
static uint32_t test_fb[1280 * 240];
static uint32_t unscaled[ANX_FONT_WIDTH * ANX_FONT_HEIGHT];
static uint32_t clock_before[234 * ANX_WM_MENUBAR_H];

static uint32_t ink_height(const uint32_t *pixels, uint32_t stride,
			   uint32_t x, uint32_t width, uint32_t height)
{
	uint32_t row, col, first = height, last = 0;
	for (row = 0; row < height; row++)
		for (col = x; col < x + width; col++)
			if (pixels[row * stride + col] == INK) {
				if (row < first) first = row;
				last = row;
			}
	return first < height ? last - first + 1 : 0;
}

static void destroy_bar(void)
{
	struct anx_surface *bar = g_menubar;
	g_menubar = NULL;
	g_menubar_pixels = NULL;
	if (bar) {
		anx_wm_canvas_free(bar);
		anx_iface_surface_destroy(bar);
	}
}

int test_menubar_scale(void)
{
	static const uint32_t widths[] = {1, 16, 32, 48, 83, 84, 159, 160, 192, 240, 320, 640, 800, 1280};
	struct anx_fb_info old_fb = *anx_fb_get_info(), fb = old_fb;
	struct anx_theme old_theme = *anx_theme_get();
	struct anx_surface *old_bar = g_menubar, *title = NULL;
	uint32_t *old_pixels = g_menubar_pixels;
	anx_oid_t old_focus = anx_input_focus_get(), nil = {0, 0};
	uint32_t i, x, y, base_height;
	int rc = 0;

	g_menubar = NULL;
	g_menubar_pixels = NULL;
	fb.addr = (uint64_t)(uintptr_t)test_fb;
	fb.height = 240; fb.bpp = 32; fb.available = true;
	CHECK(anx_renderer_gpu_register() == ANX_OK);
	CHECK(anx_theme_apply_config_checked(
		"mode=pretty;font_family=atkinson-hyperlegible-mono;antialiased=false;"
		"background=101010;surface=101010;bar_from=101010;bar_to=101010;"
		"border=202020;text_primary=ffffff;text_dim=808080;accent=00ee00") == ANX_OK);
	anx_memset(unscaled, 0, sizeof(unscaled));
	anx_font_blit_str(unscaled, ANX_FONT_WIDTH, ANX_FONT_HEIGHT,
			  0, 0, "0", INK, 0);
	base_height = ink_height(unscaled, ANX_FONT_WIDTH, 0, ANX_FONT_WIDTH, ANX_FONT_HEIGHT);
	CHECK(base_height > 0);
	anx_input_focus_set(nil);
	for (i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
		uint32_t width = widths[i], found = 0;
		bool active_visible = false;
		fb.width = width; fb.pitch = width * 4;
		CHECK(anx_fb_init(&fb) == ANX_OK);
		CHECK(anx_wm_menubar_create() == ANX_OK);
		CHECK(g_menubar->height == 51 && g_menubar->buf_h == 51);
		CHECK(anx_wm_menubar_hit(-1, 25) == 0);
		CHECK(anx_wm_menubar_hit((int32_t)width, 25) == 0);
		CHECK(anx_wm_menubar_hit(21, -1) == 0);
		CHECK(anx_wm_menubar_hit(21, 51) == 0);
		if (width >= 48) {
			CHECK(anx_wm_menubar_hit(30, 25) == -1);
			CHECK(g_menubar_pixels[19 * width + 30] == 0x00ee00);
		}
		if (width >= 84) {
			CHECK(anx_wm_menubar_hit((int32_t)width - 24, 25) == -2);
			CHECK(g_menubar_pixels[15 * width + width - 24] == 0x808080);
		}
		for (x = 51; x + 10 < width; x += 27) {
			int ws = anx_wm_menubar_hit((int32_t)x, 25);
			if (ws <= 0) continue;
			CHECK(ws <= ANX_WM_WORKSPACES);
			found++;
			if ((uint32_t)ws == anx_wm_workspace_active()) active_visible = true;
			CHECK(anx_wm_menubar_hit((int32_t)x + 10, 35) == ws);
			CHECK(g_menubar_pixels[25 * width + x + 6] != 0x101010);
		}
		if (found) CHECK(active_visible);
		if (width == 640) {
			/* A short centred clock remains distinct from workspace dots. */
			CHECK(found < 9 && found > 0);
			CHECK(ink_height(g_menubar_pixels, width, 275, 90, 51) >= base_height * 3 / 2 - 1);
			CHECK(anx_wm_menubar_hit(275, 25) == 0);
		}
		if (width == 1280) {
			CHECK(found == 9);
			for (x = 0; x < 9; x++) CHECK(anx_wm_menubar_hit(51 + (int32_t)x * 27, 25) == (int)x + 1);
			/* A long focused title cannot overwrite even one clock pixel. */
			for (y = 0; y < 51; y++)
				anx_memcpy(clock_before + y * 234, g_menubar_pixels + y * width + 523, 234 * 4);
			CHECK(anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, NULL, 0, 80, 100, 80, &title) == ANX_OK);
			anx_iface_surface_set_title(title, "user@hostname: a deliberately long focused window title extending beyond the clock");
			anx_input_focus_set(title->oid);
			anx_wm_menubar_refresh();
			for (y = 0; y < 51; y++)
				CHECK(!anx_memcmp(clock_before + y * 234, g_menubar_pixels + y * width + 523, 234 * 4));
			/* Text blends onto the bar gradient, independent of window surface colour. */
			CHECK(anx_theme_apply_config_checked("surface=ffffff") == ANX_OK);
			anx_wm_menubar_refresh();
			for (y = 0; y < 51; y++)
				CHECK(!anx_memcmp(clock_before + y * 234, g_menubar_pixels + y * width + 523, 234 * 4));
			anx_iface_surface_destroy(title);
			title = NULL;
			anx_input_focus_set(nil);
		}
		destroy_bar();
	}
out:
	if (title) anx_iface_surface_destroy(title);
	destroy_bar();
	g_menubar = old_bar;
	g_menubar_pixels = old_pixels;
	anx_input_focus_set(old_focus);
	anx_theme_restore(&old_theme);
	anx_fb_init(&old_fb);
	if (rc) kprintf("menubar_scale failed at %d\n", -rc);
	return rc;
}
