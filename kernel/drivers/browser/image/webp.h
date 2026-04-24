/*
 * webp.h — WebP image decoder (VP8L lossless + VP8 lossy).
 *
 * Supports:
 *   VP8L  — lossless WebP (complete decoder)
 *   VP8   — lossy WebP, key frames only (intra prediction + iDCT)
 *   VP8X  — extended container wrapping either of the above
 *
 * Output: heap-allocated XRGB8888 pixel buffer (caller frees via webp_free).
 * No libc, no float — integer-only arithmetic throughout.
 */

#ifndef ANX_BROWSER_WEBP_H
#define ANX_BROWSER_WEBP_H

#include <anx/types.h>

struct webp_image {
	uint32_t  width;
	uint32_t  height;
	uint32_t *pixels;   /* XRGB8888, row-major; heap-allocated */
};

/*
 * Decode a WebP image from memory.
 * Returns 0 on success; non-zero on format error or OOM.
 * On success, img->pixels is heap-allocated — caller must call webp_free().
 */
int webp_decode(const void *data, uint32_t data_len, struct webp_image *img);

/* Free pixel buffer returned by webp_decode. */
void webp_free(struct webp_image *img);

#endif /* ANX_BROWSER_WEBP_H */
