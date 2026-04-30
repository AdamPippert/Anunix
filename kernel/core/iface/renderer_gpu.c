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
#include <anx/gui.h>
#include <anx/font.h>
#include <anx/theme.h>
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

static void
render_canvas(struct anx_surface *surf, struct anx_content_node *node)
{
	const struct anx_fb_info *fbinfo;
	const uint32_t *src;
	uint32_t        row;
	uint32_t        dst_y;
	uint32_t        r0, r1, c0, c1;
	uint32_t        fb_x0, copy_w;

	if (!node->data || node->data_len < surf->width * surf->height * 4)
		return;

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
		if (dr1 > (int32_t)surf->height) dr1 = (int32_t)surf->height;
		if (dc1 > (int32_t)surf->width)  dc1 = (int32_t)surf->width;
		if (dr1 <= dr0 || dc1 <= dc0)
			return;
		r0 = (uint32_t)dr0;  r1 = (uint32_t)dr1;
		c0 = (uint32_t)dc0;  c1 = (uint32_t)dc1;
	} else {
		r0 = 0;  r1 = surf->height;
		c0 = 0;  c1 = surf->width;
	}

	/* Compute framebuffer x offset and clip to framebuffer width. */
	if (surf->x < 0 || surf->y < 0)
		return;
	fb_x0  = (uint32_t)surf->x + c0;
	copy_w = c1 - c0;
	if (fb_x0 >= fbinfo->width)
		return;
	if (fb_x0 + copy_w > fbinfo->width)
		copy_w = fbinfo->width - fb_x0;

	/* Row-at-a-time blit using 32-bit pixel writes (avoids 64-bit MMIO issues). */
	for (row = r0; row < r1; row++) {
		uint32_t       *dst_row;
		const uint32_t *src_row;
		uint32_t        col;

		dst_y = (uint32_t)surf->y + row;
		if (dst_y >= fbinfo->height)
			break;
		dst_row = anx_fb_row_ptr(dst_y) + fb_x0;
		src_row = src + row * surf->width + c0;
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

static int
gpu_commit(struct anx_surface *surf)
{
	if (!anx_fb_available())
		return ANX_EIO;

	render_node(surf, surf->content_root);

	/* Window decoration: thin titlebar above the surface canvas.
	 * Skipped for untitled surfaces and surfaces flush with the top. */
	if (surf->title[0] && surf->y >= (int32_t)ANX_WM_DECOR_H) {
		const struct anx_theme *theme = anx_theme_get();
		anx_oid_t foc    = anx_input_focus_get();
		bool      is_foc = (foc.hi == surf->oid.hi && foc.lo == surf->oid.lo);
		uint32_t  tbg    = is_foc ? theme->palette.accent : theme->palette.surface;
		uint32_t  tfg    = is_foc ? 0x00FFFFFFu : theme->palette.text_dim;
		uint32_t tx  = (uint32_t)surf->x;
		uint32_t ty  = (uint32_t)(surf->y - (int32_t)ANX_WM_DECOR_H);
		uint32_t fy  = ty + (ANX_WM_DECOR_H - ANX_FONT_HEIGHT) / 2;
		uint32_t btn = ANX_WM_DECOR_H - 4;		/* button side */
		uint32_t bx  = tx + surf->width - btn - 2;	/* close-btn x */
		uint32_t mx  = bx - btn - 3;			/* maximize-btn x */
		uint32_t by  = ty + 2;				/* buttons y */

		anx_fb_fill_rect(tx, ty, surf->width, ANX_WM_DECOR_H, tbg);
		anx_fb_fill_rect(tx, ty + ANX_WM_DECOR_H - 2, surf->width, 2,
				 theme->palette.accent);
		anx_gui_draw_string_scaled(tx + 4, fy, surf->title, tfg, tbg, 1);

		/* Maximize button: accent colour, "+" label */
		anx_fb_fill_rect(mx, by, btn, btn, theme->palette.accent);
		anx_gui_draw_string_scaled(mx + (btn - ANX_FONT_WIDTH) / 2,
					   by + (btn - ANX_FONT_HEIGHT) / 2,
					   "+", 0x00FFFFFFu,
					   theme->palette.accent, 1);

		/* Close button: filled square in error colour, "x" label */
		anx_fb_fill_rect(bx, by, btn, btn, theme->palette.error);
		anx_gui_draw_string_scaled(bx + (btn - ANX_FONT_WIDTH) / 2,
					   by + (btn - ANX_FONT_HEIGHT) / 2,
					   "x", 0x00FFFFFFu,
					   theme->palette.error, 1);
	}

	/* Draw 2px border around focused window canvas */
	if (surf->title[0] && surf->width && surf->height) {
		const struct anx_theme *theme2 = anx_theme_get();
		anx_oid_t foc2    = anx_input_focus_get();
		bool      is_foc2 = (foc2.hi == surf->oid.hi &&
				     foc2.lo == surf->oid.lo);
		uint32_t  border_col = is_foc2 ? theme2->palette.accent
					       : theme2->palette.border;
		uint32_t  bx = (uint32_t)surf->x;
		uint32_t  by = (uint32_t)surf->y;

		/* Left + Right 2px strips */
		anx_fb_fill_rect(bx, by, 2, surf->height, border_col);
		anx_fb_fill_rect(bx + surf->width - 2, by,
				 2, surf->height, border_col);
		/* Bottom 2px strip (top is covered by titlebar) */
		anx_fb_fill_rect(bx, by + surf->height - 2,
				 surf->width, 2, border_col);
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

static void
gpu_unmap(struct anx_surface *surf)
{
	/* Clear the surface region to background colour */
	if (anx_fb_available() && surf->width && surf->height)
		anx_fb_fill_rect((uint32_t)surf->x, (uint32_t)surf->y,
		                  surf->width, surf->height,
		                  ANX_COLOR_SKY_BLUE);
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
