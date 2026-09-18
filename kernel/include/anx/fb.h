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

/* Remap the framebuffer write-combining; call after the page allocator. */
void anx_fb_enable_wc(void);

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

/* Fill a rectangle with a three-stop gradient (stops at 0%, 40%, 100%).
 * diagonal=true: top-left→bottom-right (135°); false: left→right. */
void anx_fb_fill_gradient3(uint32_t x, uint32_t y,
			    uint32_t w, uint32_t h,
			    uint32_t c0, uint32_t c1, uint32_t c2,
			    bool diagonal);

/*
 * Draw into RAM instead of straight into video memory.
 *
 * Reads from the framebuffer are slow: it is mapped write-combining, so
 * every blend, shadow and transparent window would stall on video memory.
 * With a back buffer every primitive works on RAM, and anx_fb_flush()
 * copies what changed to the screen in one pass.
 *
 * Returns ANX_OK, or ANX_ENOMEM when the buffer will not fit.
 */
int  anx_fb_enable_backbuffer(void);
bool anx_fb_has_backbuffer(void);

/* Copy the accumulated dirty region to video memory. */
void anx_fb_flush(void);

/* Mark a region as needing a flush (the primitives do this themselves). */
void anx_fb_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/*
 * Corner treatments. The Anunix signature shape rounds the upper-left
 * and lower-right corners and mitres (45-degree chamfer) the other two.
 */
enum anx_corner_style {
	ANX_CORNER_SQUARE = 0,
	ANX_CORNER_ROUND,
	ANX_CORNER_MITRE,
};

/* Corner order: top-left, top-right, bottom-right, bottom-left. */
struct anx_shape {
	uint32_t radius;
	uint8_t  corner[4];
};

/* The signature shape at the given corner size. */
struct anx_shape anx_fb_shape_signature(uint32_t radius);

/* A shape with all four corners the same. */
struct anx_shape anx_fb_shape_uniform(uint32_t radius,
				      enum anx_corner_style style);

/* Fill a shape with one color. */
void anx_fb_fill_shape(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		       const struct anx_shape *shape, uint32_t color);

/* Fill a shape with a two-stop gradient (vertical: top to bottom). */
void anx_fb_fill_shape_gradient(uint32_t x, uint32_t y,
				uint32_t w, uint32_t h,
				const struct anx_shape *shape,
				uint32_t color_start, uint32_t color_end,
				bool vertical);

/* Blend one color over a rectangle; alpha 0 = invisible, 255 = opaque. */
void anx_fb_blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		       uint32_t color, uint8_t alpha);

/* Blend one color over a shape. */
void anx_fb_blend_shape(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			const struct anx_shape *shape, uint32_t color,
			uint8_t alpha);

/*
 * Blend a soft drop shadow for the shape at (x, y, w, h), offset by
 * (dx, dy) and spreading `blur` pixels outward. Draw it before the
 * shape itself: the area the shape covers is left alone.
 */
void anx_fb_shadow_shape(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			 const struct anx_shape *shape,
			 int32_t dx, int32_t dy, uint32_t blur,
			 uint32_t color, uint8_t alpha);

/* Blend an image over a shape's area, one row at a time (RGB source). */
void anx_fb_blend_row(uint32_t x, uint32_t y, uint32_t w,
		      const uint32_t *src, uint8_t alpha);

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
