#ifndef ANX_BROWSER_JPEG_H
#define ANX_BROWSER_JPEG_H

#include <anx/types.h>

struct jpeg_image {
	uint32_t  width;
	uint32_t  height;
	uint32_t *pixels;	/* XRGB8888, row-major; heap-allocated */
};

/* Returns 0 on success. img->pixels is caller-owned, free with jpeg_free(). */
int  jpeg_decode(const void *data, uint32_t data_len, struct jpeg_image *img);
void jpeg_free(struct jpeg_image *img);

#endif /* ANX_BROWSER_JPEG_H */
