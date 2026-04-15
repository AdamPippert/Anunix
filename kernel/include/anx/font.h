/*
 * anx/font.h — Bitmap font for framebuffer console.
 *
 * 8x16 VGA-style font covering printable ASCII (32-126).
 * Each glyph is 16 bytes: one byte per row, MSB = leftmost pixel.
 */

#ifndef ANX_FONT_H
#define ANX_FONT_H

#include <anx/types.h>

#define ANX_FONT_WIDTH	 8
#define ANX_FONT_HEIGHT	16

/* Get pointer to 16-byte glyph bitmap for character c */
const uint8_t *anx_font_glyph(char c);

/* Blit a character to the framebuffer at pixel position (x,y) */
void anx_font_draw_char(uint32_t x, uint32_t y,
			char c, uint32_t fg, uint32_t bg);

#endif /* ANX_FONT_H */
