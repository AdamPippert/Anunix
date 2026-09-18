/* Exercise public raster APIs; font-source arrays deliberately stay opaque. */
#include <anx/types.h>
#include <anx/font.h>
#include <anx/fb.h>
#include <anx/theme.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define CELL (ANX_FONT_WIDTH * ANX_FONT_HEIGHT)
#define STRIDE 40u
#define HEIGHT 64u
#define SENTINEL 0x13579bdfu
#define CHECK(c) do { if (!(c)) { kprintf("font families: line %u\n", __LINE__); return -__LINE__; } } while (0)

static uint32_t raster[3][CELL * 4];
static uint16_t bitmap[3][4][ANX_FONT_HEIGHT];
static uint32_t reference[CELL];
static uint32_t composite[CELL];
static uint32_t frame[STRIDE * HEIGHT];
static struct {
	uint32_t before[8];
	uint32_t pixels[STRIDE * HEIGHT];
	uint32_t after[8];
} guarded;

static void fill(uint32_t *pixels, uint32_t count, uint32_t color)
{
	uint32_t i;

	for (i = 0; i < count; i++) pixels[i] = color;
}

static void select_font(enum anx_font_family family, bool aa)
{
	struct anx_theme theme = *anx_theme_get();

	theme.font.family = family;
	theme.font.antialiased = aa;
	anx_theme_restore(&theme);
}

static int test_distinct_families(void)
{
	const char sample[] = "Ag0@";
	uint32_t f, g, i;

	for (f = 0; f < 3; f++) {
		select_font((enum anx_font_family)f, true);
		for (g = 0; g < 4; g++) {
			const uint16_t *glyph = anx_font_glyph(sample[g]);

			CHECK(glyph != NULL);
			anx_memcpy(bitmap[f][g], glyph, sizeof(bitmap[f][g]));
			CHECK(!anx_memcmp(glyph, anx_font_glyph_cp((uint8_t)sample[g]),
					 sizeof(bitmap[f][g])));
		}
		fill(raster[f], CELL * 4, SENTINEL);
		anx_font_blit_str(raster[f], ANX_FONT_WIDTH * 4, ANX_FONT_HEIGHT,
				  0, 0, sample, 0xffffff, 0);
		for (i = 0; i < CELL * 4; i++) CHECK(raster[f][i] != SENTINEL);
	}
	for (f = 0; f < 3; f++) {
		for (g = f + 1; g < 3; g++) {
			CHECK(anx_memcmp(bitmap[f], bitmap[g], sizeof(bitmap[f])) != 0);
			CHECK(anx_memcmp(raster[f], raster[g], sizeof(raster[f])) != 0);
		}
	}
	return 0;
}

static int test_coverage_and_compositing(void)
{
	uint32_t f, i;

	for (f = 0; f < 3; f++) {
		uint32_t intermediate = 0, background = 0;

		select_font((enum anx_font_family)f, true);
		fill(reference, CELL, 0x13c457);
		anx_font_blit_char(reference, ANX_FONT_WIDTH, ANX_FONT_HEIGHT,
				   0, 0, 'A', 0xffffff, 0);
		for (i = 0; i < CELL; i++) {
			uint32_t p = reference[i], grey = p & 255;

			/* An opaque grayscale glyph must replace every prior green pixel. */
			CHECK(p == grey * 0x010101);
			if (p == 0) background++;
			else if (p != 0xffffff) intermediate++;
			composite[i] = i & 1 ? 0x204060 : 0x804020;
		}
		CHECK(background && intermediate);
		anx_font_blit_char(composite, ANX_FONT_WIDTH, ANX_FONT_HEIGHT,
				   0, 0, 'A', 0xffffff, ANX_FONT_TRANSPARENT);
		for (i = 0; i < CELL; i++) {
			uint32_t old = i & 1 ? 0x204060 : 0x804020;
			uint32_t coverage = reference[i] & 255, shift;

			if (!coverage) CHECK(composite[i] == old);
			if (coverage == 255) CHECK(composite[i] == 0xffffff);
			for (shift = 0; shift < 24; shift += 8) {
				uint32_t base = (old >> shift) & 255;
				uint32_t value = (composite[i] >> shift) & 255;
				uint32_t expected = base + (255 - base) * coverage / 255;

				/* Allow either rounding convention at an antialiased edge. */
				CHECK(value + 1 >= expected && value <= expected + 1);
			}
		}
		select_font((enum anx_font_family)f, false);
		anx_font_blit_char(composite, ANX_FONT_WIDTH, ANX_FONT_HEIGHT,
				   0, 0, 'A', 0xffffff, 0);
		for (i = 0; i < CELL; i++) CHECK(composite[i] == 0 || composite[i] == 0xffffff);
		CHECK(anx_memcmp(reference, composite, sizeof(reference)) != 0);
	}
	return 0;
}

static int test_clipped_stride(void)
{
	uint32_t x, y, i;

	select_font(ANX_FONT_ATKINSON, true);
	fill(guarded.before, 8, SENTINEL);
	fill(guarded.after, 8, SENTINEL);
	fill(guarded.pixels, STRIDE * HEIGHT, SENTINEL);
	anx_font_blit_char(reference, ANX_FONT_WIDTH, ANX_FONT_HEIGHT,
			   0, 0, 'W', 0xffffff, 0x204060);
	anx_font_blit_char_stride(guarded.pixels, STRIDE, 23, 31,
				 18, 20, 'W', 0xffffff, 0x204060);
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < STRIDE; x++) {
			uint32_t expected = x >= 18 && x < 23 && y >= 20 && y < 31
				? reference[(y - 20) * ANX_FONT_WIDTH + x - 18] : SENTINEL;

			CHECK(guarded.pixels[y * STRIDE + x] == expected);
		}
	}
	/* Far-out origins must be rejected before unsigned coordinate arithmetic. */
	anx_memcpy(frame, guarded.pixels, sizeof(frame));
	anx_font_blit_char_stride(guarded.pixels, STRIDE, 23, 31,
				 0xffffffffu, 0, 'A', 0, 0);
	anx_font_blit_char_stride(guarded.pixels, STRIDE, 23, 31,
				 0, 0xffffffffu, 'A', 0, 0);
	anx_font_blit_char(guarded.pixels, STRIDE, HEIGHT,
			   0xffffffffu, 0xffffffffu, 'A', 0, 0);
	anx_font_blit_str(guarded.pixels, STRIDE, HEIGHT,
			  0xffffffffu, 0, "AB", 0, 0);
	CHECK(!anx_memcmp(frame, guarded.pixels, sizeof(frame)));
	for (i = 0; i < 8; i++) {
		CHECK(guarded.before[i] == SENTINEL);
		CHECK(guarded.after[i] == SENTINEL);
	}
	return 0;
}

static int test_framebuffer_and_scaling(void)
{
	struct anx_fb_info fb = {
		.addr = (uint64_t)(uintptr_t)frame,
		.width = 35, .height = HEIGHT, .pitch = STRIDE * 4,
		.bpp = 32, .available = true,
	};
	uint32_t f, aa, x, y;

	CHECK(anx_fb_init(&fb) == ANX_OK);
	for (f = 0; f < 3; f++) {
		for (aa = 0; aa < 2; aa++) {
			select_font((enum anx_font_family)f, aa != 0);
			fill(frame, STRIDE * HEIGHT, SENTINEL);
			fill(guarded.pixels, STRIDE * HEIGHT, SENTINEL);
			anx_font_draw_char(4, 6, '@', 0xf0e0d0, 0x102030);
			anx_font_blit_char_stride(guarded.pixels, STRIDE, fb.width, HEIGHT,
						 4, 6, '@', 0xf0e0d0, 0x102030);
			CHECK(!anx_memcmp(frame, guarded.pixels, sizeof(frame)));
			anx_font_blit_char(reference, ANX_FONT_WIDTH, ANX_FONT_HEIGHT,
					   0, 0, 'A', 0xf0e0d0, 0x102030);
			fill(frame, STRIDE * HEIGHT, SENTINEL);
			anx_font_draw_char_scaled(3, 5, 'A', 0xf0e0d0, 0x102030, 2);
			for (y = 0; y < HEIGHT; y++) {
				for (x = 0; x < STRIDE; x++) {
					uint32_t expected = SENTINEL;

					if (x >= 3 && x < 3 + ANX_FONT_WIDTH * 2 &&
					    y >= 5 && y < 5 + ANX_FONT_HEIGHT * 2)
						expected = reference[(y - 5) / 2 * ANX_FONT_WIDTH + (x - 3) / 2];
					CHECK(frame[y * STRIDE + x] == expected);
				}
			}
		}
	}
	/* A scaled glyph may intersect the right and bottom edges simultaneously. */
	fill(frame, STRIDE * HEIGHT, SENTINEL);
	anx_font_draw_char_scaled(20, 30, 'A', 0xf0e0d0, 0x102030, 2);
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < STRIDE; x++) {
			uint32_t expected = x >= 20 && x < fb.width && y >= 30
				? reference[(y - 30) / 2 * ANX_FONT_WIDTH + (x - 20) / 2] : SENTINEL;

			CHECK(frame[y * STRIDE + x] == expected);
		}
	}
	anx_memcpy(guarded.pixels, frame, sizeof(frame));
	anx_font_draw_char(0xffffffffu, 0, 'A', 0, 0);
	anx_font_draw_char(0, 0xffffffffu, 'A', 0, 0);
	anx_font_draw_char_scaled(0xffffffffu, 0, 'A', 0, 0, 2);
	anx_font_draw_char_scaled(0, 0xffffffffu, 'A', 0, 0, 2);
	anx_font_draw_char_scaled(0, 0, 'A', 0, 0, 0);
	anx_font_draw_char_scaled(0, 0, 'A', 0, 0, 5);
	CHECK(!anx_memcmp(frame, guarded.pixels, sizeof(frame)));
	return 0;
}

static uint16_t fallback_glyph[ANX_FONT_HEIGHT];

static const uint16_t *fallback(uint32_t codepoint)
{
	return codepoint == 0x2603 ? fallback_glyph : NULL;
}

static int test_registered_fallback(void)
{
	uint32_t family, x, y;

	anx_font_init();
	for (y = 0; y < ANX_FONT_HEIGHT; y++) fallback_glyph[y] = y & 1 ? 0xaaa : 0x555;
	CHECK(anx_font_fallback_register(0x2603, 0x2603, fallback) == ANX_OK);
	for (family = 0; family < ANX_FONT_FAMILY_COUNT; family++) {
		select_font((enum anx_font_family)family, true);
		CHECK(anx_font_has_glyph(0x2603));
		CHECK(anx_font_glyph_cp(0x2603) == fallback_glyph);
		fill(frame, STRIDE * HEIGHT, SENTINEL);
		anx_font_draw_codepoint(2, 3, 0x2603, 0xffffff, 0x204060);
		for (y = 0; y < ANX_FONT_HEIGHT; y++) {
			for (x = 0; x < ANX_FONT_WIDTH; x++) {
				uint32_t expected = (fallback_glyph[y] & (0x800u >> x)) ? 0xffffff : 0x204060;

				CHECK(frame[(y + 3) * STRIDE + x + 2] == expected);
			}
		}
		anx_memcpy(guarded.pixels, frame, sizeof(frame));
		fill(frame, STRIDE * HEIGHT, SENTINEL);
		CHECK(anx_font_draw_str(2, 3, "\xe2\x98\x83", 0xffffff, 0x204060) == 1);
		CHECK(!anx_memcmp(frame, guarded.pixels, sizeof(frame)));
	}
	return 0;
}

static int test_scaled_strings(void)
{
	uint32_t family, aa, x, y;
	const uint32_t cell_w = 18, cell_h = 36;

	for (family = 0; family < ANX_FONT_FAMILY_COUNT; family++) {
		for (aa = 0; aa < 2; aa++) {
			uint32_t intermediate = 0, ink = 0;

			select_font((enum anx_font_family)family, aa != 0);
			fill(frame, STRIDE * HEIGHT, SENTINEL);
			fill(guarded.pixels, STRIDE * HEIGHT, SENTINEL);
			anx_font_blit_str(frame, STRIDE, HEIGHT, 2, 3, "Ag", 0xf0e0d0, 0x102030);
			anx_font_blit_str_scaled(guarded.pixels, STRIDE, HEIGHT, 2, 3,
						 "Ag", 0xf0e0d0, 0x102030, 100);
			CHECK(!anx_memcmp(frame, guarded.pixels, sizeof(frame)));

			/* 150% produces two adjoining 18x36 cells, with no spill. */
			fill(frame, STRIDE * HEIGHT, SENTINEL);
			anx_font_blit_str_scaled(frame, STRIDE, HEIGHT, 2, 3,
						 "AA", 0xffffff, 0, 150);
			for (y = 0; y < HEIGHT; y++) {
				for (x = 0; x < STRIDE; x++) {
					uint32_t p = frame[y * STRIDE + x];

					if (x < 2 || x >= 2 + 2 * cell_w || y < 3 || y >= 3 + cell_h) {
						CHECK(p == SENTINEL);
						continue;
					}
					CHECK(p == (p & 255) * 0x010101);
					if (p) ink++;
					if (p && p != 0xffffff) intermediate++;
					if (x < 2 + cell_w)
						CHECK(p == frame[y * STRIDE + x + cell_w]);
				}
			}
			CHECK(ink);
			if (aa && family != ANX_FONT_SPLEEN) CHECK(intermediate);
			else CHECK(!intermediate);

			/* A scaled blank cell erases its full background without shifting its neighbor. */
			fill(guarded.pixels, STRIDE * HEIGHT, SENTINEL);
			anx_font_blit_str_scaled(guarded.pixels, STRIDE, HEIGHT, 2, 3,
						 "A ", 0xffffff, 0, 150);
			for (y = 0; y < HEIGHT; y++) {
				for (x = 0; x < STRIDE; x++) {
					uint32_t expected = x >= 2 + cell_w && x < 2 + 2 * cell_w &&
						y >= 3 && y < 3 + cell_h ? 0 : frame[y * STRIDE + x];

					CHECK(guarded.pixels[y * STRIDE + x] == expected);
					guarded.pixels[y * STRIDE + x] = x & 1 ? 0x204060 : 0x804020;
				}
			}

			/* The same enlarged coverage must blend against each destination pixel. */
			anx_font_blit_str_scaled(guarded.pixels, STRIDE, HEIGHT, 2, 3,
						 "AA", 0xffffff, ANX_FONT_TRANSPARENT, 150);
			for (y = 0; y < HEIGHT; y++) {
				for (x = 0; x < STRIDE; x++) {
					uint32_t old = x & 1 ? 0x204060 : 0x804020;
					uint32_t actual = guarded.pixels[y * STRIDE + x];
					uint32_t coverage = frame[y * STRIDE + x];
					uint32_t shift;

					if (coverage == SENTINEL || !coverage) {
						CHECK(actual == old);
						continue;
					}
					coverage &= 255;
					for (shift = 0; shift < 24; shift += 8) {
						uint32_t base = (old >> shift) & 255;
						uint32_t value = (actual >> shift) & 255;
						uint32_t expected = base + (255 - base) * coverage / 255;

						CHECK(value + 1 >= expected && value <= expected + 1);
					}
				}
			}
		}
	}
	return 0;
}

static int test_scaled_string_clipping(void)
{
	uint32_t family, x, y, i;

	fill(guarded.before, 8, SENTINEL);
	fill(guarded.after, 8, SENTINEL);
	for (family = 0; family < ANX_FONT_FAMILY_COUNT; family++) {
		select_font((enum anx_font_family)family, true);
		fill(frame, STRIDE * HEIGHT, SENTINEL);
		fill(guarded.pixels, STRIDE * HEIGHT, SENTINEL);
		anx_font_blit_str_scaled(frame, STRIDE, HEIGHT, 0, 0, "AA", 0xffffff, 0, 150);
		/* Crop a partly visible cell at both canvas edges. */
		anx_font_blit_str_scaled(guarded.pixels, STRIDE, HEIGHT, 30, 50,
					 "AA", 0xffffff, 0, 150);
		for (y = 0; y < HEIGHT; y++) {
			for (x = 0; x < STRIDE; x++) {
				uint32_t expected = x >= 30 && y >= 50
					? frame[(y - 50) * STRIDE + x - 30] : SENTINEL;

				CHECK(guarded.pixels[y * STRIDE + x] == expected);
			}
		}
	}
	fill(guarded.pixels, STRIDE * HEIGHT, SENTINEL);
	anx_font_blit_str_scaled(guarded.pixels, STRIDE, HEIGHT, 0xffffffffu, 0,
				 "AA", 0, 0, 150);
	anx_font_blit_str_scaled(guarded.pixels, STRIDE, HEIGHT, 0, 0xffffffffu,
				 "AA", 0, 0, 150);
	anx_font_blit_str_scaled(guarded.pixels, STRIDE, HEIGHT, 0, 0, "AA", 0, 0, 0);
	anx_font_blit_str_scaled(guarded.pixels, STRIDE, HEIGHT, 0, 0, "AA", 0, 0, 401);
	anx_font_blit_str_scaled(guarded.pixels, STRIDE, HEIGHT, 0, 0, "AA", 0, 0, 0xffffffffu);
	for (i = 0; i < STRIDE * HEIGHT; i++) CHECK(guarded.pixels[i] == SENTINEL);
	for (i = 0; i < 8; i++) {
		CHECK(guarded.before[i] == SENTINEL);
		CHECK(guarded.after[i] == SENTINEL);
	}
	return 0;
}

int test_font_families(void)
{
	struct anx_theme saved_theme = *anx_theme_get();
	struct anx_fb_info saved_fb = *anx_fb_get_info();
	int rc;

	rc = test_distinct_families();
	if (!rc) rc = test_coverage_and_compositing();
	if (!rc) rc = test_clipped_stride();
	if (!rc) rc = test_framebuffer_and_scaling();
	if (!rc) rc = test_registered_fallback();
	if (!rc) rc = test_scaled_strings();
	if (!rc) rc = test_scaled_string_clipping();
	anx_font_init();
	anx_theme_restore(&saved_theme);
	anx_fb_init(&saved_fb);
	return rc;
}
