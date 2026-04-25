/*
 * paint.c — Rasterizer.
 *
 * Draws layout paint commands into an XRGB8888 off-screen buffer
 * using the kernel VGA bitmap font for text rendering.
 *
 * Font glyphs are 8×16 pixels. Larger sizes are achieved by integer
 * scaling (2× for font_size 3). Each column of a glyph is a bitmask
 * where bit 7 = leftmost pixel.
 */

#include "paint.h"
#include <anx/fb.h>
#include <anx/string.h>

/* ── VGA 8×16 bitmap font (ASCII 32-127) ───────────────────────────
 * We embed a 96-character subset of the classic VGA ROM font.
 * Each character is 16 bytes; each byte is a row with bit7=left.
 * ----------------------------------------------------------------- */
#include "font_data.h"   /* generated constant: FONT_GLYPH[96][16] */

static void draw_glyph(uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
		         uint32_t stride, int32_t x, int32_t y,
		         uint8_t ch, uint32_t color, uint32_t scale)
{
	if (ch < 32 || ch > 127) ch = '?';
	const uint8_t *glyph = FONT_GLYPH[ch - 32];
	uint32_t row, col, sy, sx;
	uint32_t stride32 = stride / 4;

	for (row = 0; row < 16; row++) {
		for (col = 0; col < 8; col++) {
			if (!(glyph[row] & (0x80u >> col))) continue;
			for (sy = 0; sy < scale; sy++) {
				int32_t py = y + (int32_t)(row * scale + sy);
				if (py < 0 || (uint32_t)py >= fb_h) continue;
				for (sx = 0; sx < scale; sx++) {
					int32_t px = x + (int32_t)(col * scale + sx);
					if (px < 0 || (uint32_t)px >= fb_w) continue;
					fb[(uint32_t)py * stride32 + (uint32_t)px] = color;
				}
			}
		}
	}
}

static void draw_text(uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
		        uint32_t stride, int32_t x, int32_t y,
		        const char *text, uint32_t color,
		        bool bold, uint8_t font_size)
{
	uint32_t scale = (font_size >= 3) ? 2 : 1;
	uint32_t glyph_w = 8 * scale;
	int32_t  cx = x;

	while (*text) {
		draw_glyph(fb, fb_w, fb_h, stride, cx, y,
			    (uint8_t)*text, color, scale);
		if (bold) {
			/* Embolden: draw again 1px right */
			draw_glyph(fb, fb_w, fb_h, stride, cx + 1, y,
				    (uint8_t)*text, color, scale);
		}
		cx += (int32_t)glyph_w;
		text++;
	}
}

static void fill_rect(uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
		        uint32_t stride, int32_t x, int32_t y,
		        uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t stride32 = stride / 4;
	uint32_t row, col;

	for (row = 0; row < h; row++) {
		int32_t py = y + (int32_t)row;
		if (py < 0 || (uint32_t)py >= fb_h) continue;
		for (col = 0; col < w; col++) {
			int32_t px = x + (int32_t)col;
			if (px < 0 || (uint32_t)px >= fb_w) continue;
			fb[(uint32_t)py * stride32 + (uint32_t)px] = color;
		}
	}
}

void paint_clear(uint32_t *fb, uint32_t w, uint32_t h,
		  uint32_t stride, uint32_t color)
{
	fill_rect(fb, w, h, stride, 0, 0, w, h, color);
}

void paint_execute(const struct layout_ctx *ctx,
		    uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
		    uint32_t stride, int32_t scroll_y)
{
	uint32_t i;

	for (i = 0; i < ctx->n_cmds; i++) {
		const struct paint_cmd *c = &ctx->cmds[i];
		int32_t sy = c->y - scroll_y;

		switch (c->type) {
		case PCMD_FILL_RECT:
			fill_rect(fb, fb_w, fb_h, stride,
				   c->x, sy, c->w, c->h, c->color);
			break;

		case PCMD_TEXT_RUN:
			draw_text(fb, fb_w, fb_h, stride,
				   c->x, sy, c->text, c->color,
				   c->bold, c->font_size_px);
			break;

		case PCMD_IMAGE: {
			if (!c->img_pixels || !c->img_src_w || !c->img_src_h ||
			    !c->w || !c->h)
				break;
			uint32_t stride32 = stride / 4;
			uint32_t dy, dx;
			for (dy = 0; dy < c->h; dy++) {
				int32_t py = sy + (int32_t)dy;
				if (py < 0 || (uint32_t)py >= fb_h) continue;
				uint32_t src_y = dy * c->img_src_h / c->h;
				for (dx = 0; dx < c->w; dx++) {
					int32_t px = c->x + (int32_t)dx;
					if (px < 0 || (uint32_t)px >= fb_w) continue;
					uint32_t src_x = dx * c->img_src_w / c->w;
					fb[(uint32_t)py * stride32 + (uint32_t)px] =
						c->img_pixels[src_y * c->img_src_w + src_x];
				}
			}
			break;
		}

		default:
			break;
		}
	}
}
