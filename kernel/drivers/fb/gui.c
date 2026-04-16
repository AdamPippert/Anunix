/*
 * gui.c — Graphical user environment.
 *
 * Tiled window manager with:
 * - Light sky blue (#87CEEB) background
 * - Top bar with centered time in white (2x scale = 16x32 font)
 * - Midnight blue (#191970) terminal window with 30px margin
 * - White text in the terminal
 * - Functional anx> shell
 */

#include <anx/types.h>
#include <anx/gui.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/io.h>
#include <anx/arch.h>
#include <anx/string.h>

static bool gui_ready;
static uint32_t screen_w, screen_h;

/* UTC offset in hours (set via boot cmdline: tz=-7 for PDT) */
static int32_t utc_offset_hours;

void anx_gui_set_tz_offset(int32_t hours)
{
	utc_offset_hours = hours;
}

/* Terminal state */
static uint32_t term_x, term_y;	/* pixel origin of terminal area */
static uint32_t term_w, term_h;	/* pixel size of terminal area */
static uint32_t term_cols, term_rows;	/* character grid */
static uint32_t cur_col, cur_row;	/* cursor position in chars */

#define TERM_FONT_SCALE	1	/* 1x = 8x16, 2x = 16x32 */
#define TERM_CHAR_W	(ANX_FONT_WIDTH * TERM_FONT_SCALE)
#define TERM_CHAR_H	(ANX_FONT_HEIGHT * TERM_FONT_SCALE)
#define TIME_FONT_SCALE	2	/* 2x for top bar time */

/* --- Scaled font rendering --- */

void anx_gui_draw_char_scaled(uint32_t px, uint32_t py, char c,
			       uint32_t fg, uint32_t bg, uint32_t scale)
{
	const uint8_t *glyph = anx_font_glyph(c);
	uint32_t row, col, sy, sx;

	for (row = 0; row < ANX_FONT_HEIGHT; row++) {
		uint8_t bits = glyph[row];

		for (col = 0; col < ANX_FONT_WIDTH; col++) {
			uint32_t color = (bits & (0x80 >> col)) ? fg : bg;

			for (sy = 0; sy < scale; sy++) {
				uint32_t y = py + row * scale + sy;

				if (y >= screen_h)
					continue;
				for (sx = 0; sx < scale; sx++) {
					uint32_t x = px + col * scale + sx;

					if (x >= screen_w)
						continue;
					anx_fb_putpixel(x, y, color);
				}
			}
		}
	}
}

void anx_gui_draw_string_scaled(uint32_t x, uint32_t y, const char *s,
				 uint32_t fg, uint32_t bg, uint32_t scale)
{
	while (*s) {
		anx_gui_draw_char_scaled(x, y, *s, fg, bg, scale);
		x += ANX_FONT_WIDTH * scale;
		s++;
	}
}

/* --- Background and layout --- */

static void draw_background(void)
{
	anx_fb_clear(ANX_COLOR_SKY_BLUE);
}

static void draw_topbar(void)
{
	/* Dark blue strip across the top for the clock */
	anx_fb_fill_rect(0, 0, screen_w, ANX_GUI_TOPBAR_HEIGHT,
			  ANX_COLOR_MIDNIGHT);
}

static void draw_terminal_frame(void)
{
	/* Fill the terminal area with midnight blue */
	anx_fb_fill_rect(term_x, term_y, term_w, term_h,
			  ANX_COLOR_MIDNIGHT);
}

/* --- Time display --- */

/* Read a CMOS RTC register */
static uint8_t rtc_read(uint8_t reg)
{
	anx_outb(reg, 0x70);
	return anx_inb(0x71);
}

/* Convert BCD to binary */
static uint8_t bcd_to_bin(uint8_t bcd)
{
	return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static uint8_t last_drawn_min = 0xFF;

void anx_gui_update_time(void)
{
	uint8_t hrs, mins, secs;
	uint8_t status_b;
	char timebuf[16];
	uint32_t time_w, time_x;

	if (!gui_ready)
		return;

	/* Read CMOS RTC (UTC) */
	secs = rtc_read(0x00);
	mins = rtc_read(0x02);
	hrs  = rtc_read(0x04);
	status_b = rtc_read(0x0B);

	(void)secs;

	/* Convert BCD to binary if needed */
	if (!(status_b & 0x04)) {
		secs = bcd_to_bin(secs);
		mins = bcd_to_bin(mins);
		hrs  = bcd_to_bin(hrs);
	}

	/* Apply timezone offset */
	{
		int32_t h = (int32_t)hrs + utc_offset_hours;

		if (h < 0) h += 24;
		if (h >= 24) h -= 24;
		hrs = (uint8_t)h;
	}

	/* Only redraw when the minute changes */
	if (mins == last_drawn_min)
		return;
	last_drawn_min = mins;

	/* Format HH:MM */
	timebuf[0] = '0' + (char)(hrs / 10);
	timebuf[1] = '0' + (char)(hrs % 10);
	timebuf[2] = ':';
	timebuf[3] = '0' + (char)(mins / 10);
	timebuf[4] = '0' + (char)(mins % 10);
	timebuf[5] = '\0';

	/* Center the time string in the top bar */
	time_w = 5 * ANX_FONT_WIDTH * TIME_FONT_SCALE;
	time_x = (screen_w - time_w) / 2;

	/* Clear the time area */
	anx_fb_fill_rect(time_x - 4, 4, time_w + 8,
			  ANX_FONT_HEIGHT * TIME_FONT_SCALE + 4,
			  ANX_COLOR_MIDNIGHT);

	anx_gui_draw_string_scaled(time_x,
				    (ANX_GUI_TOPBAR_HEIGHT -
				     ANX_FONT_HEIGHT * TIME_FONT_SCALE) / 2,
				    timebuf, ANX_COLOR_WHITE,
				    ANX_COLOR_MIDNIGHT, TIME_FONT_SCALE);
}

/* --- Terminal output --- */

static void terminal_scroll(void)
{
	/* Scroll terminal content up by one line */
	uint32_t line_bytes;
	uint32_t y;
	const struct anx_fb_info *info = anx_fb_get_info();

	if (!info)
		return;

	line_bytes = info->pitch;

	/* Move each row up by TERM_CHAR_H pixels */
	for (y = term_y; y < term_y + term_h - TERM_CHAR_H; y++) {
		uint8_t *dst = (uint8_t *)(uintptr_t)info->addr +
			       y * line_bytes;
		uint8_t *src = dst + TERM_CHAR_H * line_bytes;

		anx_memcpy(dst + term_x * 4, src + term_x * 4, term_w * 4);
	}

	/* Clear the last line */
	anx_fb_fill_rect(term_x, term_y + term_h - TERM_CHAR_H,
			  term_w, TERM_CHAR_H, ANX_COLOR_MIDNIGHT);
}

static void terminal_newline(void)
{
	cur_col = 0;
	cur_row++;
	if (cur_row >= term_rows) {
		cur_row = term_rows - 1;
		terminal_scroll();
	}
}

void anx_gui_terminal_putc(char c)
{
	uint32_t px, py;

	if (!gui_ready)
		return;

	switch (c) {
	case '\n':
		terminal_newline();
		return;
	case '\r':
		cur_col = 0;
		return;
	case '\b':
		if (cur_col > 0) {
			cur_col--;
			px = term_x + cur_col * TERM_CHAR_W;
			py = term_y + cur_row * TERM_CHAR_H;
			anx_gui_draw_char_scaled(px, py, ' ',
						  ANX_COLOR_WHITE,
						  ANX_COLOR_MIDNIGHT,
						  TERM_FONT_SCALE);
		}
		return;
	case '\t': {
		uint32_t next = (cur_col + 8) & ~7u;

		if (next >= term_cols)
			next = term_cols - 1;
		cur_col = next;
		return;
	}
	default:
		break;
	}

	if (c < 0x20 || c >= 0x7F)
		return;

	px = term_x + cur_col * TERM_CHAR_W;
	py = term_y + cur_row * TERM_CHAR_H;

	anx_gui_draw_char_scaled(px, py, c,
				  ANX_COLOR_WHITE, ANX_COLOR_MIDNIGHT,
				  TERM_FONT_SCALE);

	cur_col++;
	if (cur_col >= term_cols)
		terminal_newline();
}

/* --- Initialization --- */

void anx_gui_init(void)
{
	const struct anx_fb_info *info;

	info = anx_fb_get_info();
	if (!info || !info->available)
		return;

	screen_w = info->width;
	screen_h = info->height;

	/* Calculate terminal geometry */
	term_x = ANX_GUI_MARGIN;
	term_y = ANX_GUI_TOPBAR_HEIGHT + ANX_GUI_MARGIN;
	term_w = screen_w - 2 * ANX_GUI_MARGIN;
	term_h = screen_h - ANX_GUI_TOPBAR_HEIGHT - 2 * ANX_GUI_MARGIN;

	term_cols = term_w / TERM_CHAR_W;
	term_rows = term_h / TERM_CHAR_H;

	cur_col = 0;
	cur_row = 0;

	/* Draw the environment */
	draw_background();
	draw_topbar();
	draw_terminal_frame();
	anx_gui_update_time();

	gui_ready = true;
}

bool anx_gui_active(void)
{
	return gui_ready;
}
