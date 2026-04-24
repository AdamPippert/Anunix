#ifndef ANX_BROWSER_PNG_H
#define ANX_BROWSER_PNG_H

#include <anx/types.h>

struct png_image {
	uint32_t  width;
	uint32_t  height;
	uint32_t *pixels;	/* XRGB8888, row-major; heap-allocated */
};

/* Returns 0 on success. img->pixels is caller-owned, free with png_free(). */
int  png_decode(const void *data, uint32_t data_len, struct png_image *img);
void png_free(struct png_image *img);

#endif /* ANX_BROWSER_PNG_H */
