/*
 * wm_paint.c — Shapes and gradients inside a pixel buffer.
 *
 * The panels (menu bar, task bar) draw into their own canvas rather than
 * straight to the screen, so they cannot use the framebuffer's shape
 * routines. These are the same shapes over a caller-supplied buffer, so
 * a tab in the task bar has the corners, gradient and bevel of a window.
 */

#include <anx/types.h>
#include <anx/wm.h>
#include <anx/fb.h>

static uint32_t isqrt32(uint32_t v)
{
	uint32_t rem = 0, root = 0, i;

	for (i = 0; i < 16; i++) {
		root <<= 1;
		rem = (rem << 2) | (v >> 30);
		v <<= 2;
		if (root < rem) {
			rem -= root | 1;
			root += 2;
		}
	}
	return root >> 1;
}

static uint32_t corner_inset(uint8_t style, uint32_t r, uint32_t depth)
{
	uint32_t dy;

	if (style == ANX_CORNER_SQUARE || r == 0 || depth >= r)
		return 0;
	dy = r - depth;
	if (style == ANX_CORNER_MITRE)
		return dy;
	return r - isqrt32(r * r - dy * dy);
}

static uint32_t mix(uint32_t a, uint32_t b, uint32_t t)
{
	uint32_t r = (((a >> 16) & 0xFF) * (255 - t) + ((b >> 16) & 0xFF) * t) / 255;
	uint32_t g = (((a >> 8) & 0xFF) * (255 - t) + ((b >> 8) & 0xFF) * t) / 255;
	uint32_t bl = ((a & 0xFF) * (255 - t) + (b & 0xFF) * t) / 255;

	return (r << 16) | (g << 8) | bl;
}

void anx_wm_buf_gradient(uint32_t *buf, uint32_t bw, uint32_t bh,
			 uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			 const struct anx_shape *shape,
			 uint32_t from, uint32_t to)
{
	uint32_t row, r = shape ? shape->radius : 0;

	if (!buf || !w || !h)
		return;
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;

	for (row = 0; row < h; row++) {
		uint32_t left = 0, right = 0, col, depth;
		uint32_t t = h > 1 ? row * 255 / (h - 1) : 0;
		uint32_t color = mix(from, to, t);

		if (y + row >= bh)
			break;
		if (shape && r) {
			if (row < r) {
				depth = row;
				left = corner_inset(shape->corner[0], r, depth);
				right = corner_inset(shape->corner[1], r, depth);
			} else if (row + r >= h) {
				depth = h - 1 - row;
				left = corner_inset(shape->corner[3], r, depth);
				right = corner_inset(shape->corner[2], r, depth);
			}
		}
		if (left + right >= w)
			continue;
		for (col = left; col < w - right; col++) {
			if (x + col >= bw)
				break;
			buf[(y + row) * bw + x + col] = color;
		}
	}
}

void anx_wm_buf_blend(uint32_t *buf, uint32_t bw, uint32_t bh,
		      uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		      uint32_t color, uint8_t alpha)
{
	uint32_t row, col;

	if (!buf || !alpha || !w || !h)
		return;
	for (row = y; row < y + h && row < bh; row++) {
		for (col = x; col < x + w && col < bw; col++) {
			uint32_t d = buf[row * bw + col];
			uint32_t inv = 255 - alpha;
			uint32_t r = ((((color >> 16) & 0xFF) * alpha) +
				      (((d >> 16) & 0xFF) * inv)) / 255;
			uint32_t g = ((((color >> 8) & 0xFF) * alpha) +
				      (((d >> 8) & 0xFF) * inv)) / 255;
			uint32_t b = (((color & 0xFF) * alpha) +
				      ((d & 0xFF) * inv)) / 255;

			buf[row * bw + col] = (r << 16) | (g << 8) | b;
		}
	}
}
