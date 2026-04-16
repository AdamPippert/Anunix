/*
 * splash.c — Boot splash screen.
 *
 * When a framebuffer is available, decodes the embedded JPEG logo
 * and displays it scaled to fill the screen for 5 seconds.
 * Falls back to ANSI art on serial-only consoles.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/fb.h>
#include <anx/jpeg.h>
#include <anx/kprintf.h>

/* Embedded JPEG from splash_img.S */
extern const uint8_t _splash_jpg_start[];
extern const uint8_t _splash_jpg_end[];

/*
 * ANSI color codes for the teal-to-navy gradient.
 * Uses 256-color mode: \033[38;5;Nm
 */

static void puts_color(const char *s, int color)
{
	kprintf("\033[38;5;%dm%s\033[0m", color, s);
}

static void splash_serial(void)
{
	kprintf("\033[2J\033[H\n");

	kprintf("                       ");
	puts_color("___", 37);
	kprintf("\n");

	kprintf("                      ");
	puts_color("/", 37);
	kprintf("   ");
	puts_color("\\", 37);
	kprintf("\n");

	kprintf("                     ");
	puts_color("/", 37);
	kprintf("  ");
	puts_color("o", 6);
	kprintf("  ");
	puts_color("\\", 73);
	kprintf("\n");

	kprintf("                    ");
	puts_color("/", 30);
	kprintf("       ");
	puts_color("\\", 73);
	kprintf("\n");

	kprintf("                   ");
	puts_color("/", 30);
	kprintf("  ");
	puts_color("_____", 37);
	kprintf("  ");
	puts_color("\\", 73);
	kprintf("\n");

	kprintf("                  ");
	puts_color("/", 24);
	kprintf("  ");
	puts_color("/", 30);
	kprintf("     ");
	puts_color("\\", 73);
	kprintf("  ");
	puts_color("\\", 73);
	kprintf("\n");

	kprintf("                 ");
	puts_color("/", 24);
	kprintf("  ");
	puts_color("/", 24);
	kprintf("       ");
	puts_color("\\", 37);
	kprintf("  ");
	puts_color("\\", 37);
	kprintf("\n");

	kprintf("                ");
	puts_color("/", 24);
	puts_color("__", 24);
	puts_color("/", 24);
	kprintf("         ");
	puts_color("\\", 37);
	puts_color("__", 37);
	puts_color("\\", 37);
	kprintf("\n\n");

	kprintf("                ");
	puts_color("A N U N I X", 37);
	kprintf("\n");

	kprintf("          ");
	puts_color("The AI-Native Operating System", 243);
	kprintf("\n\n");
}

static void splash_graphical(void)
{
	const struct anx_fb_info *info;
	struct anx_jpeg_image img;
	uint32_t jpg_size;
	uint64_t start;
	int ret;

	info = anx_fb_get_info();
	if (!info || !info->available)
		return;

	jpg_size = (uint32_t)(_splash_jpg_end - _splash_jpg_start);
	if (jpg_size == 0)
		return;

	ret = anx_jpeg_decode(_splash_jpg_start, jpg_size, &img);
	if (ret != ANX_OK) {
		kprintf("splash: jpeg decode failed (%d)\n", ret);
		return;
	}

	/* Center the logo on the framebuffer (no scaling) */
	{
		uint32_t ox, oy, sx, sy;

		/* Clear to black first */
		anx_fb_clear(0x00000000);

		/* Calculate centered position */
		ox = (info->width > img.width)
		     ? (info->width - img.width) / 2 : 0;
		oy = (info->height > img.height)
		     ? (info->height - img.height) / 2 : 0;

		/* Blit at native resolution */
		for (sy = 0; sy < img.height && oy + sy < info->height; sy++) {
			uint32_t *row = anx_fb_row_ptr(oy + sy);

			if (!row)
				continue;
			for (sx = 0; sx < img.width && ox + sx < info->width; sx++)
				row[ox + sx] = img.pixels[sy * img.width + sx];
		}
	}
	anx_jpeg_free(&img);

	/* Hold the splash for 5 seconds */
	start = arch_timer_ticks();
	while (arch_timer_ticks() - start < 500)
		;

	/* Clear framebuffer for text output */
	anx_fb_clear(0x00000000);
}

void anx_splash(void)
{
	if (anx_fb_available())
		splash_graphical();

	/* Always show ANSI splash on serial for headless monitoring */
	splash_serial();
}
