/*
 * gui.c — Graphical user environment.
 *
 * Aether design language:
 * - Deep navy (#0B1A2B) desktop wallpaper
 * - Floating topbar pill (navy-800, 10 px inset) with 1 px teal accent border
 * - Navy-900 (#0E2338) terminal window, 14 px margins
 * - Warm paper-white (#F7F5F1) terminal text
 * - Functional anx> shell
 */

#include <anx/types.h>
#include <anx/gui.h>
#include <anx/fb.h>
#include <anx/kprintf.h>
#include <anx/perf.h>
#include <anx/font.h>
#include <anx/io.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/civil.h>
#include <anx/net.h>

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

/* Computed at init time from framebuffer width; see anx_gui_init() */
static uint32_t term_font_scale;
static uint32_t time_font_scale;
static uint32_t term_char_w;
static uint32_t term_char_h;

static uint32_t topbar_inset;	/* pill gap from screen edges (px) */
static uint32_t topbar_h;	/* pill height, computed at init */

/* --- Scaled font rendering --- */

void anx_gui_draw_char_scaled(uint32_t px, uint32_t py, char c,
			       uint32_t fg, uint32_t bg, uint32_t scale)
{
	anx_font_draw_char_scaled(px, py, c, fg, bg, scale);
}

void anx_gui_draw_string_scaled(uint32_t x, uint32_t y, const char *s,
				 uint32_t fg, uint32_t bg, uint32_t scale)
{
	uint32_t n = 0;

	for (n = 0; s[n]; n++)
		;
	anx_fb_mark_dirty(x, y, n * ANX_FONT_WIDTH * scale,
			  ANX_FONT_HEIGHT * scale);

	while (*s) {
		anx_gui_draw_char_scaled(x, y, *s, fg, bg, scale);
		x += ANX_FONT_WIDTH * scale;
		s++;
	}
}

/* --- Background and layout --- */

static void draw_background(void)
{
	anx_fb_clear(ANX_COLOR_AX_BG);
}

static void draw_topbar(void)
{
	uint32_t bw = screen_w > 2 * topbar_inset
		      ? screen_w - 2 * topbar_inset : screen_w;

	/* Floating panel pill: inset from all screen edges */
	anx_fb_fill_rect(topbar_inset, topbar_inset, bw, topbar_h,
			  ANX_COLOR_AX_PANEL);
	/* 1 px teal accent line at the bottom of the pill (E17-style bevel) */
	anx_fb_fill_rect(topbar_inset, topbar_inset + topbar_h - 1, bw, 1,
			  ANX_COLOR_AX_TEAL);
}

static void draw_terminal_frame(void)
{
	/* Fill the terminal area */
	anx_fb_fill_rect(term_x, term_y, term_w, term_h,
			  ANX_COLOR_AX_SURFACE);
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

void anx_gui_get_time(char *buf, uint32_t buflen)
{
	uint8_t hrs, mins, secs, status_b;
	uint32_t now = anx_ntp_unix_time();
	int32_t h;

	if (buflen < 6)
		return;

	if (now) {
		struct anx_civil c;

		anx_civil_from_unix(now, utc_offset_hours, &c);
		anx_snprintf(buf, buflen, "%02u:%02u", c.hour, c.min);
		return;
	}

	secs     = rtc_read(0x00);
	mins     = rtc_read(0x02);
	hrs      = rtc_read(0x04);
	status_b = rtc_read(0x0B);
	(void)secs;

	if (!(status_b & 0x04)) {
		mins = bcd_to_bin(mins);
		hrs  = bcd_to_bin(hrs);
	}

	h = (int32_t)hrs + utc_offset_hours;
	if (h < 0)  h += 24;
	if (h >= 24) h -= 24;

	buf[0] = '0' + (char)(h / 10);
	buf[1] = '0' + (char)(h % 10);
	buf[2] = ':';
	buf[3] = '0' + (char)(mins / 10);
	buf[4] = '0' + (char)(mins % 10);
	buf[5] = '\0';
}

void anx_gui_get_date(char *buf, uint32_t buflen)
{
	static const char * const day_names[8] = {
		"???", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
	};
	uint8_t dow, day, status_b;
	const char *dn;
	uint32_t i;

	if (buflen < 8)
		return;

	if (anx_ntp_unix_time()) {
		struct anx_civil c;

		anx_civil_from_unix(anx_ntp_unix_time(), utc_offset_hours, &c);
		anx_snprintf(buf, buflen, "%s %02u", anx_civil_day_name(c.wday),
			     c.day);
		return;
	}

	dow      = rtc_read(0x06);   /* CMOS weekday: 1=Sunday..7=Saturday */
	day      = rtc_read(0x07);   /* day of month (BCD or binary) */
	status_b = rtc_read(0x0B);

	if (!(status_b & 0x04)) {
		day = bcd_to_bin(day);
		/* weekday register is 1-7 — not BCD */
	}

	if (dow < 1 || dow > 7)
		dow = 0;
	dn = day_names[dow];

	/* "Mon 26" */
	for (i = 0; i < 3; i++)
		buf[i] = dn[i];
	buf[3] = ' ';
	buf[4] = '0' + (char)(day / 10);
	buf[5] = '0' + (char)(day % 10);
	buf[6] = '\0';
}

void anx_gui_update_time(void)
{
	char timebuf[16];
	uint32_t time_w, time_x;
	uint8_t mins;

	if (!gui_ready)
		return;

	/* Only redraw when the minute changes */
	mins = bcd_to_bin(rtc_read(0x02));
	if (!(rtc_read(0x0B) & 0x04))
		mins = bcd_to_bin(rtc_read(0x02));
	if (mins == last_drawn_min)
		return;
	last_drawn_min = mins;

	anx_gui_get_time(timebuf, sizeof(timebuf));

	/* Center the time string in the top bar */
	time_w = 5 * ANX_FONT_WIDTH * time_font_scale;
	time_x = (screen_w - time_w) / 2;

	/* Clear the time area within the panel pill */
	anx_fb_fill_rect(time_x - 4, topbar_inset + 2,
			  time_w + 8, topbar_h - 4,
			  ANX_COLOR_AX_PANEL);

	anx_gui_draw_string_scaled(time_x,
				    topbar_inset +
				    (topbar_h - ANX_FONT_HEIGHT * time_font_scale) / 2,
				    timebuf, ANX_COLOR_WHITE,
				    ANX_COLOR_AX_PANEL, time_font_scale);
}

/* --- Terminal output --- */

/*
 * Reaching the bottom of the boot terminal.
 *
 * This used to scroll with a memmove of nearly the whole panel inside the
 * framebuffer -- about 15 MiB per line on a 2560x1600 panel, and the read
 * half of it comes back from video memory, which is slow however the
 * mapping is cached. Each new line visibly wiped down the screen for about
 * a second; video of the Framework boot shows it clearly. The console
 * paging added to fbcon.c never helped, because while this panel is up
 * fbcon hands every character straight here.
 *
 * Page instead: clear the panel and start again at the top. That is one
 * write-only fill per screenful and no read at all. The serial log and the
 * boot-log ring keep every line, so only the pixels are lost.
 *
 * anx_gui_set_paging(false) restores scrolling for callers that want the
 * history on screen.
 */
static bool term_paging = true;

void anx_gui_set_paging(bool paging)
{
	term_paging = paging;
}

static void terminal_scroll(void)
{
	const struct anx_fb_info *info = anx_fb_get_info();
	uint8_t *base;
	uint32_t pitch;

	if (!info)
		return;

	pitch = info->pitch;
	base  = (uint8_t *)(uintptr_t)info->addr;

	anx_memmove(base + term_y * pitch,
		    base + (term_y + term_char_h) * pitch,
		    (term_h - term_char_h) * pitch);

	/* Clear the last character row */
	anx_fb_fill_rect(term_x, term_y + term_h - term_char_h,
			  term_w, term_char_h, ANX_COLOR_AX_SURFACE);
}

static void terminal_newline(void)
{
	cur_col = 0;
	cur_row++;
	if (cur_row < term_rows)
		return;

	if (term_paging) {
		anx_fb_fill_rect(term_x, term_y, term_w, term_h,
				  ANX_COLOR_AX_SURFACE);
		cur_row = 0;
	} else {
		cur_row = term_rows - 1;
		terminal_scroll();
	}
}

void anx_gui_terminal_move_cursor(int32_t cells)
{
	int64_t offset;

	if (!gui_ready || !term_cols || !term_rows) return;
	offset = (int64_t)cur_row * term_cols + cur_col + cells;
	if (offset < 0) offset = 0;
	if (offset >= (int64_t)term_rows * term_cols)
		offset = (int64_t)term_rows * term_cols - 1;
	cur_row = (uint32_t)offset / term_cols;
	cur_col = (uint32_t)offset % term_cols;
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
			px = term_x + cur_col * term_char_w;
			py = term_y + cur_row * term_char_h;
			anx_gui_draw_char_scaled(px, py, ' ',
						  ANX_COLOR_AX_TEXT,
						  ANX_COLOR_AX_SURFACE,
						  term_font_scale);
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

	px = term_x + cur_col * term_char_w;
	py = term_y + cur_row * term_char_h;

	anx_gui_draw_char_scaled(px, py, c,
				  ANX_COLOR_AX_TEXT, ANX_COLOR_AX_SURFACE,
				  term_font_scale);

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

	/* DPI-aware scale: pick readable font size for this resolution */
	if (screen_w >= 3840)
		term_font_scale = 4;
	else if (screen_w >= 2560)
		term_font_scale = 3;
	else if (screen_w >= 1920)
		term_font_scale = 2;
	else
		term_font_scale = 1;
	time_font_scale = term_font_scale + 1;
	term_char_w     = ANX_FONT_WIDTH  * term_font_scale;
	term_char_h     = ANX_FONT_HEIGHT * term_font_scale;

	/* Topbar pill: 10 px inset from edges; height sized to fit the clock */
	topbar_inset = 10;
	topbar_h     = ANX_FONT_HEIGHT * time_font_scale + 16;

	/* Terminal: below the pill, 14 px margins on all sides */
	term_x = ANX_GUI_MARGIN;
	term_y = topbar_inset + topbar_h + ANX_GUI_MARGIN;
	term_w = screen_w - 2 * ANX_GUI_MARGIN;
	term_h = screen_h - term_y - ANX_GUI_MARGIN;

	term_cols = term_w / term_char_w;
	term_rows = term_h / term_char_h;

	cur_col = 0;
	cur_row = 0;

	/* Draw the environment */
	draw_background();
	draw_topbar();
	draw_terminal_frame();
	anx_gui_update_time();

	gui_ready = true;
}

/*
 * Time the framebuffer operations the boot panel depends on, on whatever
 * hardware is running, and log the result.
 *
 * QEMU's framebuffer is ordinary RAM, where reading pixels back costs the
 * same as writing them, so it cannot show the cost that made the panel
 * slow on real hardware. These numbers come from the machine itself and
 * land in the boot-log ring.
 *
 * The TSC is uncalibrated, so the cycle counts are converted with the
 * 100 Hz timer when it is ticking; otherwise raw cycles are reported.
 */
void anx_gui_benchmark(void)
{
	const struct anx_fb_info *info = anx_fb_get_info();
	uint64_t hz = 0, t0, t_fill, t_scroll, t_line, t_read;
	uint64_t tick0, c0;
	uint8_t *base;
	uint32_t i, row;
	volatile uint32_t sum = 0;

	if (!gui_ready || !info)
		return;

	/* Calibrate: cycles across 10 timer ticks, if ticks advance. */
	tick0 = arch_timer_ticks();
	c0 = anx_rdtsc();
	for (i = 0; i < 20000000u && arch_timer_ticks() == tick0; i++)
		;
	if (arch_timer_ticks() != tick0) {
		tick0 = arch_timer_ticks();
		c0 = anx_rdtsc();
		while (arch_timer_ticks() - tick0 < 10)
			;
		hz = (anx_rdtsc() - c0) * 10;	/* 10 ticks = 100 ms */
	}

	base = (uint8_t *)(uintptr_t)info->addr;

	t0 = anx_rdtsc();
	anx_fb_fill_rect(term_x, term_y, term_w, term_h, ANX_COLOR_AX_SURFACE);
	t_fill = anx_rdtsc() - t0;

	t0 = anx_rdtsc();
	anx_memmove(base + term_y * info->pitch,
		    base + (term_y + term_char_h) * info->pitch,
		    (term_h - term_char_h) * info->pitch);
	t_scroll = anx_rdtsc() - t0;

	t0 = anx_rdtsc();
	for (i = 0; i < term_cols; i++)
		anx_gui_draw_char_scaled(term_x + i * term_char_w, term_y,
					  (char)('A' + i % 26),
					  ANX_COLOR_AX_TEXT,
					  ANX_COLOR_AX_SURFACE,
					  term_font_scale);
	t_line = anx_rdtsc() - t0;

	t0 = anx_rdtsc();
	for (row = term_y; row < term_y + term_h; row++) {
		volatile uint32_t *p = anx_fb_row_ptr(row);

		/* volatile: the reads must happen, not be optimised out */
		for (i = 0; i < info->width; i += 4)
			sum += p[i];
	}
	t_read = anx_rdtsc() - t0;
	(void)sum;

	anx_fb_fill_rect(term_x, term_y, term_w, term_h, ANX_COLOR_AX_SURFACE);
	cur_row = 0;
	cur_col = 0;

	if (hz) {
		kprintf("fbbench: tsc %llu Hz; panel %ux%u; fill %llu us, "
			"scroll-copy %llu us, text line %llu us, "
			"read quarter-panel %llu us\n",
			(unsigned long long)hz, term_w, term_h,
			(unsigned long long)(t_fill * 1000000ULL / hz),
			(unsigned long long)(t_scroll * 1000000ULL / hz),
			(unsigned long long)(t_line * 1000000ULL / hz),
			(unsigned long long)(t_read * 1000000ULL / hz));
	} else {
		kprintf("fbbench: timer not ticking, raw cycles; panel %ux%u; "
			"fill %llu, scroll-copy %llu, text line %llu, "
			"read quarter-panel %llu\n", term_w, term_h,
			(unsigned long long)t_fill,
			(unsigned long long)t_scroll,
			(unsigned long long)t_line,
			(unsigned long long)t_read);
	}
}

bool anx_gui_active(void)
{
	return gui_ready;
}

void anx_gui_terminal_clear(void)
{
	if (!gui_ready)
		return;
	anx_fb_fill_rect(term_x, term_y, term_w, term_h, ANX_COLOR_AX_SURFACE);
	cur_col = 0;
	cur_row = 0;
}

void anx_gui_disable(void)
{
	gui_ready = false;
}

int32_t anx_gui_get_tz_offset(void)
{
	return utc_offset_hours;
}
