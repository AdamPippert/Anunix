/*
 * test_text_shaping.c — Deterministic tests for P1-004: text/font
 * shaping correctness baseline.
 *
 * Tests:
 * - P1-004-U01: UTF-8 decode validity and rejection vectors
 * - P1-004-I02: multilingual text renders without crash, stable glyph count
 * - P1-004-I03: fallback font path triggers deterministically for missing glyphs
 */

#include <anx/types.h>
#include <anx/utf8.h>
#include <anx/font.h>
#include <anx/fb.h>

#define ASSERT(cond, code)    do { if (!(cond)) return (code); } while (0)
#define ASSERT_EQ(a, b, code) do { if ((a) != (b)) return (code); } while (0)

/* 256 × 64 mock framebuffer — enough for 2 rows of 12×24 glyphs. */
#define TEST_FB_W  256
#define TEST_FB_H   64

static uint8_t test_fb_mem[TEST_FB_H * TEST_FB_W * 4];

static int setup_fb(void)
{
	struct anx_fb_info info;

	info.addr      = (uint64_t)(uintptr_t)test_fb_mem;
	info.width     = TEST_FB_W;
	info.height    = TEST_FB_H;
	info.pitch     = TEST_FB_W * 4;
	info.bpp       = 32;
	info.available = true;
	return anx_fb_init(&info);
}

/* ------------------------------------------------------------------ */
/* P1-004-U01: UTF-8 decode validity and rejection                     */
/* ------------------------------------------------------------------ */

static int test_utf8_decode_vectors(void)
{
	static const uint8_t e_acute[]  = {0xC3, 0xA9};           /* U+00E9 é */
	static const uint8_t zhong[]    = {0xE4, 0xB8, 0xAD};     /* U+4E2D 中 */
	static const uint8_t emoji[]    = {0xF0, 0x9F, 0x98, 0x80}; /* U+1F600 😀 */
	static const uint8_t cont[]     = {0x80};                  /* bare continuation */
	static const uint8_t overlong[] = {0xC0, 0x80};            /* overlong U+0000 */
	static const uint8_t surrogate[]= {0xED, 0xA0, 0x80};     /* U+D800 */
	static const uint8_t bad_cont[] = {0xC3, 0x30};            /* bad continuation */
	static const uint8_t never[]    = {0xFE};                  /* 0xFE never valid */

	uint32_t cp, consumed;
	int rc;

	/* Valid: single-byte ASCII */
	rc = anx_utf8_decode((uint8_t *)"A", 1, &cp, &consumed);
	ASSERT_EQ(rc,       ANX_OK, -100);
	ASSERT_EQ(cp,       0x41u,  -101);
	ASSERT_EQ(consumed, 1u,     -102);

	/* Valid: NUL byte (U+0000) */
	static const uint8_t nul[] = {0x00};
	rc = anx_utf8_decode(nul, 1, &cp, &consumed);
	ASSERT_EQ(rc,       ANX_OK, -103);
	ASSERT_EQ(cp,       0u,     -104);
	ASSERT_EQ(consumed, 1u,     -105);

	/* Valid: 2-byte é (U+00E9) */
	rc = anx_utf8_decode(e_acute, 2, &cp, &consumed);
	ASSERT_EQ(rc,       ANX_OK,  -106);
	ASSERT_EQ(cp,       0xE9u,   -107);
	ASSERT_EQ(consumed, 2u,      -108);

	/* Valid: 3-byte 中 (U+4E2D) */
	rc = anx_utf8_decode(zhong, 3, &cp, &consumed);
	ASSERT_EQ(rc,       ANX_OK,   -109);
	ASSERT_EQ(cp,       0x4E2Du,  -110);
	ASSERT_EQ(consumed, 3u,       -111);

	/* Valid: 4-byte 😀 (U+1F600) */
	rc = anx_utf8_decode(emoji, 4, &cp, &consumed);
	ASSERT_EQ(rc,       ANX_OK,   -112);
	ASSERT_EQ(cp,       0x1F600u, -113);
	ASSERT_EQ(consumed, 4u,       -114);

	/* Invalid: bare continuation byte */
	rc = anx_utf8_decode(cont, 1, &cp, &consumed);
	ASSERT_EQ(rc, ANX_EINVAL, -115);

	/* Invalid: overlong 2-byte U+0000 */
	rc = anx_utf8_decode(overlong, 2, &cp, &consumed);
	ASSERT_EQ(rc, ANX_EINVAL, -116);

	/* Invalid: surrogate U+D800 */
	rc = anx_utf8_decode(surrogate, 3, &cp, &consumed);
	ASSERT_EQ(rc, ANX_EINVAL, -117);

	/* Invalid: truncated 2-byte (only 1 byte supplied) */
	rc = anx_utf8_decode(e_acute, 1, &cp, &consumed);
	ASSERT_EQ(rc, ANX_EINVAL, -118);

	/* Invalid: bad continuation byte in 2-byte sequence */
	rc = anx_utf8_decode(bad_cont, 2, &cp, &consumed);
	ASSERT_EQ(rc, ANX_EINVAL, -119);

	/* Invalid: 0xFE is never valid */
	rc = anx_utf8_decode(never, 1, &cp, &consumed);
	ASSERT_EQ(rc, ANX_EINVAL, -120);

	/* Invalid: empty input */
	rc = anx_utf8_decode((uint8_t *)"", 0, &cp, &consumed);
	ASSERT_EQ(rc, ANX_EINVAL, -121);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-004-I02: multilingual text renders without crash, stable count  */
/* ------------------------------------------------------------------ */

static int test_multilingual_render(void)
{
	int count_a, count_b;
	int rc;

	/*
	 * "Hello" (5 ASCII) + é (U+00E9, 2 bytes) + 中 (U+4E2D, 3 bytes)
	 * = 7 codepoints, 10 bytes.
	 */
	static const char mixed[] = "Hello\xC3\xA9\xE4\xB8\xAD";

	rc = setup_fb();
	ASSERT_EQ(rc, ANX_OK, -200);

	anx_font_init();

	count_a = anx_font_draw_str(0, 0, mixed, 0xFFFFFF, 0x000000);
	ASSERT_EQ(count_a, 7, -201);

	/* Second call must return identical count (stable). */
	count_b = anx_font_draw_str(0, ANX_FONT_HEIGHT, mixed, 0xFFFFFF, 0x000000);
	ASSERT_EQ(count_b, count_a, -202);

	/* Pure ASCII string */
	count_a = anx_font_draw_str(0, 0, "ABC", 0xFFFFFF, 0x000000);
	ASSERT_EQ(count_a, 3, -203);

	/* Empty string */
	count_a = anx_font_draw_str(0, 0, "", 0xFFFFFF, 0x000000);
	ASSERT_EQ(count_a, 0, -204);

	/* String containing a 4-byte emoji (U+1F600) — not in primary font,
	 * renders as filled-block but still counts as 1 glyph. */
	static const char emoji_str[] = "\xF0\x9F\x98\x80";
	count_a = anx_font_draw_str(0, 0, emoji_str, 0xFFFFFF, 0x000000);
	ASSERT_EQ(count_a, 1, -205);

	return 0;
}

/* ------------------------------------------------------------------ */
/* P1-004-I03: fallback font triggers deterministically                */
/* ------------------------------------------------------------------ */

/* Checkerboard pattern — first row 0x555, distinguishable from filled-block */
static const uint16_t test_fallback_glyph[ANX_FONT_HEIGHT] = {
	0x555, 0xAAA, 0x555, 0xAAA, 0x555, 0xAAA, 0x555, 0xAAA,
	0x555, 0xAAA, 0x555, 0xAAA, 0x555, 0xAAA, 0x555, 0xAAA,
	0x555, 0xAAA, 0x555, 0xAAA, 0x555, 0xAAA, 0x555, 0xAAA,
};

static const uint16_t *fallback_get(uint32_t cp)
{
	(void)cp;
	return test_fallback_glyph;
}

static int test_fallback_font_path(void)
{
	const uint16_t *glyph;
	int rc;

	anx_font_init();  /* clear fallback table from previous subtests */

	/* U+0102 (Ă, Latin Extended-A) — not in primary font → filled-block */
	glyph = anx_font_glyph_cp(0x0102);
	ASSERT_EQ(glyph[0], 0xFFFu, -300);  /* filled-block row */

	/* ASCII 'A' (U+0041) has a native glyph. */
	ASSERT(anx_font_has_glyph(0x41), -301);

	/* U+0102 not covered before fallback is registered. */
	ASSERT(!anx_font_has_glyph(0x0102), -302);

	/* Register test fallback covering Latin Extended-A (U+0100–U+017F). */
	rc = anx_font_fallback_register(0x0100, 0x017F, fallback_get);
	ASSERT_EQ(rc, ANX_OK, -303);

	/* Now U+0102 routes to our test glyph, not the filled-block. */
	glyph = anx_font_glyph_cp(0x0102);
	ASSERT(glyph[0] != 0xFFFu, -304);
	ASSERT_EQ(glyph[0], test_fallback_glyph[0], -305);

	/* has_glyph now returns true for U+0102. */
	ASSERT(anx_font_has_glyph(0x0102), -306);

	/* U+0500 (Cyrillic Supplement — no coverage) still returns filled-block. */
	glyph = anx_font_glyph_cp(0x0500);
	ASSERT_EQ(glyph[0], 0xFFFu, -307);
	ASSERT(!anx_font_has_glyph(0x0500), -308);

	/* Registering beyond FALLBACK_MAX returns ANX_EFULL. */
	rc = anx_font_fallback_register(0x0400, 0x04FF, fallback_get);
	ASSERT_EQ(rc, ANX_OK, -309);
	rc = anx_font_fallback_register(0x0500, 0x05FF, fallback_get);
	ASSERT_EQ(rc, ANX_OK, -310);
	rc = anx_font_fallback_register(0x0600, 0x06FF, fallback_get);
	ASSERT_EQ(rc, ANX_OK, -311);
	rc = anx_font_fallback_register(0x0700, 0x07FF, fallback_get);
	ASSERT_EQ(rc, ANX_EFULL, -312);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Test suite entry point                                               */
/* ------------------------------------------------------------------ */

int test_text_shaping(void)
{
	int rc;

	rc = test_utf8_decode_vectors();
	if (rc != 0)
		return rc;

	rc = test_multilingual_render();
	if (rc != 0)
		return rc;

	rc = test_fallback_font_path();
	if (rc != 0)
		return rc;

	return 0;
}
