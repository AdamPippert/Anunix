/*
 * anx/jpeg.h — Baseline JPEG decoder.
 *
 * Decodes baseline (8-bit, sequential DCT) JPEG images to raw
 * XRGB8888 pixel data for framebuffer display. Supports standard
 * Huffman coding and YCbCr 4:2:0/4:2:2/4:4:4 subsampling.
 */

#ifndef ANX_JPEG_H
#define ANX_JPEG_H

#include <anx/types.h>

struct anx_jpeg_image {
	uint32_t width;
	uint32_t height;
	uint32_t *pixels;	/* XRGB8888, row-major */
};

/* Decode a JPEG from memory. Caller must free pixels with anx_free(). */
int anx_jpeg_decode(const void *data, uint32_t data_len,
		     struct anx_jpeg_image *out);

/* Free decoded image pixels */
void anx_jpeg_free(struct anx_jpeg_image *img);

/* Scale and blit an image to the framebuffer (nearest-neighbor) */
void anx_jpeg_blit_scaled(const struct anx_jpeg_image *img,
			   uint32_t fb_width, uint32_t fb_height);

#endif /* ANX_JPEG_H */
