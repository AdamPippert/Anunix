/*
 * anx/font.h — Bitmap font for framebuffer console.
 *
 * ANX Schoolbook 12×24: Century Schoolbook-inspired design with
 * bracketed half-serifs and moderate stroke contrast.
 * Covers printable ASCII (0x20–0x7E, 95 glyphs).
 * Each glyph row is a uint16_t; bit 11 (0x800) = leftmost pixel.
 */

#ifndef ANX_FONT_H
#define ANX_FONT_H

#include <anx/types.h>

#define ANX_FONT_WIDTH	12
#define ANX_FONT_HEIGHT	24

/* Return pointer to 24-element uint16_t glyph bitmap for character c.
 * Characters outside 0x20–0x7E return a filled-block fallback. */
const uint16_t *anx_font_glyph(char c);

/* Blit a character to the framebuffer at pixel position (x, y). */
void anx_font_draw_char(uint32_t x, uint32_t y,
			char c, uint32_t fg, uint32_t bg);

#endif /* ANX_FONT_H */
