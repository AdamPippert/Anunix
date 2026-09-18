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
#include <anx/theme.h>
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

/* --- Shape, blend and shadow tests --- */

/*
 * The signature shape rounds the upper-left and lower-right corners and
 * mitres the other two, so a corner pixel is inside on the mitred
 * diagonal but outside the rounded arc at the same offset.
 */
static int test_fb_shape_signature_corners(void)
{
	struct anx_shape sh = anx_fb_shape_signature(10);

	anx_fb_fill_rect(0, 0, 100, 60, 0x000000);
	anx_fb_fill_shape(10, 10, 60, 40, &sh, 0xFF0000);

	ASSERT(sh.corner[0] == ANX_CORNER_ROUND &&
	       sh.corner[2] == ANX_CORNER_ROUND, "UL and LR rounded");
	ASSERT(sh.corner[1] == ANX_CORNER_MITRE &&
	       sh.corner[3] == ANX_CORNER_MITRE, "UR and LL mitred");

	/* Outermost corner pixels are cut on every corner */
	ASSERT(read_pixel(10, 10) == 0x000000, "UL corner cut");
	ASSERT(read_pixel(69, 10) == 0x000000, "UR corner cut");
	ASSERT(read_pixel(10, 49) == 0x000000, "LL corner cut");
	ASSERT(read_pixel(69, 49) == 0x000000, "LR corner cut");

	/* Edge midpoints and the interior are filled */
	ASSERT(read_pixel(40, 10) == 0xFF0000, "top edge filled");
	ASSERT(read_pixel(40, 49) == 0xFF0000, "bottom edge filled");
	ASSERT(read_pixel(10, 30) == 0xFF0000, "left edge filled");
	ASSERT(read_pixel(69, 30) == 0xFF0000, "right edge filled");

	/*
	 * One row down from the top the mitre has eaten 9 columns and the
	 * round corner only 6: the chamfer cuts deeper near the corner.
	 */
	ASSERT(read_pixel(60, 11) == 0xFF0000 &&
	       read_pixel(61, 11) == 0x000000, "mitre cuts 9 columns");
	ASSERT(read_pixel(16, 11) == 0xFF0000 &&
	       read_pixel(15, 11) == 0x000000, "round corner cuts 6");
	return 0;
}

static int test_fb_shape_square_is_a_rect(void)
{
	struct anx_shape sq = anx_fb_shape_uniform(8, ANX_CORNER_SQUARE);

	anx_fb_fill_rect(0, 0, 100, 60, 0x000000);
	anx_fb_fill_shape(10, 10, 40, 30, &sq, 0x00FF00);
	ASSERT(read_pixel(10, 10) == 0x00FF00, "square keeps its corner");
	ASSERT(read_pixel(49, 39) == 0x00FF00, "square keeps all corners");
	ASSERT(read_pixel(50, 40) == 0x000000, "nothing outside");
	return 0;
}

static int test_fb_shape_gradient_runs_top_to_bottom(void)
{
	struct anx_shape sq = anx_fb_shape_uniform(0, ANX_CORNER_SQUARE);
	uint32_t top, bottom;

	anx_fb_fill_shape_gradient(0, 0, 40, 40, &sq, 0x000000, 0xFFFFFF,
				   true);
	top = read_pixel(20, 0);
	bottom = read_pixel(20, 39);
	ASSERT(top == 0x000000, "starts at the first color");
	ASSERT(bottom == 0xFFFFFF, "ends at the second");
	ASSERT(read_pixel(20, 20) > top && read_pixel(20, 20) < bottom,
	       "midpoint between the two");
	return 0;
}

static int test_fb_blend_is_alpha_correct(void)
{
	anx_fb_fill_rect(0, 0, 40, 40, 0x000000);
	anx_fb_blend_rect(0, 0, 20, 20, 0xFFFFFF, 128);
	ASSERT(read_pixel(5, 5) == 0x808080, "half blend of white on black");
	anx_fb_blend_rect(0, 0, 20, 20, 0xFFFFFF, 0);
	ASSERT(read_pixel(5, 5) == 0x808080, "alpha 0 changes nothing");
	anx_fb_blend_rect(0, 0, 20, 20, 0x102030, 255);
	ASSERT(read_pixel(5, 5) == 0x102030, "alpha 255 replaces");
	ASSERT(read_pixel(25, 25) == 0x000000, "outside untouched");
	return 0;
}

/* The shadow darkens outside the frame and never paints over it. */
static int test_fb_shadow_stays_outside(void)
{
	struct anx_shape sh = anx_fb_shape_signature(6);

	anx_fb_fill_rect(0, 0, 200, 120, 0xFFFFFF);
	anx_fb_shadow_shape(60, 40, 60, 40, &sh, 2, 4, 8, 0x000000, 200);

	ASSERT(read_pixel(90, 60) == 0xFFFFFF, "inside the frame is clean");
	ASSERT(read_pixel(90, 86) < 0xFFFFFF, "shadow below the frame");
	ASSERT(read_pixel(90, 84) < read_pixel(90, 90),
	       "shadow fades with distance");
	ASSERT(read_pixel(5, 5) == 0xFFFFFF, "far away is untouched");
	return 0;
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
	const uint16_t *glyph;

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
	const uint16_t *glyph;
	int i;

	glyph = anx_font_glyph(' ');
	ASSERT(glyph != NULL, "glyph for ' ' should not be NULL");

	for (i = 0; i < ANX_FONT_HEIGHT; i++)
		ASSERT(glyph[i] == 0, "space glyph should be all zeros");

	return 0;
}

static int test_font_glyph_unprintable_returns_default(void)
{
	const uint16_t *glyph;
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
	struct anx_theme saved_theme = *anx_theme_get();
	struct anx_theme bitmap_theme = saved_theme;
	uint32_t fg = 0x00FFFFFF;
	uint32_t bg = 0x00000000;
	const uint16_t *glyph;
	uint32_t y;

	/* This legacy assertion checks the binary glyph contract, not coverage. */
	bitmap_theme.font.antialiased = false;
	anx_theme_restore(&bitmap_theme);
	anx_fb_clear(bg);
	anx_font_draw_char(0, 0, 'A', fg, bg);
	anx_theme_restore(&saved_theme);

	/* Verify that drawn pixels match the glyph bitmap.
	 * Each row is 12 bits; bit 11 (0x800) is the leftmost pixel. */
	glyph = anx_font_glyph('A');
	for (y = 0; y < ANX_FONT_HEIGHT; y++) {
		uint32_t x;
		for (x = 0; x < ANX_FONT_WIDTH; x++) {
			uint32_t expected;
			bool set = (glyph[y] & (0x800u >> x)) != 0;
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

/*
 * The console has two bottom-of-screen behaviours and both must keep the
 * cursor on screen under sustained output. Paging clears and restarts at the
 * top, which is what boot uses because it never reads the framebuffer.
 * Scrolling preserves the text above the cursor and costs a full-screen read
 * and write.
 */
static int test_fbcon_scroll_at_bottom(void)
{
	uint32_t rows = anx_fbcon_rows();
	uint32_t i;

	ASSERT(rows > 1, "console has more than one row");

	/* ---- paging ---- */
	anx_fbcon_set_paging(true);
	anx_fbcon_clear();
	for (i = 0; i < rows; i++)
		anx_fbcon_putc('\n');
	ASSERT(anx_fbcon_cursor_y() == 0,
	      "paging restarts at the top of the screen");

	for (i = 0; i < rows * 3 + 5; i++) {
		anx_fbcon_putc('x');
		anx_fbcon_putc('\n');
		ASSERT(anx_fbcon_cursor_y() < rows,
		      "paging keeps the cursor on screen");
	}
	anx_fbcon_putc('A');
	ASSERT(anx_fbcon_cursor_x() == 1, "column advances after a page turn");

	/* ---- scrolling ---- */
	anx_fbcon_set_paging(false);
	anx_fbcon_clear();
	for (i = 0; i < rows; i++)
		anx_fbcon_putc('\n');
	ASSERT(anx_fbcon_cursor_y() < rows,
	      "scrolling keeps the cursor on screen");
	ASSERT(anx_fbcon_cursor_y() != 0,
	      "scrolling does not restart at the top -- that is paging");
	ASSERT(anx_fbcon_cursor_y() < rows - 1,
	      "a scroll frees more than one row");

	for (i = 0; i < rows * 3 + 5; i++) {
		anx_fbcon_putc('x');
		anx_fbcon_putc('\n');
		ASSERT(anx_fbcon_cursor_y() < rows,
		      "scrolling keeps the cursor on screen under load");
	}

	/* Leave the console as boot expects to find it. */
	anx_fbcon_set_paging(true);
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

static int test_fbcon_cursor_motion(void)
{
	uint32_t before[TEST_FB_WIDTH / ANX_FONT_WIDTH * ANX_FONT_WIDTH * ANX_FONT_HEIGHT];
	uint32_t i, count = sizeof(before) / sizeof(before[0]);

	anx_fbcon_init();
	anx_fbcon_clear();
	anx_fbcon_puts("editing");
	for (i = 0; i < count; i++) before[i] = ((uint32_t *)test_fb_mem)[i];
	anx_fbcon_move_cursor(-3);
	ASSERT(anx_fbcon_cursor_x() == 4, "move left without deleting");
	for (i = 0; i < count; i++)
		ASSERT(before[i] == ((uint32_t *)test_fb_mem)[i], "cursor motion preserves pixels");
	anx_fbcon_move_cursor((int32_t)anx_fbcon_cols());
	ASSERT(anx_fbcon_cursor_y() == 1 && anx_fbcon_cursor_x() == 4, "move across row");
	anx_fbcon_move_cursor(-5);
	ASSERT(anx_fbcon_cursor_y() == 0 && anx_fbcon_cursor_x() == anx_fbcon_cols() - 1,
	       "move left through row boundary");
	anx_fbcon_move_cursor(-2147483647);
	ASSERT(anx_fbcon_cursor_x() == 0 && anx_fbcon_cursor_y() == 0, "clamp before origin");
	anx_fbcon_move_cursor(2147483647);
	ASSERT(anx_fbcon_cursor_x() == anx_fbcon_cols() - 1 &&
	       anx_fbcon_cursor_y() == anx_fbcon_rows() - 1, "clamp after screen");
	return 0;
}

/* --- Test runner --- */

typedef int (*test_fn)(void);

struct fb_test {
	const char *name;
	test_fn fn;
};

static struct fb_test fb_tests[] = {
	{ "fbcon_cursor_motion", test_fbcon_cursor_motion },
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
	{ "fb_shape_signature_corners",		test_fb_shape_signature_corners },
	{ "fb_shape_square_is_a_rect",		test_fb_shape_square_is_a_rect },
	{ "fb_shape_gradient",			test_fb_shape_gradient_runs_top_to_bottom },
	{ "fb_blend_alpha",			test_fb_blend_is_alpha_correct },
	{ "fb_shadow_outside",			test_fb_shadow_stays_outside },
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
