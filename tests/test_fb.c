/*
 * test_fb.c — Tests for framebuffer, font, and fbcon subsystems.
 *
 * Uses a mock framebuffer buffer on the host to verify pixel
 * operations, font rendering, and console behavior.
 */

#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/fbcon.h>

/* 80x25 character cells at 8x16 = 640x400 pixels */
#define TEST_FB_WIDTH	640
#define TEST_FB_HEIGHT	400
#define TEST_FB_BPP	32
#define TEST_FB_PITCH	(TEST_FB_WIDTH * (TEST_FB_BPP / 8))

static uint8_t test_fb_mem[TEST_FB_HEIGHT * TEST_FB_PITCH];

#define ASSERT(cond, msg) do { \
	if (!(cond)) { \
		kprintf("    ASSERT FAILED: %s (%s:%d)\n", \
			msg, __FILE__, __LINE__); \
		return -1; \
	} \
} while (0)

/* Read pixel at (x,y) from the mock framebuffer */
static uint32_t read_pixel(uint32_t x, uint32_t y)
{
	uint32_t *row = (uint32_t *)(test_fb_mem + y * TEST_FB_PITCH);
	return row[x];
}

/* --- Framebuffer core tests --- */

static int test_fb_init_sets_available(void)
{
	struct anx_fb_info info = {
		.addr = (uint64_t)(uintptr_t)test_fb_mem,
		.width = TEST_FB_WIDTH,
		.height = TEST_FB_HEIGHT,
		.pitch = TEST_FB_PITCH,
		.bpp = TEST_FB_BPP,
		.available = true,
	};
	int ret;

	ret = anx_fb_init(&info);
	ASSERT(ret == 0, "fb_init should succeed");
	ASSERT(anx_fb_available(), "fb should be available after init");

	return 0;
}

static int test_fb_putpixel_writes_correct_color(void)
{
	uint32_t color = 0x00FF8800;

	/* Clear first */
	for (size_t i = 0; i < sizeof(test_fb_mem); i++)
		test_fb_mem[i] = 0;

	anx_fb_putpixel(10, 20, color);
	ASSERT(read_pixel(10, 20) == color,
	       "putpixel should write correct color");
	ASSERT(read_pixel(11, 20) == 0,
	       "putpixel should not affect adjacent pixels");

	return 0;
}

static int test_fb_putpixel_bounds_check(void)
{
	/* Out of bounds writes should be silently ignored */
	anx_fb_putpixel(TEST_FB_WIDTH, 0, 0xFFFFFFFF);
	anx_fb_putpixel(0, TEST_FB_HEIGHT, 0xFFFFFFFF);
	anx_fb_putpixel(TEST_FB_WIDTH + 100, TEST_FB_HEIGHT + 100, 0xFFFFFFFF);

	/* If we got here without crashing, bounds checking works */
	return 0;
}

static int test_fb_fill_rect(void)
{
	uint32_t color = 0x00112233;

	anx_fb_clear(0);

	anx_fb_fill_rect(5, 10, 3, 2, color);

	/* Inside the rect */
	ASSERT(read_pixel(5, 10) == color, "fill_rect top-left");
	ASSERT(read_pixel(7, 10) == color, "fill_rect top-right");
	ASSERT(read_pixel(5, 11) == color, "fill_rect bottom-left");
	ASSERT(read_pixel(7, 11) == color, "fill_rect bottom-right");
	ASSERT(read_pixel(6, 10) == color, "fill_rect interior");

	/* Outside the rect */
	ASSERT(read_pixel(4, 10) == 0, "fill_rect left neighbor");
	ASSERT(read_pixel(8, 10) == 0, "fill_rect right neighbor");
	ASSERT(read_pixel(5, 9) == 0, "fill_rect top neighbor");
	ASSERT(read_pixel(5, 12) == 0, "fill_rect bottom neighbor");

	return 0;
}

static int test_fb_clear(void)
{
	uint32_t color = 0x00AABBCC;

	anx_fb_clear(color);
	ASSERT(read_pixel(0, 0) == color, "clear top-left");
	ASSERT(read_pixel(TEST_FB_WIDTH - 1, 0) == color, "clear top-right");
	ASSERT(read_pixel(0, TEST_FB_HEIGHT - 1) == color, "clear bottom-left");

	return 0;
}

static int test_fb_scroll(void)
{
	uint32_t marker = 0x00DEAD00;
	uint32_t fill = 0x00000000;

	anx_fb_clear(fill);

	/* Put a marker pixel on row 20 */
	anx_fb_putpixel(0, 20, marker);
	ASSERT(read_pixel(0, 20) == marker, "marker placed");

	/* Scroll up by 5 rows */
	anx_fb_scroll(5, fill);

	/* Marker should now be at row 15 */
	ASSERT(read_pixel(0, 15) == marker, "marker scrolled up");
	ASSERT(read_pixel(0, 20) != marker, "old marker position cleared");

	/* Bottom 5 rows should be fill color */
	ASSERT(read_pixel(0, TEST_FB_HEIGHT - 1) == fill,
	       "bottom filled after scroll");

	return 0;
}

/* --- Font tests --- */

static int test_font_glyph_returns_data(void)
{
	const uint8_t *glyph;

	glyph = anx_font_glyph('A');
	ASSERT(glyph != NULL, "glyph for 'A' should not be NULL");

	/* 'A' should have some non-zero rows (it's not blank) */
	{
		int nonzero = 0;
		int i;
		for (i = 0; i < ANX_FONT_HEIGHT; i++) {
			if (glyph[i] != 0)
				nonzero++;
		}
		ASSERT(nonzero > 0, "'A' glyph should have non-zero rows");
	}

	return 0;
}

static int test_font_glyph_space_is_blank(void)
{
	const uint8_t *glyph;
	int i;

	glyph = anx_font_glyph(' ');
	ASSERT(glyph != NULL, "glyph for ' ' should not be NULL");

	for (i = 0; i < ANX_FONT_HEIGHT; i++)
		ASSERT(glyph[i] == 0, "space glyph should be all zeros");

	return 0;
}

static int test_font_glyph_unprintable_returns_default(void)
{
	const uint8_t *glyph;
	int nonzero = 0;
	int i;

	/* Control character should return a fallback glyph */
	glyph = anx_font_glyph('\x01');
	ASSERT(glyph != NULL, "unprintable should return non-NULL glyph");

	/* The fallback glyph should be visible (filled block or similar) */
	for (i = 0; i < ANX_FONT_HEIGHT; i++) {
		if (glyph[i] != 0)
			nonzero++;
	}
	ASSERT(nonzero > 0, "fallback glyph should be visible");

	return 0;
}

static int test_font_draw_char_pixels(void)
{
	uint32_t fg = 0x00FFFFFF;
	uint32_t bg = 0x00000000;
	const uint8_t *glyph;
	uint32_t y;

	anx_fb_clear(bg);
	anx_font_draw_char(0, 0, 'A', fg, bg);

	/* Verify that drawn pixels match the glyph bitmap */
	glyph = anx_font_glyph('A');
	for (y = 0; y < ANX_FONT_HEIGHT; y++) {
		uint32_t x;
		for (x = 0; x < ANX_FONT_WIDTH; x++) {
			uint32_t expected;
			bool set = (glyph[y] >> (7 - x)) & 1;
			expected = set ? fg : bg;
			ASSERT(read_pixel(x, y) == expected,
			       "drawn char pixel mismatch");
		}
	}

	return 0;
}

/* --- Framebuffer console tests --- */

static int test_fbcon_init_dimensions(void)
{
	int ret;

	ret = anx_fbcon_init();
	ASSERT(ret == 0, "fbcon_init should succeed");
	ASSERT(anx_fbcon_active(), "fbcon should be active");
	ASSERT(anx_fbcon_cols() == TEST_FB_WIDTH / ANX_FONT_WIDTH,
	       "cols should be fb_width / font_width");
	ASSERT(anx_fbcon_rows() == TEST_FB_HEIGHT / ANX_FONT_HEIGHT,
	       "rows should be fb_height / font_height");

	return 0;
}

static int test_fbcon_putc_advances_cursor(void)
{
	anx_fbcon_clear();
	ASSERT(anx_fbcon_cursor_x() == 0, "cursor starts at x=0");
	ASSERT(anx_fbcon_cursor_y() == 0, "cursor starts at y=0");

	anx_fbcon_putc('A');
	ASSERT(anx_fbcon_cursor_x() == 1, "cursor advances to x=1");
	ASSERT(anx_fbcon_cursor_y() == 0, "cursor stays at y=0");

	return 0;
}

static int test_fbcon_newline(void)
{
	anx_fbcon_clear();
	anx_fbcon_putc('A');
	anx_fbcon_putc('\n');
	ASSERT(anx_fbcon_cursor_x() == 0, "newline resets x to 0");
	ASSERT(anx_fbcon_cursor_y() == 1, "newline advances y");

	return 0;
}

static int test_fbcon_line_wrap(void)
{
	uint32_t cols = anx_fbcon_cols();
	uint32_t i;

	anx_fbcon_clear();

	/* Fill an entire row */
	for (i = 0; i < cols; i++)
		anx_fbcon_putc('X');

	ASSERT(anx_fbcon_cursor_x() == 0, "wraps to x=0");
	ASSERT(anx_fbcon_cursor_y() == 1, "wraps to next line");

	return 0;
}

static int test_fbcon_scroll_at_bottom(void)
{
	uint32_t rows = anx_fbcon_rows();
	uint32_t i;

	anx_fbcon_clear();

	/* Fill all rows */
	for (i = 0; i < rows; i++)
		anx_fbcon_putc('\n');

	/* Cursor should still be on the last row (scrolled) */
	ASSERT(anx_fbcon_cursor_y() == rows - 1,
	       "cursor stays on last row after scroll");

	return 0;
}

static int test_fbcon_carriage_return(void)
{
	anx_fbcon_clear();
	anx_fbcon_putc('A');
	anx_fbcon_putc('B');
	anx_fbcon_putc('\r');
	ASSERT(anx_fbcon_cursor_x() == 0, "CR resets x to 0");
	ASSERT(anx_fbcon_cursor_y() == 0, "CR stays on same line");

	return 0;
}

static int test_fbcon_backspace(void)
{
	anx_fbcon_clear();
	anx_fbcon_putc('A');
	anx_fbcon_putc('B');
	ASSERT(anx_fbcon_cursor_x() == 2, "at x=2 after AB");

	anx_fbcon_putc('\b');
	ASSERT(anx_fbcon_cursor_x() == 1, "backspace moves back");

	/* Backspace at column 0 should not go negative */
	anx_fbcon_clear();
	anx_fbcon_putc('\b');
	ASSERT(anx_fbcon_cursor_x() == 0, "backspace at 0 stays at 0");

	return 0;
}

static int test_fbcon_tab(void)
{
	anx_fbcon_clear();
	anx_fbcon_putc('\t');
	ASSERT(anx_fbcon_cursor_x() == 8, "tab advances to column 8");

	return 0;
}

/* --- Test runner --- */

typedef int (*test_fn)(void);

struct fb_test {
	const char *name;
	test_fn fn;
};

static struct fb_test fb_tests[] = {
	{ "fb_init_sets_available",		test_fb_init_sets_available },
	{ "fb_putpixel_writes_correct_color",	test_fb_putpixel_writes_correct_color },
	{ "fb_putpixel_bounds_check",		test_fb_putpixel_bounds_check },
	{ "fb_fill_rect",			test_fb_fill_rect },
	{ "fb_clear",				test_fb_clear },
	{ "fb_scroll",				test_fb_scroll },
	{ "font_glyph_returns_data",		test_font_glyph_returns_data },
	{ "font_glyph_space_is_blank",		test_font_glyph_space_is_blank },
	{ "font_glyph_unprintable_default",	test_font_glyph_unprintable_returns_default },
	{ "font_draw_char_pixels",		test_font_draw_char_pixels },
	{ "fbcon_init_dimensions",		test_fbcon_init_dimensions },
	{ "fbcon_putc_advances_cursor",		test_fbcon_putc_advances_cursor },
	{ "fbcon_newline",			test_fbcon_newline },
	{ "fbcon_line_wrap",			test_fbcon_line_wrap },
	{ "fbcon_scroll_at_bottom",		test_fbcon_scroll_at_bottom },
	{ "fbcon_carriage_return",		test_fbcon_carriage_return },
	{ "fbcon_backspace",			test_fbcon_backspace },
	{ "fbcon_tab",				test_fbcon_tab },
};

#define NUM_FB_TESTS (sizeof(fb_tests) / sizeof(fb_tests[0]))

int test_fb(void)
{
	uint32_t i;
	int failures = 0;

	/* Set up mock framebuffer for all tests */
	struct anx_fb_info info = {
		.addr = (uint64_t)(uintptr_t)test_fb_mem,
		.width = TEST_FB_WIDTH,
		.height = TEST_FB_HEIGHT,
		.pitch = TEST_FB_PITCH,
		.bpp = TEST_FB_BPP,
		.available = true,
	};
	anx_fb_init(&info);

	for (i = 0; i < NUM_FB_TESTS; i++) {
		int ret = fb_tests[i].fn();
		if (ret != 0) {
			kprintf("      FAIL: %s\n", fb_tests[i].name);
			failures++;
		}
	}

	return failures;
}
