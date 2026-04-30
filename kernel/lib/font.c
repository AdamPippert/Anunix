/*
 * font.c — Spleen 12×24 bitmap font.
 *
 * Spleen 12x24 by Frederic Cambus, BSD-2-Clause.
 * Sun-workstation aesthetic. Covers printable ASCII (0x20–0x7E).
 * Each glyph is 24 uint16_t rows; bit 11 (0x800) is the leftmost pixel.
 */

#include <anx/types.h>
#include <anx/font.h>
#include <anx/utf8.h>
#include <anx/fb.h>
#include <anx/spleen_12x24.h>

/* Fallback glyph for characters outside 0x20-0x7E: filled block */
static const uint16_t glyph_fallback[ANX_FONT_HEIGHT] = {
	0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF,
	0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF,
	0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF, 0xFFF,
};

const uint16_t *anx_font_glyph(char c)
{
	unsigned char uc = (unsigned char)c;

	if (uc < ANX_FONT12_FIRST || uc > ANX_FONT12_LAST)
		return glyph_fallback;

	return anx_font12[uc - ANX_FONT12_FIRST];
}

void anx_font_draw_char(uint32_t x, uint32_t y,
			char c, uint32_t fg, uint32_t bg)
{
	const struct anx_fb_info *info = anx_fb_get_info();
	const uint16_t *glyph = anx_font_glyph(c);
	uint32_t row;

	if (!info->available)
		return;

	for (row = 0; row < ANX_FONT_HEIGHT; row++) {
		uint16_t bits = glyph[row];
		uint32_t *dst;
		uint32_t col;

		if (y + row >= info->height)
			break;

		dst = anx_fb_row_ptr(y + row) + x;

		for (col = 0; col < ANX_FONT_WIDTH; col++) {
			if (x + col >= info->width)
				break;
			dst[col] = (bits & (0x800u >> col)) ? fg : bg;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Unicode / fallback extension                                        */
/* ------------------------------------------------------------------ */

static struct anx_font_fallback font_fallbacks[ANX_FONT_FALLBACK_MAX];
static uint32_t                 font_fallback_count;

void anx_font_init(void)
{
	uint32_t i;

	for (i = 0; i < ANX_FONT_FALLBACK_MAX; i++) {
		font_fallbacks[i].cp_first   = 0;
		font_fallbacks[i].cp_last    = 0;
		font_fallbacks[i].get_glyph  = NULL;
	}
	font_fallback_count = 0;
}

int anx_font_fallback_register(uint32_t cp_first, uint32_t cp_last,
                                 const uint16_t *(*get_glyph)(uint32_t cp))
{
	if (!get_glyph || cp_first > cp_last)
		return ANX_EINVAL;
	if (font_fallback_count >= ANX_FONT_FALLBACK_MAX)
		return ANX_EFULL;

	font_fallbacks[font_fallback_count].cp_first  = cp_first;
	font_fallbacks[font_fallback_count].cp_last   = cp_last;
	font_fallbacks[font_fallback_count].get_glyph = get_glyph;
	font_fallback_count++;
	return ANX_OK;
}

const uint16_t *anx_font_glyph_cp(uint32_t codepoint)
{
	uint32_t i;

	/* Primary font covers printable ASCII. */
	if (codepoint >= ANX_FONT12_FIRST && codepoint <= ANX_FONT12_LAST)
		return anx_font12[codepoint - ANX_FONT12_FIRST];

	/* Check registered fallback fonts in registration order. */
	for (i = 0; i < font_fallback_count; i++) {
		if (codepoint >= font_fallbacks[i].cp_first &&
		    codepoint <= font_fallbacks[i].cp_last)
			return font_fallbacks[i].get_glyph(codepoint);
	}

	return glyph_fallback;
}

bool anx_font_has_glyph(uint32_t codepoint)
{
	uint32_t i;

	if (codepoint >= ANX_FONT12_FIRST && codepoint <= ANX_FONT12_LAST)
		return true;

	for (i = 0; i < font_fallback_count; i++) {
		if (codepoint >= font_fallbacks[i].cp_first &&
		    codepoint <= font_fallbacks[i].cp_last)
			return true;
	}

	return false;
}

void anx_font_draw_codepoint(uint32_t x, uint32_t y,
                              uint32_t codepoint, uint32_t fg, uint32_t bg)
{
	const uint16_t *glyph;
	uint32_t row;

	if (!anx_fb_available())
		return;

	glyph = anx_font_glyph_cp(codepoint);
	for (row = 0; row < ANX_FONT_HEIGHT; row++) {
		uint16_t bits = glyph[row];
		uint32_t *dst = anx_fb_row_ptr(y + row) + x;
		uint32_t col;

		for (col = 0; col < ANX_FONT_WIDTH; col++)
			dst[col] = (bits & (0x800u >> col)) ? fg : bg;
	}
}

void anx_font_blit_char(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
                         uint32_t x, uint32_t y, char c,
                         uint32_t fg, uint32_t bg)
{
	const uint16_t *glyph = anx_font_glyph(c);
	uint32_t row, col;

	for (row = 0; row < ANX_FONT_HEIGHT && (y + row) < buf_h; row++) {
		uint16_t bits = glyph[row];

		for (col = 0; col < ANX_FONT_WIDTH && (x + col) < buf_w; col++) {
			if (bits & (0x800u >> col))
				buf[(y + row) * buf_w + (x + col)] = fg;
			else if (bg != ANX_FONT_TRANSPARENT)
				buf[(y + row) * buf_w + (x + col)] = bg;
		}
	}
}

void anx_font_blit_str(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
                        uint32_t x, uint32_t y, const char *s,
                        uint32_t fg, uint32_t bg)
{
	for (; *s && x + ANX_FONT_WIDTH <= buf_w; s++, x += ANX_FONT_WIDTH)
		anx_font_blit_char(buf, buf_w, buf_h, x, y, *s, fg, bg);
}

int anx_font_draw_str(uint32_t x, uint32_t y,
                       const char *utf8_str, uint32_t fg, uint32_t bg)
{
	const uint8_t *p;
	uint32_t remaining, cp, consumed, glyphs, cx;
	int rc;

	if (!utf8_str)
		return 0;

	p         = (const uint8_t *)utf8_str;
	remaining = 0;
	while (p[remaining])
		remaining++;

	glyphs = 0;
	cx     = x;
	while (remaining > 0) {
		rc = anx_utf8_decode(p, remaining, &cp, &consumed);
		if (rc != ANX_OK) {
			/* Skip one invalid byte and continue. */
			p++;
			remaining--;
			continue;
		}
		anx_font_draw_codepoint(cx, y, cp, fg, bg);
		cx += ANX_FONT_WIDTH;
		p  += consumed;
		remaining -= consumed;
		glyphs++;
	}
	return (int)glyphs;
}
