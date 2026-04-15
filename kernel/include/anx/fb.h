/*
 * anx/fb.h — Framebuffer interface.
 *
 * Provides pixel-level access to a linear framebuffer.
 * Architecture code fills in anx_fb_info during boot;
 * core code uses fb_*() functions to draw.
 */

#ifndef ANX_FB_H
#define ANX_FB_H

#include <anx/types.h>

struct anx_fb_info {
	uint64_t addr;		/* physical/virtual address of pixel data */
	uint32_t width;		/* pixels per scanline */
	uint32_t height;	/* number of scanlines */
	uint32_t pitch;		/* bytes per scanline (may exceed width*bpp/8) */
	uint8_t  bpp;		/* bits per pixel (32 = XRGB8888) */
	bool     available;	/* true if framebuffer was set up */
};

/* Initialize framebuffer subsystem with hardware-provided info */
int anx_fb_init(const struct anx_fb_info *info);

/* Query whether framebuffer is available */
bool anx_fb_available(void);

/* Get current framebuffer info (valid only if available) */
const struct anx_fb_info *anx_fb_get_info(void);

/* Write a single pixel (XRGB8888: 0x00RRGGBB) */
void anx_fb_putpixel(uint32_t x, uint32_t y, uint32_t color);

/* Fill a rectangle with a solid color */
void anx_fb_fill_rect(uint32_t x, uint32_t y,
		       uint32_t w, uint32_t h, uint32_t color);

/* Clear entire screen to a color */
void anx_fb_clear(uint32_t color);

/* Scroll the framebuffer up by n pixel rows, fill gap with color */
void anx_fb_scroll(uint32_t rows, uint32_t fill_color);

/* Direct pointer to pixel row (for bulk operations) */
uint32_t *anx_fb_row_ptr(uint32_t y);

#endif /* ANX_FB_H */
