/*
 * fb.c — Framebuffer core operations.
 *
 * Provides pixel-level access to a linear XRGB8888 framebuffer.
 * The framebuffer address and geometry come from architecture
 * code during boot (multiboot on x86_64, ramfb on arm64).
 */

#include <anx/types.h>
#include <anx/fb.h>
#include <anx/string.h>

static struct anx_fb_info fb;

int anx_fb_init(const struct anx_fb_info *info)
{
	if (!info || !info->available || info->addr == 0)
		return ANX_EINVAL;
	if (info->bpp != 32)
		return ANX_EINVAL;

	fb = *info;
	return ANX_OK;
}

bool anx_fb_available(void)
{
	return fb.available;
}

const struct anx_fb_info *anx_fb_get_info(void)
{
	return &fb;
}

uint32_t *anx_fb_row_ptr(uint32_t y)
{
	uint8_t *base = (uint8_t *)(uintptr_t)fb.addr;

	return (uint32_t *)(base + y * fb.pitch);
}

void anx_fb_putpixel(uint32_t x, uint32_t y, uint32_t color)
{
	uint32_t *row;

	if (x >= fb.width || y >= fb.height)
		return;

	row = anx_fb_row_ptr(y);
	row[x] = color;
}

void anx_fb_fill_rect(uint32_t x, uint32_t y,
		       uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row_y, col_x;

	/* Clamp to screen bounds */
	if (x >= fb.width || y >= fb.height)
		return;
	if (x + w > fb.width)
		w = fb.width - x;
	if (y + h > fb.height)
		h = fb.height - y;

	for (row_y = y; row_y < y + h; row_y++) {
		uint32_t *row = anx_fb_row_ptr(row_y);

		for (col_x = x; col_x < x + w; col_x++)
			row[col_x] = color;
	}
}

void anx_fb_clear(uint32_t color)
{
	anx_fb_fill_rect(0, 0, fb.width, fb.height, color);
}

void anx_fb_scroll(uint32_t rows, uint32_t fill_color)
{
	uint8_t *base;
	uint32_t copy_height;
	uint32_t y;

	if (!fb.available || rows == 0)
		return;

	if (rows >= fb.height) {
		anx_fb_clear(fill_color);
		return;
	}

	base = (uint8_t *)(uintptr_t)fb.addr;
	copy_height = fb.height - rows;

	/* Move rows up (overlapping regions, use memmove) */
	anx_memmove(base, base + rows * fb.pitch, copy_height * fb.pitch);

	/* Fill the gap at the bottom */
	for (y = copy_height; y < fb.height; y++) {
		uint32_t *row = anx_fb_row_ptr(y);
		uint32_t x;

		for (x = 0; x < fb.width; x++)
			row[x] = fill_color;
	}
}
