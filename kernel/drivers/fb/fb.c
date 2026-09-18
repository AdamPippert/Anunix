/*
 * fb.c — Framebuffer core operations.
 *
 * Provides pixel-level access to a linear XRGB8888 framebuffer.
 * The framebuffer address and geometry come from architecture
 * code during boot (multiboot on x86_64, ramfb on arm64).
 */

#include <anx/types.h>
#include <anx/fb.h>
#include <anx/mmio.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/page.h>

static struct anx_fb_info fb;

/* ------------------------------------------------------------------ */
/* Back buffer                                                          */
/* ------------------------------------------------------------------ */

static uint32_t *back;			/* RAM mirror of the screen */
static uint32_t  back_order;		/* page order of the allocation */
static uint32_t  dirty_x0, dirty_y0;	/* region waiting for a flush */
static uint32_t  dirty_x1, dirty_y1;	/* exclusive; x1 == 0 means clean */

bool anx_fb_has_backbuffer(void)
{
	return back != NULL;
}

int anx_fb_enable_backbuffer(void)
{
	uint64_t bytes;
	uint32_t order = 0;
	uintptr_t mem;

	if (back)
		return ANX_OK;
	if (!fb.available)
		return ANX_ENODEV;

	bytes = (uint64_t)fb.width * fb.height * 4;
	while (((uint64_t)ANX_PAGE_SIZE << order) < bytes && order < 20)
		order++;
	mem = anx_page_alloc(order);
	if (!mem)
		return ANX_ENOMEM;

	back = (uint32_t *)mem;
	back_order = order;

	/* Start from what is already on screen so nothing flickers */
	{
		uint32_t y;

		for (y = 0; y < fb.height; y++) {
			uint8_t *src = (uint8_t *)(uintptr_t)fb.addr +
				       (uint64_t)y * fb.pitch;

			anx_memcpy(back + (uint64_t)y * fb.width, src,
				   (size_t)fb.width * 4);
		}
	}
	kprintf("fb: %u KiB back buffer (%ux%u)\n",
		(uint32_t)(bytes >> 10), fb.width, fb.height);
	return ANX_OK;
}

void anx_fb_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	if (!back || w == 0 || h == 0 || x >= fb.width || y >= fb.height)
		return;
	if (x + w > fb.width)
		w = fb.width - x;
	if (y + h > fb.height)
		h = fb.height - y;

	if (dirty_x1 == 0) {
		dirty_x0 = x;
		dirty_y0 = y;
		dirty_x1 = x + w;
		dirty_y1 = y + h;
		return;
	}
	if (x < dirty_x0)
		dirty_x0 = x;
	if (y < dirty_y0)
		dirty_y0 = y;
	if (x + w > dirty_x1)
		dirty_x1 = x + w;
	if (y + h > dirty_y1)
		dirty_y1 = y + h;
}

void anx_fb_flush(void)
{
	uint32_t y, w;

	if (!back || dirty_x1 == 0)
		return;
	w = dirty_x1 - dirty_x0;
	for (y = dirty_y0; y < dirty_y1; y++) {
		uint8_t *dst = (uint8_t *)(uintptr_t)fb.addr +
			       (uint64_t)y * fb.pitch + (uint64_t)dirty_x0 * 4;

		anx_memcpy(dst, back + (uint64_t)y * fb.width + dirty_x0,
			   (size_t)w * 4);
	}
	dirty_x1 = 0;
}

static struct anx_gop_mode gop_modes[ANX_GOP_MODES_MAX];
static uint8_t gop_mode_count;
static uint8_t gop_current_mode;

int anx_fb_init(const struct anx_fb_info *info)
{
	if (!info || !info->available || info->addr == 0)
		return ANX_EINVAL;
	if (info->bpp != 32)
		return ANX_EINVAL;

	fb = *info;
	return ANX_OK;
}

/*
 * Remap the framebuffer write-combining. Called once the page allocator
 * exists, since remapping may need a new page table.
 */
void anx_fb_enable_wc(void)
{
	uint64_t size;

	if (!fb.available)
		return;
	if (!anx_pat_enable_wc())
		return;
	size = (uint64_t)fb.pitch * fb.height;
	if (anx_mmio_map_wc(fb.addr, size) == NULL)
		kprintf("fb: write-combining map failed\n");
	else
		kprintf("fb: 0x%llx+0x%llx mapped write-combining\n",
			(unsigned long long)fb.addr, (unsigned long long)size);
}

bool anx_fb_available(void)
{
	return fb.available;
}

const struct anx_fb_info *anx_fb_get_info(void)
{
	return &fb;
}

uint32_t *anx_fb_row_ptr(uint32_t y)
{
	uint8_t *base;

	if (back)
		return back + (uint64_t)y * fb.width;
	base = (uint8_t *)(uintptr_t)fb.addr;
	return (uint32_t *)(base + y * fb.pitch);
}

void anx_fb_putpixel(uint32_t x, uint32_t y, uint32_t color)
{
	uint32_t *row;

	if (x >= fb.width || y >= fb.height)
		return;
	anx_fb_mark_dirty(x, y, 1, 1);

	row = anx_fb_row_ptr(y);
	row[x] = color;
}

void anx_fb_fill_rect(uint32_t x, uint32_t y,
		       uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row_y;

	if (x >= fb.width || y >= fb.height)
		return;
	if (x + w > fb.width)
		w = fb.width - x;
	if (y + h > fb.height)
		h = fb.height - y;
	anx_fb_mark_dirty(x, y, w, h);

	if (back || (x == 0 && w == fb.width && fb.pitch == fb.width * 4)) {
		/*
		 * Full-width rect aligned to pitch: one memset covers all rows.
		 * The 64-bit anx_memset fills 8 bytes per iteration; for a
		 * repeated 32-bit color value the 64-bit word is color|color<<32.
		 * anx_memset operates on bytes, so pack the color into a byte
		 * value — only works when all four bytes of color are equal
		 * (e.g., 0x00000000, 0xFFFFFFFF). For the general case, fall
		 * through to the per-row path which uses a 32-bit word loop.
		 */
		uint8_t b = (uint8_t)(color & 0xFF);

		if (color == (uint32_t)((b << 24) | (b << 16) | (b << 8) | b)) {
			anx_memset(anx_fb_row_ptr(y), (int)b, (size_t)w * 4 * h);
			return;
		}
	}

	{
		/* Word-fill: pack two pixels into 64 bits, write 8B per iteration */
		uint64_t c64  = (uint64_t)color | ((uint64_t)color << 32);
		uint32_t even = w & ~1u;

		for (row_y = y; row_y < y + h; row_y++) {
			uint32_t *row = anx_fb_row_ptr(row_y);
			uint64_t *r64 = (uint64_t *)(row + x);
			uint32_t  i;

			for (i = 0; i < even; i += 2, r64++)
				*r64 = c64;
			if (w & 1)
				row[x + even] = color;
		}
	}
}

void anx_fb_clear(uint32_t color)
{
	/* Full-screen clear: single memmove-sized write via fill_rect */
	anx_fb_fill_rect(0, 0, fb.width, fb.height, color);
}

void anx_fb_fill_rounded_rect(uint32_t x, uint32_t y,
			       uint32_t w, uint32_t h,
			       uint32_t radius, uint32_t color)
{
	uint32_t r, row_y, col_x, dx, dy;

	if (w == 0 || h == 0)
		return;
	anx_fb_mark_dirty(x, y, w, h);

	/* Clamp radius so it fits inside the rect */
	r = radius;
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;

	for (row_y = y; row_y < y + h; row_y++) {
		uint32_t row_off = row_y - y;
		uint32_t x_start = x;
		uint32_t x_end   = x + w;

		/* Determine horizontal clipping from rounded corners */
		if (row_off < r) {
			/* Top edge — corner quarter-circles */
			dy = r - row_off;
			for (col_x = x; col_x < x + r; col_x++) {
				dx = r - (col_x - x);
				if (dx * dx + dy * dy > r * r)
					x_start = col_x + 1;
				else
					break;
			}
			for (col_x = x + w - 1; col_x >= x + w - r; col_x--) {
				dx = r - (x + w - 1 - col_x);
				if (dx * dx + dy * dy > r * r)
					x_end = col_x;
				else
					break;
			}
		} else if (row_off >= h - r) {
			/* Bottom edge — corner quarter-circles */
			dy = r - (h - 1 - row_off);
			for (col_x = x; col_x < x + r; col_x++) {
				dx = r - (col_x - x);
				if (dx * dx + dy * dy > r * r)
					x_start = col_x + 1;
				else
					break;
			}
			for (col_x = x + w - 1; col_x >= x + w - r; col_x--) {
				dx = r - (x + w - 1 - col_x);
				if (dx * dx + dy * dy > r * r)
					x_end = col_x;
				else
					break;
			}
		}

		if (x_start < x_end) {
			/* Clip to framebuffer */
			uint32_t sx = (x_start < fb.width) ? x_start : fb.width;
			uint32_t ex = (x_end   < fb.width) ? x_end   : fb.width;
			uint32_t ry = (row_y   < fb.height) ? row_y   : fb.height;

			if (ry < fb.height && sx < ex) {
				uint32_t *row = anx_fb_row_ptr(ry);
				uint32_t i;
				for (i = sx; i < ex; i++)
					row[i] = color;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Shapes: per-corner rounded or mitred rectangles                      */
/* ------------------------------------------------------------------ */

static uint32_t isqrt32(uint32_t v)
{
	uint32_t rem = 0, root = 0, i;

	for (i = 0; i < 16; i++) {
		root <<= 1;
		rem = (rem << 2) | (v >> 30);
		v <<= 2;
		if (root < rem) {
			rem -= root | 1;
			root += 2;
		}
	}
	return root >> 1;
}

struct anx_shape anx_fb_shape_signature(uint32_t radius)
{
	struct anx_shape s;

	s.radius = radius;
	s.corner[0] = ANX_CORNER_ROUND;		/* top-left */
	s.corner[1] = ANX_CORNER_MITRE;		/* top-right */
	s.corner[2] = ANX_CORNER_ROUND;		/* bottom-right */
	s.corner[3] = ANX_CORNER_MITRE;		/* bottom-left */
	return s;
}

struct anx_shape anx_fb_shape_uniform(uint32_t radius,
				      enum anx_corner_style style)
{
	struct anx_shape s;
	uint32_t i;

	s.radius = radius;
	for (i = 0; i < 4; i++)
		s.corner[i] = (uint8_t)style;
	return s;
}

/*
 * How far a corner eats into a row: `depth` counts rows into the corner
 * (0 at the outermost row), so a round corner follows the circle and a
 * mitre falls away at 45 degrees.
 */
static uint32_t corner_inset(uint8_t style, uint32_t r, uint32_t depth)
{
	uint32_t dy;

	if (style == ANX_CORNER_SQUARE || r == 0 || depth >= r)
		return 0;
	dy = r - depth;
	if (style == ANX_CORNER_MITRE)
		return dy;
	return r - isqrt32(r * r - dy * dy);	/* round */
}

/* Horizontal span [*x0, *x1) of the shape on one row; false when empty. */
static bool shape_span(uint32_t w, uint32_t h, const struct anx_shape *sh,
		       uint32_t row_off, uint32_t *x0, uint32_t *x1)
{
	uint32_t r = sh->radius, left = 0, right = 0;

	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;

	if (row_off < r) {
		left  = corner_inset(sh->corner[0], r, row_off);
		right = corner_inset(sh->corner[1], r, row_off);
	} else if (row_off + r >= h) {
		uint32_t depth = h - 1 - row_off;

		left  = corner_inset(sh->corner[3], r, depth);
		right = corner_inset(sh->corner[2], r, depth);
	}
	if (left + right >= w)
		return false;
	*x0 = left;
	*x1 = w - right;
	return true;
}

/* Clip a row span to the screen and return the row pointer, or NULL. */
static uint32_t *clip_span(uint32_t x, uint32_t y, uint32_t *sx, uint32_t *ex)
{
	if (y >= fb.height || *sx >= *ex)
		return NULL;
	if (x + *sx >= fb.width)
		return NULL;
	*sx += x;
	*ex += x;
	if (*ex > fb.width)
		*ex = fb.width;
	return anx_fb_row_ptr(y);
}

void anx_fb_fill_shape(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		       const struct anx_shape *shape, uint32_t color)
{
	uint32_t row_off;

	if (!shape || w == 0 || h == 0)
		return;
	anx_fb_mark_dirty(x, y, w, h);

	for (row_off = 0; row_off < h; row_off++) {
		uint32_t sx, ex, i;
		uint32_t *row;

		if (!shape_span(w, h, shape, row_off, &sx, &ex))
			continue;
		row = clip_span(x, y + row_off, &sx, &ex);
		if (!row)
			continue;
		for (i = sx; i < ex; i++)
			row[i] = color;
	}
}

/* Linear blend between two colors; t is 0..255. */
static uint32_t color_mix(uint32_t a, uint32_t b, uint32_t t)
{
	uint32_t r = (((a >> 16) & 0xFF) * (255 - t) + ((b >> 16) & 0xFF) * t) / 255;
	uint32_t g = (((a >>  8) & 0xFF) * (255 - t) + ((b >>  8) & 0xFF) * t) / 255;
	uint32_t bl = ((a & 0xFF) * (255 - t) + (b & 0xFF) * t) / 255;

	return (r << 16) | (g << 8) | bl;
}

void anx_fb_fill_shape_gradient(uint32_t x, uint32_t y,
				uint32_t w, uint32_t h,
				const struct anx_shape *shape,
				uint32_t color_start, uint32_t color_end,
				bool vertical)
{
	uint32_t row_off;

	if (!shape || w == 0 || h == 0)
		return;
	anx_fb_mark_dirty(x, y, w, h);

	for (row_off = 0; row_off < h; row_off++) {
		uint32_t sx, ex, i, t;
		uint32_t *row;

		if (!shape_span(w, h, shape, row_off, &sx, &ex))
			continue;
		row = clip_span(x, y + row_off, &sx, &ex);
		if (!row)
			continue;
		if (vertical) {
			t = h > 1 ? row_off * 255 / (h - 1) : 0;
			{
				uint32_t c = color_mix(color_start, color_end, t);

				for (i = sx; i < ex; i++)
					row[i] = c;
			}
		} else {
			for (i = sx; i < ex; i++) {
				t = w > 1 ? (i - x) * 255 / (w - 1) : 0;
				row[i] = color_mix(color_start, color_end, t);
			}
		}
	}
}

/* Blend src over dst by alpha (0..255). */
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t alpha)
{
	uint32_t inv = 255 - alpha;
	uint32_t r = ((((src >> 16) & 0xFF) * alpha) + (((dst >> 16) & 0xFF) * inv)) / 255;
	uint32_t g = ((((src >>  8) & 0xFF) * alpha) + (((dst >>  8) & 0xFF) * inv)) / 255;
	uint32_t b = (((src & 0xFF) * alpha) + ((dst & 0xFF) * inv)) / 255;

	return (r << 16) | (g << 8) | b;
}

void anx_fb_blend_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		       uint32_t color, uint8_t alpha)
{
	uint32_t row_y;

	if (alpha == 0 || w == 0 || h == 0 || x >= fb.width || y >= fb.height)
		return;
	if (x + w > fb.width)
		w = fb.width - x;
	if (y + h > fb.height)
		h = fb.height - y;
	anx_fb_mark_dirty(x, y, w, h);

	for (row_y = y; row_y < y + h; row_y++) {
		uint32_t *row = anx_fb_row_ptr(row_y);
		uint32_t i;

		for (i = x; i < x + w; i++)
			row[i] = blend(row[i], color, alpha);
	}
}

void anx_fb_blend_shape(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			const struct anx_shape *shape, uint32_t color,
			uint8_t alpha)
{
	uint32_t row_off;

	if (!shape || alpha == 0 || w == 0 || h == 0)
		return;
	anx_fb_mark_dirty(x, y, w, h);

	for (row_off = 0; row_off < h; row_off++) {
		uint32_t sx, ex, i;
		uint32_t *row;

		if (!shape_span(w, h, shape, row_off, &sx, &ex))
			continue;
		row = clip_span(x, y + row_off, &sx, &ex);
		if (!row)
			continue;
		for (i = sx; i < ex; i++)
			row[i] = blend(row[i], color, alpha);
	}
}

void anx_fb_blend_row(uint32_t x, uint32_t y, uint32_t w,
		      const uint32_t *src, uint8_t alpha)
{
	uint32_t *row;
	uint32_t i;

	if (!src || alpha == 0 || y >= fb.height || x >= fb.width)
		return;
	if (x + w > fb.width)
		w = fb.width - x;
	anx_fb_mark_dirty(x, y, w, 1);
	row = anx_fb_row_ptr(y);
	if (alpha == 255) {
		for (i = 0; i < w; i++)
			row[x + i] = src[i];
		return;
	}
	for (i = 0; i < w; i++)
		row[x + i] = blend(row[x + i], src[i], alpha);
}

/*
 * The shadow is drawn as concentric rings around the shape, each one
 * pixel further out and fainter than the last. Only the ring is touched,
 * so the cost follows the perimeter rather than the area, and the shape
 * itself is left for the caller to draw on top.
 */
void anx_fb_shadow_shape(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			 const struct anx_shape *shape,
			 int32_t dx, int32_t dy, uint32_t blur,
			 uint32_t color, uint8_t alpha)
{
	int32_t bx = (int32_t)x + dx, by = (int32_t)y + dy;
	uint32_t step;

	if (!shape || alpha == 0 || w == 0 || h == 0)
		return;
	if (blur == 0)
		blur = 1;
	if (bx - (int32_t)blur >= 0 && by - (int32_t)blur >= 0)
		anx_fb_mark_dirty((uint32_t)(bx - (int32_t)blur),
				  (uint32_t)(by - (int32_t)blur),
				  w + 2 * blur, h + 2 * blur);

	for (step = blur; step >= 1; step--) {
		struct anx_shape ring = *shape;
		int32_t rx = bx - (int32_t)step, ry = by - (int32_t)step;
		uint32_t rw = w + 2 * step, rh = h + 2 * step;
		uint32_t a, row_off;

		/* Quadratic falloff: dense against the frame, faint outside */
		a = (uint32_t)alpha * (blur - step + 1) * (blur - step + 1) /
		    (blur * blur * 2);
		if (a == 0)
			continue;
		ring.radius = shape->radius + step;

		for (row_off = 0; row_off < rh; row_off++) {
			uint32_t sx, ex, i, isx, iex;
			bool has_inner;
			uint32_t *row;
			int32_t py = ry + (int32_t)row_off;

			if (py < 0 || rx < 0)
				continue;
			if (!shape_span(rw, rh, &ring, row_off, &sx, &ex))
				continue;

			/* Skip the previous ring: blend only the new band */
			has_inner = row_off >= 1 && row_off <= rh - 2 &&
				    shape_span(rw - 2, rh - 2, shape,
					       row_off - 1, &isx, &iex);
			row = clip_span((uint32_t)rx, (uint32_t)py, &sx, &ex);
			if (!row)
				continue;
			if (has_inner) {
				isx += (uint32_t)rx + 1;
				iex += (uint32_t)rx + 1;
				for (i = sx; i < ex; i++) {
					if (i >= isx && i < iex)
						continue;
					row[i] = blend(row[i], color, a);
				}
			} else {
				for (i = sx; i < ex; i++)
					row[i] = blend(row[i], color, a);
			}
		}
	}
}

/* 3-stop gradient across a rect.
 * diagonal=true: blends top-left→bottom-right (135°), stops at 40% and 100%.
 * diagonal=false: horizontal, stops at 40% and 100%. */
void anx_fb_fill_gradient3(uint32_t x, uint32_t y,
			    uint32_t w, uint32_t h,
			    uint32_t c0, uint32_t c1, uint32_t c2,
			    bool diagonal)
{
	uint32_t px, py;
	int32_t r0, g0, b0, r1, g1, b1, r2, g2, b2;

	anx_fb_mark_dirty(x, y, w, h);

	if (!w || !h)
		return;

	r0 = (int32_t)((c0 >> 16) & 0xFF);
	g0 = (int32_t)((c0 >>  8) & 0xFF);
	b0 = (int32_t)( c0        & 0xFF);
	r1 = (int32_t)((c1 >> 16) & 0xFF);
	g1 = (int32_t)((c1 >>  8) & 0xFF);
	b1 = (int32_t)( c1        & 0xFF);
	r2 = (int32_t)((c2 >> 16) & 0xFF);
	g2 = (int32_t)((c2 >>  8) & 0xFF);
	b2 = (int32_t)( c2        & 0xFF);

	for (py = y; py < y + h && py < fb.height; py++) {
		uint32_t *row = anx_fb_row_ptr(py);
		for (px = x; px < x + w && px < fb.width; px++) {
			/* t in [0, 1024] */
			uint32_t t;
			int32_t  r, g, b, lt;

			if (diagonal)
				t = (px - x) * 512 / w + (py - y) * 512 / h;
			else
				t = (px - x) * 1024 / w;

			/* Mid-stop at t=410 (≈ 40%) */
			if (t < 410) {
				lt = (int32_t)t * 1024 / 410;
				r = r0 + (r1 - r0) * lt / 1024;
				g = g0 + (g1 - g0) * lt / 1024;
				b = b0 + (b1 - b0) * lt / 1024;
			} else {
				lt = ((int32_t)t - 410) * 1024 / 614;
				r = r1 + (r2 - r1) * lt / 1024;
				g = g1 + (g2 - g1) * lt / 1024;
				b = b1 + (b2 - b1) * lt / 1024;
			}

			row[px] = ((uint32_t)r << 16) |
				  ((uint32_t)g <<  8) |
				   (uint32_t)b;
		}
	}
}

void anx_fb_fill_gradient(uint32_t x, uint32_t y,
			   uint32_t w, uint32_t h,
			   uint32_t color_start, uint32_t color_end,
			   bool vertical)
{
	uint32_t row_y, col_x, steps, i;
	uint32_t rs, gs, bs, re, ge, be;

	anx_fb_mark_dirty(x, y, w, h);

	if (w == 0 || h == 0)
		return;

	rs = (color_start >> 16) & 0xFF;
	gs = (color_start >>  8) & 0xFF;
	bs =  color_start        & 0xFF;
	re = (color_end   >> 16) & 0xFF;
	ge = (color_end   >>  8) & 0xFF;
	be =  color_end          & 0xFF;

	steps = vertical ? h : w;
	if (steps == 0) steps = 1;

	for (row_y = y; row_y < y + h && row_y < fb.height; row_y++) {
		for (col_x = x; col_x < x + w && col_x < fb.width; col_x++) {
			uint32_t *row = anx_fb_row_ptr(row_y);
			int32_t r2, g2, b2;

			i = vertical ? (row_y - y) : (col_x - x);
			r2 = (int32_t)rs + ((int32_t)re - (int32_t)rs) * (int32_t)i / (int32_t)steps;
			g2 = (int32_t)gs + ((int32_t)ge - (int32_t)gs) * (int32_t)i / (int32_t)steps;
			b2 = (int32_t)bs + ((int32_t)be - (int32_t)bs) * (int32_t)i / (int32_t)steps;
			row[col_x] = ((uint32_t)r2 << 16) | ((uint32_t)g2 << 8) | (uint32_t)b2;
		}
	}
}

void anx_fb_set_gop_modes(const struct anx_gop_mode *modes,
			   uint8_t count, uint8_t current_idx)
{
	uint8_t i;

	if (!modes || count == 0)
		return;
	if (count > ANX_GOP_MODES_MAX)
		count = ANX_GOP_MODES_MAX;

	for (i = 0; i < count; i++)
		gop_modes[i] = modes[i];

	gop_mode_count   = count;
	gop_current_mode = current_idx;
}

const struct anx_gop_mode *anx_fb_get_gop_modes(uint8_t *count_out,
						  uint8_t *current_out)
{
	if (count_out)
		*count_out = gop_mode_count;
	if (current_out)
		*current_out = gop_current_mode;
	return gop_modes;
}

void anx_fb_scroll(uint32_t rows, uint32_t fill_color)
{
	uint8_t *base;
	uint32_t copy_height;

	anx_fb_mark_dirty(0, 0, fb.width, fb.height);

	if (!fb.available || rows == 0)
		return;

	if (rows >= fb.height) {
		anx_fb_clear(fill_color);
		return;
	}

	base = (uint8_t *)(uintptr_t)fb.addr;
	copy_height = fb.height - rows;

	/* Move rows up (overlapping regions, use memmove) */
	anx_memmove(base, base + rows * fb.pitch, copy_height * fb.pitch);

	/* Fill the vacated rows at the bottom */
	anx_fb_fill_rect(0, copy_height, fb.width, rows, fill_color);
}
