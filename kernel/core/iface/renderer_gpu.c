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

/* ------------------------------------------------------------------ */
/* Content rendering helpers                                            */
/* ------------------------------------------------------------------ */

#define BUTTON_PAD_X   8u
#define BUTTON_PAD_Y   4u
#define BUTTON_BORDER  2u

static void
render_canvas(struct anx_surface *surf, struct anx_content_node *node)
{
	const uint32_t *src;
	uint32_t        row, col;
	uint32_t        dst_x, dst_y;
	uint32_t        r0, r1, c0, c1;

	if (!node->data || node->data_len < surf->width * surf->height * 4)
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

	for (row = r0; row < r1; row++) {
		dst_y = (uint32_t)surf->y + row;
		for (col = c0; col < c1; col++) {
			dst_x = (uint32_t)surf->x + col;
			anx_fb_putpixel(dst_x, dst_y,
			                src[row * surf->width + col]);
		}
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
		uint32_t tx = (uint32_t)surf->x;
		uint32_t ty = (uint32_t)(surf->y - (int32_t)ANX_WM_DECOR_H);
		uint32_t fy = ty + (ANX_WM_DECOR_H - ANX_FONT_HEIGHT) / 2;

		anx_fb_fill_rect(tx, ty, surf->width, ANX_WM_DECOR_H,
				 theme->palette.surface);
		anx_fb_fill_rect(tx, ty + ANX_WM_DECOR_H - 2, surf->width, 2,
				 theme->palette.accent);
		anx_gui_draw_string_scaled(tx + 4, fy, surf->title, 1,
					   theme->palette.text_primary,
					   theme->palette.surface);
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
