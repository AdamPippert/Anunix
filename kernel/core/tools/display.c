/*
 * display.c — Display diagnostics: fb_info, gop_list, fb_test.
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/fb.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* fb_info                                                              */
/* ------------------------------------------------------------------ */

void cmd_fb_info(int argc, char **argv)
{
	const struct anx_fb_info *fb;

	(void)argc;
	(void)argv;

	fb = anx_fb_get_info();
	if (!fb || !fb->available) {
		kprintf("{\"available\":false}\n");
		return;
	}
	kprintf("{\"available\":true,\"width\":%u,\"height\":%u,"
		"\"pitch\":%u,\"bpp\":%u}\n",
		fb->width, fb->height, fb->pitch, (uint32_t)fb->bpp);
}

/* ------------------------------------------------------------------ */
/* gop_list                                                             */
/* ------------------------------------------------------------------ */

void cmd_gop_list(int argc, char **argv)
{
	const struct anx_gop_mode *modes;
	uint8_t count, current;
	uint8_t i;

	(void)argc;
	(void)argv;

	modes = anx_fb_get_gop_modes(&count, &current);
	if (count == 0) {
		kprintf("gop_list: no GOP mode data (QEMU or multiboot2 path)\n");
		return;
	}

	kprintf("GOP modes available at boot (%u total, * = selected):\n",
		(uint32_t)count);
	for (i = 0; i < count; i++) {
		kprintf("  %s[%u] %ux%u (GOP mode %u)\n",
			(i == current) ? "*" : " ",
			(uint32_t)i,
			modes[i].width,
			modes[i].height,
			modes[i].mode_number);
	}
}

/* ------------------------------------------------------------------ */
/* fb_test — paint a recognizable pattern to verify fb geometry        */
/* ------------------------------------------------------------------ */

/*
 * Paints 8 equal-height color bars left-to-right:
 *   black, white, red, green, blue, cyan, magenta, yellow
 * This is the classic SMPTE-style test signal and immediately
 * reveals pitch/width/bpp errors (bars will be skewed or wrong color).
 */
static const uint32_t test_colors[8] = {
	0x00000000, /* black   */
	0x00FFFFFF, /* white   */
	0x00FF0000, /* red     */
	0x0000FF00, /* green   */
	0x000000FF, /* blue    */
	0x0000FFFF, /* cyan    */
	0x00FF00FF, /* magenta */
	0x00FFFF00, /* yellow  */
};

void cmd_fb_test(int argc, char **argv)
{
	const struct anx_fb_info *fb;
	uint32_t bar_w, x, y, bar;

	(void)argc;
	(void)argv;

	fb = anx_fb_get_info();
	if (!fb || !fb->available) {
		kprintf("fb_test: framebuffer not available\n");
		return;
	}

	bar_w = fb->width / 8;

	for (bar = 0; bar < 8; bar++) {
		uint32_t x0 = bar * bar_w;
		uint32_t x1 = (bar == 7) ? fb->width : x0 + bar_w;

		for (y = 0; y < fb->height; y++) {
			uint32_t *row = anx_fb_row_ptr(y);

			for (x = x0; x < x1; x++)
				row[x] = test_colors[bar];
		}
	}

	kprintf("fb_test: painted 8-bar pattern at %ux%u\n",
		fb->width, fb->height);
}
