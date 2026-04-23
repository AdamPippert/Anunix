/*
 * jpeg_enc.h — Minimal baseline JPEG encoder.
 *
 * Encodes an XRGB8888 framebuffer region to a JFIF JPEG byte stream.
 * Uses fixed standard Huffman and quantization tables (quality ~75).
 * Output is a valid standalone JPEG file.
 */

#ifndef ANX_JPEG_ENC_H
#define ANX_JPEG_ENC_H

#include <anx/types.h>

/*
 * Encode width×height pixels from xrgb_pixels (row-major, 4 bytes/pixel,
 * byte order 0x00RRGGBB) into out_buf.
 *
 * stride: bytes per row in xrgb_pixels (may be > width*4 for sub-rects)
 * out_cap: capacity of out_buf in bytes
 *
 * Returns number of JPEG bytes written, or 0 on error (buffer too small).
 */
size_t anx_jpeg_encode(const uint32_t *xrgb_pixels,
		        uint32_t width, uint32_t height, uint32_t stride,
		        uint8_t *out_buf, size_t out_cap);

#endif /* ANX_JPEG_ENC_H */
