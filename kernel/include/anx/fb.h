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

/* GOP mode entry (populated from EFI boot block before ExitBootServices) */
struct anx_gop_mode {
	uint32_t width;
	uint32_t height;
	uint32_t pixel_format;	/* 1 = BGRX8888 */
	uint32_t mode_number;	/* EFI GOP mode index */
};

#define ANX_GOP_MODES_MAX	16

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

/* Fill a rectangle with rounded corners (radius in pixels) */
void anx_fb_fill_rounded_rect(uint32_t x, uint32_t y,
			       uint32_t w, uint32_t h,
			       uint32_t radius, uint32_t color);

/* Fill a rectangle with a two-stop linear gradient.
 * vertical=true: top→bottom; false: left→right. */
void anx_fb_fill_gradient(uint32_t x, uint32_t y,
			   uint32_t w, uint32_t h,
			   uint32_t color_start, uint32_t color_end,
			   bool vertical);

/* Scroll the framebuffer up by n pixel rows, fill gap with color */
void anx_fb_scroll(uint32_t rows, uint32_t fill_color);

/* Direct pointer to pixel row (for bulk operations) */
uint32_t *anx_fb_row_ptr(uint32_t y);

/* Store GOP mode list from boot block (called by arch_fb_detect) */
void anx_fb_set_gop_modes(const struct anx_gop_mode *modes,
			   uint8_t count, uint8_t current_idx);

/* Query GOP mode list (count = 0 if not populated) */
const struct anx_gop_mode *anx_fb_get_gop_modes(uint8_t *count_out,
						  uint8_t *current_out);

#endif /* ANX_FB_H */
