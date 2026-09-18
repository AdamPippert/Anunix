#ifndef ANX_WALLPAPER_H
#define ANX_WALLPAPER_H

#include <anx/types.h>

/* Existing State Object format: little-endian header and XRGB8888 pixels. */
#define ANX_WALLPAPER_MAGIC 0x414E5750u
#define ANX_WALLPAPER_MAX_DIM 16384u

struct anx_wallpaper_header {
	uint32_t magic, width, height, reserved;
};

extern const uint8_t anx_default_wallpaper_start[];
extern const uint8_t anx_default_wallpaper_end[];

#endif /* ANX_WALLPAPER_H */
