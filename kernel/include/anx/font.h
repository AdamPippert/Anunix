/* Theme-selectable fonts share a 12x24 cell. ASCII has coverage masks;
 * the bitmap API and registered Unicode fallbacks retain their 12-bit rows. */

#ifndef ANX_FONT_H
#define ANX_FONT_H

#include <anx/types.h>

#define ANX_FONT_WIDTH		12
#define ANX_FONT_HEIGHT		24

enum anx_font_family {
	ANX_FONT_ATKINSON = 0,
	ANX_FONT_CASCADIA,
	ANX_FONT_JETBRAINS,
	ANX_FONT_SPLEEN,
	ANX_FONT_FAMILY_COUNT,
};

/* Canonical family name, or NULL for an invalid family. */
const char *anx_font_family_name(enum anx_font_family family);
/* Parse a canonical name without changing output on failure. */
int anx_font_family_parse(const char *name, enum anx_font_family *family);

/* Return pointer to 24-element uint16_t glyph bitmap for character c.
 * Characters outside 0x20–0x7E return a filled-block fallback. */
const uint16_t *anx_font_glyph(char c);

/* Blit a character to the framebuffer at pixel position (x, y). */
void anx_font_draw_char(uint32_t x, uint32_t y,
			char c, uint32_t fg, uint32_t bg);

/* ------------------------------------------------------------------ */
/* Unicode / UTF-8 extension (P1-004)                                  */
/* ------------------------------------------------------------------ */

#define ANX_FONT_FALLBACK_MAX  4u

/* Fallback font entry: covers codepoints [cp_first, cp_last]. */
struct anx_font_fallback {
	uint32_t cp_first;
	uint32_t cp_last;
	const uint16_t *(*get_glyph)(uint32_t codepoint);
};

/* Reset fallback table (call before registering in tests). */
void anx_font_init(void);

/* Register a fallback font for [cp_first, cp_last]. */
int anx_font_fallback_register(uint32_t cp_first, uint32_t cp_last,
                                 const uint16_t *(*get_glyph)(uint32_t cp));

/* Return glyph for Unicode codepoint; checks primary then fallbacks,
 * returns filled-block if unmapped. */
const uint16_t *anx_font_glyph_cp(uint32_t codepoint);

/* True if codepoint is covered by the primary font or a registered fallback. */
bool anx_font_has_glyph(uint32_t codepoint);

/* Blit a Unicode codepoint to the framebuffer. */
void anx_font_draw_codepoint(uint32_t x, uint32_t y,
                              uint32_t codepoint, uint32_t fg, uint32_t bg);

/* Render a NUL-terminated UTF-8 string.
 * Returns the number of glyphs rendered (invalid UTF-8 bytes are skipped). */
int anx_font_draw_str(uint32_t x, uint32_t y,
                       const char *utf8_str, uint32_t fg, uint32_t bg);

/* ------------------------------------------------------------------ */
/* Off-screen buffer rendering                                          */
/* ------------------------------------------------------------------ */

/* Pass as bg to skip drawing background pixels (text over pre-filled bg). */
#define ANX_FONT_TRANSPARENT  0xFF000000u

/* Blit a single ASCII character into a caller-supplied pixel buffer.
 * Background pixels are skipped when bg == ANX_FONT_TRANSPARENT. */
void anx_font_blit_char(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
                         uint32_t x, uint32_t y, char c,
                         uint32_t fg, uint32_t bg);

/* Blit with a row stride independent of the clipped width. */
void anx_font_blit_char_stride(uint32_t *buf, uint32_t stride,
                              uint32_t clip_w, uint32_t clip_h,
                              uint32_t x, uint32_t y, char c,
                              uint32_t fg, uint32_t bg);

/* Draw a nearest-neighbor scaled glyph with the theme's coverage rendering. */
void anx_font_draw_char_scaled(uint32_t x, uint32_t y, char c,
                              uint32_t fg, uint32_t bg, uint32_t scale);

/* Render a clipped string at 100..400 percent of the 12x24 metrics. */
void anx_font_blit_str_scaled(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
                             uint32_t x, uint32_t y, const char *s,
                             uint32_t fg, uint32_t bg, uint32_t scale_percent);

/* Blit a NUL-terminated ASCII string into a caller-supplied pixel buffer.
 * Background pixels are skipped when bg == ANX_FONT_TRANSPARENT. */
void anx_font_blit_str(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
                        uint32_t x, uint32_t y, const char *s,
                        uint32_t fg, uint32_t bg);

#endif /* ANX_FONT_H */
