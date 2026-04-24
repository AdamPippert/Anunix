/*
 * paint.h — Rasterizer: paint commands → XRGB8888 framebuffer.
 */

#ifndef ANX_BROWSER_PAINT_H
#define ANX_BROWSER_PAINT_H

#include <anx/types.h>
#include "../layout/layout.h"

/*
 * Execute all paint commands in ctx, drawing into fb.
 * fb:     XRGB8888 pixel buffer, row-major
 * w, h:   framebuffer dimensions in pixels
 * stride: bytes per row (must be >= w*4)
 */
/* scroll_y: vertical scroll offset in pixels; commands are shifted up by this amount. */
void paint_execute(const struct layout_ctx *ctx,
		    uint32_t *fb, uint32_t w, uint32_t h, uint32_t stride,
		    int32_t scroll_y);

/*
 * Fill the framebuffer with a solid color.
 */
void paint_clear(uint32_t *fb, uint32_t w, uint32_t h,
		  uint32_t stride, uint32_t color);

#endif /* ANX_BROWSER_PAINT_H */
