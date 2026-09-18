/*
 * renderer_gpu.c — GPU (framebuffer) renderer for the Interface Plane.
 *
 * Handles CANVAS (pixel blit), TEXT, and BUTTON content nodes using
 * the existing fb.c and gui.c drawing primitives. This renderer is the
 * bridge between the abstract Interface Plane and the physical display.
 *
 * Registered at boot as ANX_ENGINE_RENDERER_GPU.
 */

#include <anx/interface_plane.h>
#include <anx/fb.h>
#include <anx/kprintf.h>
#include <anx/gui.h>
#include <anx/font.h>
#include <anx/theme.h>
#include <anx/window_chrome.h>
#include <anx/string.h>
#include <anx/types.h>
#include <anx/wm.h>
#include <anx/input.h>

/* ------------------------------------------------------------------ */
/* Content rendering helpers                                            */
/* ------------------------------------------------------------------ */

#define BUTTON_PAD_X   8u
#define BUTTON_PAD_Y   4u
#define BUTTON_BORDER  2u

/* Same integer circle as the framebuffer's rounded corners. */
static uint32_t isqrt_corner(uint32_t r, uint32_t dy)
{
	uint32_t v = r * r - dy * dy, rem = 0, root = 0, i;

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

/*
 * The frame shape the canvas is being drawn inside, so the blit can stop
 * at a rounded or mitred corner instead of squaring it off.
 */
static struct {
	bool             on;
	int32_t          x, y;
	uint32_t         w, h;
	struct anx_shape shape;
} g_clip;

/* Absolute x range the frame allows on this screen row. */
static bool clip_row(uint32_t abs_y, uint32_t *x0, uint32_t *x1)
{
	uint32_t r, row_off, left = 0, right = 0, depth;
	uint8_t cl, cr;

	if (!g_clip.on)
		return false;
	if ((int32_t)abs_y < g_clip.y ||
	    abs_y >= (uint32_t)(g_clip.y + (int32_t)g_clip.h))
		return false;

	r = g_clip.shape.radius;
	if (r > g_clip.w / 2)
		r = g_clip.w / 2;
	if (r > g_clip.h / 2)
		r = g_clip.h / 2;
	if (r == 0)
		return false;

	row_off = abs_y - (uint32_t)g_clip.y;
	if (row_off < r) {
		depth = row_off;
		cl = g_clip.shape.corner[0];
		cr = g_clip.shape.corner[1];
	} else if (row_off + r >= g_clip.h) {
		depth = g_clip.h - 1 - row_off;
		cl = g_clip.shape.corner[3];
		cr = g_clip.shape.corner[2];
	} else {
		return false;
	}

	if (cl == ANX_CORNER_MITRE)
		left = r - depth;
	else if (cl == ANX_CORNER_ROUND)
		left = r - isqrt_corner(r, r - depth);
	if (cr == ANX_CORNER_MITRE)
		right = r - depth;
	else if (cr == ANX_CORNER_ROUND)
		right = r - isqrt_corner(r, r - depth);

	*x0 = (uint32_t)g_clip.x + left;
	*x1 = (uint32_t)g_clip.x + g_clip.w - right;
	return true;
}

static void
render_canvas(struct anx_surface *surf, struct anx_content_node *node)
{
	const struct anx_fb_info *fbinfo;
	const uint32_t *src;
	uint32_t        row;
	uint32_t        dst_y;
	uint32_t        r0, r1, c0, c1;
	uint32_t        fb_x0, copy_w;
	uint32_t        bw, bh, vis_w, vis_h;
	uint32_t        clip_x0 = 0, clip_x1 = 0;
	uint8_t         blend_alpha;

	/*
	 * The buffer keeps the size it was created with, but the WM may have
	 * resized the surface since (maximize, tile, drag-resize). Blit with
	 * the buffer's own stride and only where both overlap: a grown window
	 * shows its content unscaled instead of going blank, and a shrunk one
	 * is cropped instead of sheared.
	 */
	bw = surf->buf_w ? surf->buf_w : surf->width;
	bh = surf->buf_h ? surf->buf_h : surf->height;
	if (!node->data || (uint64_t)node->data_len < (uint64_t)bw * bh * 4)
		return;
	vis_w = surf->width  < bw ? surf->width  : bw;
	vis_h = surf->height < bh ? surf->height : bh;

	fbinfo = anx_fb_get_info();
	if (!fbinfo || !fbinfo->available)
		return;

	src = (const uint32_t *)node->data;

	/* Clip to damage rect when available; fall back to full surface. */
	if (surf->damage_valid && surf->damage_w && surf->damage_h) {
		int32_t dr0 = surf->damage_y;
		int32_t dr1 = dr0 + (int32_t)surf->damage_h;
		int32_t dc0 = surf->damage_x;
		int32_t dc1 = dc0 + (int32_t)surf->damage_w;
		if (dr0 < 0) dr0 = 0;
		if (dc0 < 0) dc0 = 0;
		if (dr1 > (int32_t)vis_h) dr1 = (int32_t)vis_h;
		if (dc1 > (int32_t)vis_w) dc1 = (int32_t)vis_w;
		if (dr1 <= dr0 || dc1 <= dc0)
			return;
		r0 = (uint32_t)dr0;  r1 = (uint32_t)dr1;
		c0 = (uint32_t)dc0;  c1 = (uint32_t)dc1;
	} else {
		r0 = 0;  r1 = vis_h;
		c0 = 0;  c1 = vis_w;
	}

	/* Compute framebuffer x offset and clip to framebuffer width. */
	if (surf->x < 0 || surf->y < 0)
		return;

	/* A full repaint of a grown window also paints the part with no content. */
	if (!surf->damage_valid) {
		if (surf->width > bw)
			anx_fb_fill_rect((uint32_t)surf->x + bw, (uint32_t)surf->y,
					 surf->width - bw, surf->height,
					 ANX_COLOR_AX_BG);
		if (surf->height > bh)
			anx_fb_fill_rect((uint32_t)surf->x,
					 (uint32_t)surf->y + bh,
					 vis_w, surf->height - bh,
					 ANX_COLOR_AX_BG);
	}

	/*
	 * Transparency. A panel (the menu bar, the task bar) sits on the
	 * desktop, so the desktop under it is repainted and the canvas is
	 * blended over it; without that the blend would darken a little
	 * more on every commit. Windows are only blended during a full
	 * repaint, when what is under them is freshly drawn.
	 */
	{
		const struct anx_theme *th = anx_theme_get();
		bool panel = surf->no_focus;

		blend_alpha = 255;
		if (panel && th->deco.bar_opacity < 255) {
			blend_alpha = th->deco.bar_opacity;
			anx_wm_desktop_paint((uint32_t)surf->x,
					     (uint32_t)surf->y,
					     surf->width, surf->height);
		} else if (!panel && th->deco.transparency_enabled &&
			   th->deco.window_opacity < 255 &&
			   anx_wm_in_repaint()) {
			blend_alpha = th->deco.window_opacity;
		}
	}

	fb_x0  = (uint32_t)surf->x + c0;
	copy_w = c1 - c0;
	if (fb_x0 >= fbinfo->width)
		return;
	if (fb_x0 + copy_w > fbinfo->width)
		copy_w = fbinfo->width - fb_x0;

	anx_fb_mark_dirty(fb_x0, (uint32_t)surf->y + r0, copy_w, r1 - r0);

	/* Row-at-a-time blit using 32-bit pixel writes (avoids 64-bit MMIO issues). */
	for (row = r0; row < r1; row++) {
		uint32_t       *dst_row;
		const uint32_t *src_row;
		uint32_t        col;

		dst_y = (uint32_t)surf->y + row;
		if (dst_y >= fbinfo->height)
			break;
		dst_row = anx_fb_row_ptr(dst_y) + fb_x0;
		src_row = src + row * bw + c0;
		if (blend_alpha != 255 && !clip_row(dst_y, &clip_x0, &clip_x1)) {
			anx_fb_blend_row(fb_x0, dst_y, copy_w, src_row,
					 blend_alpha);
			continue;
		}
		if (clip_row(dst_y, &clip_x0, &clip_x1)) {
			for (col = 0; col < copy_w; col++) {
				uint32_t px = fb_x0 + col;

				if (px >= clip_x0 && px < clip_x1)
					dst_row[col] = src_row[col];
			}
			continue;
		}
		for (col = 0; col < copy_w; col++)
			dst_row[col] = src_row[col];
	}
}

static void
render_text(struct anx_surface *surf, struct anx_content_node *node)
{
	const char *text;

	/* TEXT nodes carry their content in data (NUL-terminated string) */
	if (node->data && node->data_len > 0)
		text = (const char *)node->data;
	else if (node->label[0])
		text = node->label;
	else
		return;

	/* Background fill */
	anx_fb_fill_rect((uint32_t)surf->x, (uint32_t)surf->y,
	                  surf->width, surf->height, ANX_COLOR_MIDNIGHT);

	anx_gui_draw_string_scaled((uint32_t)surf->x, (uint32_t)surf->y,
	                            text, ANX_COLOR_WHITE, ANX_COLOR_MIDNIGHT, 1);
}

static void
render_button(struct anx_surface *surf, struct anx_content_node *node)
{
	uint32_t bx, by, bw, bh;

	bx = (uint32_t)surf->x;
	by = (uint32_t)surf->y;
	bw = surf->width;
	bh = surf->height;

	/* Outer border */
	anx_fb_fill_rect(bx, by, bw, bh, ANX_COLOR_WHITE);
	/* Inner fill */
	anx_fb_fill_rect(bx + BUTTON_BORDER, by + BUTTON_BORDER,
	                  bw - 2 * BUTTON_BORDER, bh - 2 * BUTTON_BORDER,
	                  ANX_COLOR_MIDNIGHT);

	/* Centered label */
	if (node->label[0]) {
		uint32_t text_w = (uint32_t)anx_strlen(node->label) * ANX_FONT_WIDTH;
		uint32_t tx     = bx + (bw > text_w ? (bw - text_w) / 2 : BUTTON_PAD_X);
		uint32_t ty     = by + BUTTON_PAD_Y + BUTTON_BORDER;

		anx_gui_draw_string_scaled(tx, ty, node->label,
		                            ANX_COLOR_WHITE, ANX_COLOR_MIDNIGHT, 1);
	}
}

static void
render_node(struct anx_surface *surf, struct anx_content_node *node)
{
	uint32_t i;

	if (!node)
		return;

	switch (node->type) {
	case ANX_CONTENT_CANVAS:
		render_canvas(surf, node);
		break;
	case ANX_CONTENT_TEXT:
		render_text(surf, node);
		break;
	case ANX_CONTENT_BUTTON:
		render_button(surf, node);
		break;
	default:
		/* FORM, VIEWPORT, VOID, and streaming types are not yet rendered */
		break;
	}

	/* Recurse into children */
	for (i = 0; i < node->child_count; i++)
		render_node(surf, &node->children[i]);
}

/* ------------------------------------------------------------------ */
/* Renderer ops                                                         */
/* ------------------------------------------------------------------ */

static int
gpu_map(struct anx_surface *surf)
{
	if (!anx_fb_available())
		return ANX_EIO;
	(void)surf;
	return ANX_OK;
}

/*
 * The frame a window paints: its title bar, border ring and the shadow
 * around them, in screen coordinates.
 */
struct frame_rect {
	int32_t  x, y;
	uint32_t w, h;
	uint32_t title_h;	/* 0 when the window has no title bar */
	uint32_t border;
};

static void frame_of(const struct anx_surface *surf, struct frame_rect *f)
{
	const struct anx_theme *theme = anx_theme_get();
	bool titled = surf->title[0] && surf->y >= (int32_t)ANX_WM_DECOR_H;

	f->border  = surf->title[0] ? anx_wm_tiling.border_w : 0;
	f->title_h = titled ? ANX_WM_DECOR_H : 0;
	f->x = surf->x - (int32_t)f->border;
	f->y = surf->y - (int32_t)(f->title_h + f->border);
	f->w = surf->width + 2 * f->border;
	f->h = surf->height + f->title_h + 2 * f->border;
	(void)theme;
}

/*
 * The shadow is translucent, so it may only be laid on freshly painted
 * background: during a full repaint the WM paints the desktop and the
 * windows below this one first. Outside that pass whatever shadow is
 * already on screen stays, which keeps it from darkening on every
 * commit.
 */
static void draw_shadow(const struct frame_rect *f,
			const struct anx_shape *shape)
{
	const struct anx_theme *theme = anx_theme_get();

	if (!theme->deco.shadow_enabled || !anx_wm_in_repaint())
		return;
	if (f->x < 0 || f->y < 0)
		return;

	anx_fb_shadow_shape((uint32_t)f->x, (uint32_t)f->y, f->w, f->h, shape,
			    (int32_t)theme->deco.shadow_offset_x,
			    (int32_t)theme->deco.shadow_offset_y,
			    theme->deco.shadow_blur,
			    theme->palette.shadow & 0x00FFFFFFu, 190);
}

/* Caption glyphs use the titlebar's shape and screen bounds. */
static void chrome_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			     uint32_t color)
{
	const struct anx_fb_info *fb = anx_fb_get_info();
	uint32_t row;

	if (x >= fb->width || y >= fb->height)
		return;
	if (w > fb->width - x) w = fb->width - x;
	if (h > fb->height - y) h = fb->height - y;
	for (row = y; row < y + h; row++) {
		uint32_t x0 = x, x1 = x + w, left, right;

		if (clip_row(row, &left, &right)) {
			if (x0 < left) x0 = left;
			if (x1 > right) x1 = right;
		}
		if (x1 > x0)
			anx_fb_fill_rect(x0, row, x1 - x0, 1, color);
	}
}

static void draw_caption_button(uint32_t tx, uint32_t ty,
				const struct anx_chrome_rect *r,
				enum anx_window_button button,
				uint32_t background, uint32_t glyph)
{
	uint32_t x = tx + r->x, y = ty + r->y, i;

	if (!r->w || !r->h)
		return;
	chrome_fill_rect(x, y, r->w, r->h, background);
	if (r->w < 12 || r->h < 12)
		return;
	x += (r->w - 10) / 2;
	y += (r->h - 10) / 2;
	if (button == ANX_WINDOW_BUTTON_MINIMIZE) {
		chrome_fill_rect(x, y + 5, 10, 1, glyph);
	} else if (button == ANX_WINDOW_BUTTON_MAXIMIZE) {
		chrome_fill_rect(x, y, 10, 1, glyph);
		chrome_fill_rect(x, y + 9, 10, 1, glyph);
		chrome_fill_rect(x, y, 1, 10, glyph);
		chrome_fill_rect(x + 9, y, 1, 10, glyph);
	} else {
		for (i = 0; i < 10; i++) {
			chrome_fill_rect(x + i, y + i, 1, 1, glyph);
			chrome_fill_rect(x + i, y + 9 - i, 1, 1, glyph);
		}
	}
}

static void draw_signature_button(uint32_t tx, uint32_t ty,
				  const struct anx_chrome_rect *r,
				  bool close, uint32_t color, uint32_t glyph)
{
	uint32_t x = tx + r->x, y = ty + r->y, i;

	if (!r->w || !r->h)
		return;
	anx_fb_fill_rounded_rect(x, y, r->w, r->h, r->w / 2, color);
	if (r->w < 14 || r->h < 14)
		return;
	anx_fb_blend_rect(x + 2, y + 2, 5, 4, 0x00FFFFFF, 90);
	if (close)
		for (i = 0; i < 6; i++) {
			chrome_fill_rect(x + 4 + i, y + 4 + i, 2, 1, glyph);
			chrome_fill_rect(x + 4 + i, y + 9 - i, 2, 1, glyph);
		}
}

static int
gpu_commit(struct anx_surface *surf)
{
	const struct anx_theme *theme = anx_theme_get();
	struct anx_shape shape;
	struct frame_rect f;
	bool decorated;

	if (!anx_fb_available())
		return ANX_EIO;

	frame_of(surf, &f);
	decorated = surf->title[0] != '\0';
	shape = decorated ? anx_theme_window_shape(0)
			  : anx_fb_shape_uniform(0, ANX_CORNER_SQUARE);

	/* Everything below paints the surface, its title bar and border. */
	{
		int32_t reach = (int32_t)anx_wm_shadow_reach();

		anx_wm_cursor_hide_rect(f.x - reach, f.y - reach,
					f.w + 2 * (uint32_t)reach,
					f.h + 2 * (uint32_t)reach);
	}

	if (decorated)
		draw_shadow(&f, &shape);

	render_node(surf, surf->content_root);

	/* Untitled surfaces and surfaces flush with the top have no caption. */
	if (f.title_h) {
		anx_oid_t foc = anx_input_focus_get();
		bool is_foc = foc.hi == surf->oid.hi && foc.lo == surf->oid.lo;
		bool windows = theme->deco.controls == ANX_CONTROLS_WINDOWS;
		uint32_t tfg = is_foc ? theme->palette.text_primary : theme->palette.text_dim;
		uint32_t tx = (uint32_t)surf->x;
		uint32_t ty = (uint32_t)(surf->y - (int32_t)f.title_h);
		uint32_t fy = ty + (f.title_h - ANX_FONT_HEIGHT) / 2;
		uint32_t bg_from = is_foc ? theme->palette.title_from : theme->palette.title_idle;
		uint32_t bg_to = is_foc ? theme->palette.title_to : theme->palette.title_idle;
		uint32_t colors[] = { theme->palette.btn_close, theme->palette.btn_min,
				      theme->palette.btn_max };
		struct anx_shape top = shape;
		struct anx_window_chrome chrome;
		char title[sizeof(surf->title)];
		uint32_t i, max_chars;

		top.corner[2] = ANX_CORNER_SQUARE;
		top.corner[3] = ANX_CORNER_SQUARE;
		if (top.radius > surf->width / 2) top.radius = surf->width / 2;
		if (top.radius > f.title_h / 2) top.radius = f.title_h / 2;
		anx_window_chrome_layout(theme->deco.controls, surf->width, f.title_h, &chrome);
		anx_fb_fill_shape_gradient(tx, ty, surf->width, f.title_h,
					   &top, bg_from, bg_to, true);
		g_clip.on = true;
		g_clip.x = surf->x;
		g_clip.y = (int32_t)ty;
		g_clip.w = surf->width;
		g_clip.h = f.title_h;
		g_clip.shape = top;
		if (!windows) {
			/* Keep the signature theme's light bevel and shaded seam. */
			anx_fb_blend_rect(tx + top.radius, ty, surf->width - 2 * top.radius,
					  1, 0x00FFFFFF, is_foc ? 40 : 20);
			anx_fb_blend_rect(tx, ty + f.title_h - 1, surf->width, 1,
					  0x00000000, is_foc ? 90 : 50);
		}
		for (i = 0; i < 3; i++) {
			if (windows)
				draw_caption_button(tx, ty, &chrome.draw[i],
					(enum anx_window_button)(ANX_WINDOW_BUTTON_CLOSE + i),
					is_foc ? colors[i] : theme->palette.title_idle,
					is_foc ? theme->palette.btn_glyph : theme->palette.text_dim);
			else
				draw_signature_button(tx, ty, &chrome.draw[i], i == 0,
						      colors[i], theme->palette.btn_glyph);
		}
		if (windows)
			chrome_fill_rect(tx, ty + f.title_h - 1, surf->width, 1,
					 is_foc ? theme->palette.accent : theme->palette.border);
		g_clip.on = false;
		/* A long title must stop before controls and the far window edge. */
		max_chars = chrome.title_width / ANX_FONT_WIDTH;
		if (max_chars >= sizeof(title)) max_chars = sizeof(title) - 1;
		for (i = 0; i < max_chars && surf->title[i]; i++)
			title[i] = surf->title[i];
		title[i] = '\0';
		if (i)
			anx_gui_draw_string_scaled(tx + chrome.title_x, fy,
						   title, tfg, bg_from, 1);
	}

	return ANX_OK;
}

static void
gpu_damage(struct anx_surface *surf,
           int32_t x, int32_t y, uint32_t w, uint32_t h)
{
	/* Hint only — full commit on next gpu_commit call. */
	(void)surf; (void)x; (void)y; (void)w; (void)h;
}

/* Clearing on unmap has to cover the shadow too, or it stays behind. */
static void
gpu_unmap(struct anx_surface *surf)
{
	/* Repaint the desktop over the frame and everything it cast */
	if (anx_fb_available() && surf->width && surf->height) {
		struct frame_rect f;
		int32_t reach = (int32_t)anx_wm_shadow_reach();
		int32_t x, y;

		frame_of(surf, &f);
		x = f.x - reach;
		y = f.y - reach;
		anx_wm_cursor_hide_rect(x, y, f.w + 2 * (uint32_t)reach,
					f.h + 2 * (uint32_t)reach);
		if (x < 0)
			x = 0;
		if (y < 0)
			y = 0;
		anx_wm_desktop_paint((uint32_t)x, (uint32_t)y,
				     f.w + 2 * (uint32_t)reach,
				     f.h + 2 * (uint32_t)reach);
	}
}

static const struct anx_renderer_ops gpu_ops = {
	.map    = gpu_map,
	.commit = gpu_commit,
	.damage = gpu_damage,
	.unmap  = gpu_unmap,
};

/* ------------------------------------------------------------------ */
/* Registration                                                         */
/* ------------------------------------------------------------------ */

int
anx_renderer_gpu_register(void)
{
	return anx_iface_renderer_register(ANX_ENGINE_RENDERER_GPU,
	                                    &gpu_ops, "gpu-framebuffer");
}
