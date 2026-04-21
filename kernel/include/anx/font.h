/*
 * anx/font.h — Bitmap font for framebuffer console.
 *
 * Spleen 12x24 — BSD-2-Clause, Frederic Cambus.
 * Covers printable ASCII (32-126). Each glyph is 48 bytes:
 * 24 rows × 2 bytes, pixels left-justified (MSB of byte[0] = leftmost).
 */

#ifndef ANX_FONT_H
#define ANX_FONT_H

#include <anx/types.h>

#define ANX_FONT_WIDTH		12
#define ANX_FONT_HEIGHT		24
#define ANX_FONT_STRIDE		2		/* bytes per row */
#define ANX_FONT_GLYPH_BYTES	(ANX_FONT_HEIGHT * ANX_FONT_STRIDE)

/* Get pointer to 16-byte glyph bitmap for character c */
const uint8_t *anx_font_glyph(char c);

/* Blit a character to the framebuffer at pixel position (x,y) */
void anx_font_draw_char(uint32_t x, uint32_t y,
			char c, uint32_t fg, uint32_t bg);

#endif /* ANX_FONT_H */
