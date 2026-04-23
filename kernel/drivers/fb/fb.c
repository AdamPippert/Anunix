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

static struct anx_gop_mode gop_modes[ANX_GOP_MODES_MAX];
static uint8_t gop_mode_count;
static uint8_t gop_current_mode;

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
	uint32_t row_y;

	if (x >= fb.width || y >= fb.height)
		return;
	if (x + w > fb.width)
		w = fb.width - x;
	if (y + h > fb.height)
		h = fb.height - y;

	if (x == 0 && w == fb.width && fb.pitch == fb.width * 4) {
		/*
		 * Full-width rect aligned to pitch: one memset covers all rows.
		 * The 64-bit anx_memset fills 8 bytes per iteration; for a
		 * repeated 32-bit color value the 64-bit word is color|color<<32.
		 * anx_memset operates on bytes, so pack the color into a byte
		 * value — only works when all four bytes of color are equal
		 * (e.g., 0x00000000, 0xFFFFFFFF). For the general case, fall
		 * through to the per-row path which uses a 32-bit word loop.
		 */
		uint8_t b = (uint8_t)(color & 0xFF);

		if (color == (uint32_t)((b << 24) | (b << 16) | (b << 8) | b)) {
			anx_memset(anx_fb_row_ptr(y), (int)b, (size_t)w * 4 * h);
			return;
		}
	}

	{
		/* Word-fill: pack two pixels into 64 bits, write 8B per iteration */
		uint64_t c64  = (uint64_t)color | ((uint64_t)color << 32);
		uint32_t even = w & ~1u;

		for (row_y = y; row_y < y + h; row_y++) {
			uint32_t *row = anx_fb_row_ptr(row_y);
			uint64_t *r64 = (uint64_t *)(row + x);
			uint32_t  i;

			for (i = 0; i < even; i += 2, r64++)
				*r64 = c64;
			if (w & 1)
				row[x + even] = color;
		}
	}
}

void anx_fb_clear(uint32_t color)
{
	/* Full-screen clear: single memmove-sized write via fill_rect */
	anx_fb_fill_rect(0, 0, fb.width, fb.height, color);
}

void anx_fb_set_gop_modes(const struct anx_gop_mode *modes,
			   uint8_t count, uint8_t current_idx)
{
	uint8_t i;

	if (!modes || count == 0)
		return;
	if (count > ANX_GOP_MODES_MAX)
		count = ANX_GOP_MODES_MAX;

	for (i = 0; i < count; i++)
		gop_modes[i] = modes[i];

	gop_mode_count   = count;
	gop_current_mode = current_idx;
}

const struct anx_gop_mode *anx_fb_get_gop_modes(uint8_t *count_out,
						  uint8_t *current_out)
{
	if (count_out)
		*count_out = gop_mode_count;
	if (current_out)
		*current_out = gop_current_mode;
	return gop_modes;
}

void anx_fb_scroll(uint32_t rows, uint32_t fill_color)
{
	uint8_t *base;
	uint32_t copy_height;

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

	/* Fill the vacated rows at the bottom */
	anx_fb_fill_rect(0, copy_height, fb.width, rows, fill_color);
}
