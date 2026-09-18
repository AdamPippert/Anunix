/*
 * wm_desktop.c — The desktop behind the windows.
 *
 * Every repaint asks for a region rather than the whole screen, so the
 * wallpaper has to be drawable in pieces: a gradient computes each row
 * from its absolute y, and an image is copied from the row it lands on.
 * Three places used to fill this area with one hardcoded navy; they all
 * come here now.
 *
 * An image wallpaper is a State Object holding a raw ARGB frame with a
 * small header, so it can be created from ansh (`wallpaper set`) like
 * any other object. A missing or mismatched image uses the embedded
 * default photograph. A gradient remains available as an explicit mode.
 */

#include <anx/types.h>
#include <anx/wm.h>
#include <anx/wallpaper.h>
#include <anx/fb.h>
#include <anx/theme.h>
#include <anx/gui.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/spinlock.h>

struct wallpaper_view {
	uint32_t width, height;
	const uint8_t *pixels;
};

/* Payloads can change after selection; validate again before every repaint. */
static bool wallpaper_view(const void *data, uint64_t size, struct wallpaper_view *view)
{
	struct anx_wallpaper_header header;

	if (!data || size < sizeof(header))
		return false;
	anx_memcpy(&header, data, sizeof(header));
	if (header.magic != ANX_WALLPAPER_MAGIC || !header.width || !header.height ||
	    header.width > ANX_WALLPAPER_MAX_DIM || header.height > ANX_WALLPAPER_MAX_DIM ||
	    (uint64_t)header.width * header.height > (size - sizeof(header)) / 4)
		return false;
	view->width = header.width;
	view->height = header.height;
	view->pixels = (const uint8_t *)data + sizeof(header);
	return true;
}

static bool default_wallpaper(struct wallpaper_view *view)
{
	uint64_t size = (uintptr_t)anx_default_wallpaper_end -
			(uintptr_t)anx_default_wallpaper_start;

	return wallpaper_view(anx_default_wallpaper_start, size, view);
}

static anx_oid_t g_wallpaper_oid;
static bool      g_have_wallpaper;

uint32_t anx_wm_shadow_reach(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t off;

	if (!theme->deco.shadow_enabled)
		return 0;
	off = theme->deco.shadow_offset_x > theme->deco.shadow_offset_y
	      ? theme->deco.shadow_offset_x : theme->deco.shadow_offset_y;
	return theme->deco.shadow_blur + off;
}

int anx_wm_wallpaper_set(const char *path)
{
	struct anx_state_object *obj;
	struct wallpaper_view view;
	anx_oid_t oid;
	int rc;

	if (!path || !path[0]) {
		g_have_wallpaper = false;
		return ANX_OK;
	}

	rc = anx_so_resolve(path, &oid);
	if (rc != ANX_OK)
		return rc;
	obj = anx_objstore_lookup(&oid);
	if (!obj)
		return ANX_ENOENT;

	rc = ANX_EINVAL;
	anx_spin_lock(&obj->lock);
	if (wallpaper_view(obj->payload, obj->payload_size, &view)) {
		g_wallpaper_oid = oid;
		g_have_wallpaper = true;
		rc = ANX_OK;
	}
	anx_spin_unlock(&obj->lock);
	anx_objstore_release(obj);
	return rc;
}

bool anx_wm_wallpaper_ready(void)
{
	struct wallpaper_view view;
	struct anx_state_object *obj = g_have_wallpaper
		? anx_objstore_lookup(&g_wallpaper_oid) : NULL;
	bool valid = false;

	if (obj) {
		anx_spin_lock(&obj->lock);
		valid = wallpaper_view(obj->payload, obj->payload_size, &view);
		anx_spin_unlock(&obj->lock);
		anx_objstore_release(obj);
	}
	return valid || default_wallpaper(&view);
}

/* Center-cover uses absolute screen coordinates so partial repaints match. */
static void wallpaper_paint(const struct wallpaper_view *view,
			    uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			    const struct anx_fb_info *fb)
{
	uint64_t numerator, denominator, xoff = 0, yoff = 0;
	uint32_t row, col;

	if ((uint64_t)fb->width * view->height >= (uint64_t)fb->height * view->width) {
		numerator = view->width;
		denominator = 2ull * fb->width;
		yoff = (uint64_t)view->height * fb->width - (uint64_t)fb->height * view->width;
	} else {
		numerator = view->height;
		denominator = 2ull * fb->height;
		xoff = (uint64_t)view->width * fb->height - (uint64_t)fb->width * view->height;
	}
	for (row = y; row < y + h; row++) {
		uint32_t sy = (uint32_t)((yoff + (2ull * row + 1) * numerator) / denominator);
		uint32_t *out = anx_fb_row_ptr(row) + x;
		uint64_t sxn = xoff + (2ull * x + 1) * numerator;

		for (col = 0; col < w; col++, sxn += 2 * numerator) {
			uint32_t sx = (uint32_t)(sxn / denominator), color;

			anx_memcpy(&color, view->pixels + ((uint64_t)sy * view->width + sx) * 4, 4);
			out[col] = color & 0x00FFFFFF;
		}
	}
	anx_fb_mark_dirty(x, y, w, h);
}

/*
 * Paint one region of the desktop. x, y, w, h are screen coordinates and
 * clipped to the framebuffer before painting.
 */
void anx_wm_desktop_paint(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	const struct anx_theme *theme = anx_theme_get();
	const struct anx_fb_info *fb = anx_fb_get_info();
	uint32_t top, bottom, row;

	if (!fb || !fb->available || !w || !h || x >= fb->width || y >= fb->height)
		return;

	if (w > fb->width - x) w = fb->width - x;
	if (h > fb->height - y) h = fb->height - y;
	if (theme->deco.wallpaper == ANX_WALLPAPER_IMAGE) {
		struct wallpaper_view view;
		struct anx_state_object *obj = g_have_wallpaper
			? anx_objstore_lookup(&g_wallpaper_oid) : NULL;
		bool valid = false;

		if (obj) {
			/* Keep a concurrent payload replacement from freeing the sampled pixels. */
			anx_spin_lock(&obj->lock);
			valid = wallpaper_view(obj->payload, obj->payload_size, &view);
		}
		if (!valid)
			valid = default_wallpaper(&view);
		if (valid)
			wallpaper_paint(&view, x, y, w, h, fb);
		if (obj) {
			anx_spin_unlock(&obj->lock);
			anx_objstore_release(obj);
		}
		if (valid)
			return;
	}

	if (theme->deco.wallpaper == ANX_WALLPAPER_SOLID) {
		anx_fb_fill_rect(x, y, w, h, theme->palette.background);
		return;
	}

	/*
	 * A gradient down the whole screen, drawn one region at a time:
	 * each row takes its color from its absolute position, so a patch
	 * repainted on its own still lines up with what is around it.
	 */
	top = theme->palette.wallpaper_from;
	bottom = theme->palette.wallpaper_to;
	for (row = 0; row < h; row++) {
		uint32_t sy = y + row;
		uint32_t t = fb->height > 1 ? (uint32_t)((uint64_t)sy * 255 / (fb->height - 1)) : 0;
		uint32_t r = (((top >> 16) & 0xFF) * (255 - t) +
			      ((bottom >> 16) & 0xFF) * t) / 255;
		uint32_t g = (((top >> 8) & 0xFF) * (255 - t) +
			      ((bottom >> 8) & 0xFF) * t) / 255;
		uint32_t b = ((top & 0xFF) * (255 - t) +
			      (bottom & 0xFF) * t) / 255;

		anx_fb_fill_rect(x, sy, w, 1, (r << 16) | (g << 8) | b);
	}
}
