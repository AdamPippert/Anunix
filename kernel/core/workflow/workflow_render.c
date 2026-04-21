/*
 * workflow_render.c — Render a workflow node graph onto a 32bpp pixel buffer.
 *
 * Draws background, dot grid, edges (Bresenham lines), and filled nodes with
 * borders and port indicators.  All coordinates are clipped to canvas bounds.
 * This is a Phase 1 renderer: text labels are rendered as white pixel dots
 * per character; proper text awaits Phase 2 GUI integration.
 */

#include <anx/types.h>
#include <anx/workflow.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/gui.h>

/* Node fill colors by kind */
#define COLOR_BG		0x00121212
#define COLOR_GRID		0x00222222
#define COLOR_EDGE		0x00445566
#define COLOR_BORDER		0x00888888
#define COLOR_PORT		0x00AAAAAA
#define COLOR_LABEL		0x00FFFFFF

#define COLOR_TRIGGER		0x00334422
#define COLOR_MODEL_AGENT	0x00223344
#define COLOR_COND_FAN		0x00443322
#define COLOR_HUMAN		0x00442233
#define COLOR_OUTPUT		0x00223333
#define COLOR_DEFAULT		0x00282828

#define NODE_DEFAULT_W		120
#define NODE_DEFAULT_H		48
#define PORT_HALF		1	/* half-size of the 3x3 port square */

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                       */
/* ------------------------------------------------------------------ */

/* Set a single pixel, guarded by canvas bounds. */
static inline void set_pixel(uint32_t *pixels, uint32_t cw, uint32_t ch,
			      int32_t x, int32_t y, uint32_t color)
{
	if ((uint32_t)x < cw && (uint32_t)y < ch)
		pixels[(uint32_t)y * cw + (uint32_t)x] = color;
}

/* Fill a rectangle, clipping to [0, cw) x [0, ch). */
static void fill_rect(uint32_t *pixels, uint32_t cw, uint32_t ch,
		      int32_t x, int32_t y, uint32_t w, uint32_t h,
		      uint32_t color)
{
	int32_t x0, y0, x1, y1, xi, yi;

	x0 = x < 0 ? 0 : x;
	y0 = y < 0 ? 0 : y;
	x1 = (int32_t)(x + (int32_t)w);
	y1 = (int32_t)(y + (int32_t)h);
	if (x1 > (int32_t)cw) x1 = (int32_t)cw;
	if (y1 > (int32_t)ch) y1 = (int32_t)ch;

	for (yi = y0; yi < y1; yi++)
		for (xi = x0; xi < x1; xi++)
			pixels[(uint32_t)yi * cw + (uint32_t)xi] = color;
}

/* Draw a 1-pixel border around a rectangle. */
static void draw_rect_border(uint32_t *pixels, uint32_t cw, uint32_t ch,
			     int32_t x, int32_t y, uint32_t w, uint32_t h,
			     uint32_t color)
{
	int32_t x1 = x + (int32_t)w - 1;
	int32_t y1 = y + (int32_t)h - 1;
	int32_t i;

	/* Top and bottom rows */
	for (i = x; i <= x1; i++) {
		set_pixel(pixels, cw, ch, i, y,  color);
		set_pixel(pixels, cw, ch, i, y1, color);
	}
	/* Left and right columns */
	for (i = y; i <= y1; i++) {
		set_pixel(pixels, cw, ch, x,  i, color);
		set_pixel(pixels, cw, ch, x1, i, color);
	}
}

/* Bresenham line between (x0,y0) and (x1,y1), clipped per-pixel. */
static void draw_line(uint32_t *pixels, uint32_t cw, uint32_t ch,
		      int32_t x0, int32_t y0, int32_t x1, int32_t y1,
		      uint32_t color)
{
	int32_t dx, dy, sx, sy, err, e2;

	dx = x1 - x0;
	if (dx < 0) dx = -dx;
	dy = y1 - y0;
	if (dy < 0) dy = -dy;

	sx = (x0 < x1) ? 1 : -1;
	sy = (y0 < y1) ? 1 : -1;
	err = dx - dy;

	for (;;) {
		set_pixel(pixels, cw, ch, x0, y0, color);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 <  dx) { err += dx; y0 += sy; }
	}
}

/* ------------------------------------------------------------------ */
/* Node color selection                                                */
/* ------------------------------------------------------------------ */

static uint32_t node_fill_color(enum anx_wf_node_kind kind)
{
	switch (kind) {
	case ANX_WF_NODE_TRIGGER:
		return COLOR_TRIGGER;
	case ANX_WF_NODE_MODEL_CALL:
	case ANX_WF_NODE_AGENT_CALL:
		return COLOR_MODEL_AGENT;
	case ANX_WF_NODE_CONDITION:
	case ANX_WF_NODE_FAN_OUT:
	case ANX_WF_NODE_FAN_IN:
		return COLOR_COND_FAN;
	case ANX_WF_NODE_HUMAN_REVIEW:
		return COLOR_HUMAN;
	case ANX_WF_NODE_OUTPUT:
		return COLOR_OUTPUT;
	default:
		return COLOR_DEFAULT;
	}
}

/* ------------------------------------------------------------------ */
/* Label rendering                                                     */
/* ------------------------------------------------------------------ */

/*
 * Draw the first 8 characters of the label as single white pixel dots.
 * Each character occupies a 3x6 block; the dot is placed at the top-left
 * of that block so there is visible spacing between characters.
 *
 * TODO Phase 2: integrate anx_gui_draw_string_scaled for proper text.
 */
static void draw_label(uint32_t *pixels, uint32_t cw, uint32_t ch,
		       int32_t nx, int32_t ny, uint32_t nw, uint32_t nh,
		       const char *label)
{
	int32_t x_pos, y_pos;
	uint32_t i, len;

	len = (uint32_t)anx_strlen(label);
	if (len > 8)
		len = 8;

	y_pos = ny + (int32_t)nh / 2 - 3;

	for (i = 0; i < len; i++) {
		x_pos = nx + 4 + (int32_t)(i * 7);

		/*
		 * Guard: only draw if the character dot falls within the
		 * node's horizontal extent so it doesn't spill into
		 * adjacent nodes.
		 */
		if (x_pos >= nx + (int32_t)nw - 4)
			break;

		/* 3x6 dot block per character */
		fill_rect(pixels, cw, ch, x_pos, y_pos, 3, 6, COLOR_LABEL);
	}
}

/* ------------------------------------------------------------------ */
/* Port dot rendering                                                  */
/* ------------------------------------------------------------------ */

/*
 * Draw 3x3 squares on the left (input) and right (output) edges of the
 * node, evenly spaced.  If the node declares no ports, we still draw one
 * dot on each side to indicate connectivity.
 */
static void draw_ports(uint32_t *pixels, uint32_t cw, uint32_t ch,
		       const struct anx_wf_node *node,
		       int32_t nx, int32_t ny, uint32_t nw, uint32_t nh)
{
	uint32_t i;
	uint32_t in_count = 0, out_count = 0;
	uint32_t in_idx = 0, out_idx = 0;
	int32_t px, py;

	/* Count input and output ports */
	for (i = 0; i < node->port_count && i < ANX_WF_MAX_PORTS; i++) {
		if (node->ports[i].dir == ANX_WF_PORT_IN)
			in_count++;
		else
			out_count++;
	}

	/* Ensure at least one dot per side for visual coherence */
	if (in_count == 0)  in_count  = 1;
	if (out_count == 0) out_count = 1;

	/* Draw input dots on left edge */
	for (i = 0; i < in_count; i++) {
		py = ny + (int32_t)((i + 1) * nh / (in_count + 1));
		px = nx - PORT_HALF;
		fill_rect(pixels, cw, ch,
			  px, py - PORT_HALF,
			  2 * PORT_HALF + 1, 2 * PORT_HALF + 1,
			  COLOR_PORT);
		in_idx++;
	}
	(void)in_idx;

	/* Draw output dots on right edge */
	for (i = 0; i < out_count; i++) {
		py = ny + (int32_t)((i + 1) * nh / (out_count + 1));
		px = nx + (int32_t)nw - PORT_HALF;
		fill_rect(pixels, cw, ch,
			  px, py - PORT_HALF,
			  2 * PORT_HALF + 1, 2 * PORT_HALF + 1,
			  COLOR_PORT);
		out_idx++;
	}
	(void)out_idx;
}

/* ------------------------------------------------------------------ */
/* Public render entry point                                           */
/* ------------------------------------------------------------------ */

/*
 * Render the workflow identified by wf_oid onto the 32bpp pixel buffer.
 * Returns ANX_OK on success, ANX_ENOENT if the workflow is not found.
 */
int anx_wf_render_canvas(const anx_oid_t *wf_oid, uint32_t *pixels,
			  uint32_t width, uint32_t height)
{
	struct anx_wf_object *wf;
	uint32_t x, y, i;

	if (!wf_oid || !pixels || width == 0 || height == 0)
		return ANX_EINVAL;

	wf = anx_wf_object_get(wf_oid);
	if (!wf)
		return ANX_ENOENT;

	/* ---- Background ---- */
	for (i = 0; i < width * height; i++)
		pixels[i] = COLOR_BG;

	/* ---- Dot grid: one dot every 32 pixels ---- */
	for (y = 0; y < height; y += 32) {
		for (x = 0; x < width; x += 32) {
			if (x < width && y < height)
				pixels[y * width + x] = COLOR_GRID;
		}
	}

	/* ---- Edges ---- */
	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		const struct anx_wf_edge *edge = &wf->edges[i];
		const struct anx_wf_node *from_node = NULL;
		const struct anx_wf_node *to_node   = NULL;
		uint32_t j;
		int32_t  fx, fy, tx, ty;
		uint32_t fw, fh, tw, th;

		if (edge->from_node == 0 || edge->to_node == 0)
			continue;

		/* Find from and to nodes by id */
		for (j = 0; j < ANX_WF_MAX_NODES; j++) {
			if (wf->nodes[j].id == edge->from_node)
				from_node = &wf->nodes[j];
			if (wf->nodes[j].id == edge->to_node)
				to_node = &wf->nodes[j];
		}

		if (!from_node || !to_node)
			continue;

		fw = from_node->canvas_w ? from_node->canvas_w : NODE_DEFAULT_W;
		fh = from_node->canvas_h ? from_node->canvas_h : NODE_DEFAULT_H;
		tw = to_node->canvas_w   ? to_node->canvas_w   : NODE_DEFAULT_W;
		th = to_node->canvas_h   ? to_node->canvas_h   : NODE_DEFAULT_H;

		/* Right edge mid-point of source, left edge mid-point of dest */
		fx = from_node->canvas_x + (int32_t)fw;
		fy = from_node->canvas_y + (int32_t)fh / 2;
		tx = to_node->canvas_x;
		ty = to_node->canvas_y + (int32_t)th / 2;

		(void)tw;

		draw_line(pixels, width, height, fx, fy, tx, ty, COLOR_EDGE);
	}

	/* ---- Nodes ---- */
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		const struct anx_wf_node *node = &wf->nodes[i];
		uint32_t nw, nh;
		int32_t  nx, ny;

		if (node->id == 0)
			continue;

		nx = node->canvas_x;
		ny = node->canvas_y;
		nw = node->canvas_w ? node->canvas_w : NODE_DEFAULT_W;
		nh = node->canvas_h ? node->canvas_h : NODE_DEFAULT_H;

		/* Filled body */
		fill_rect(pixels, width, height, nx, ny, nw, nh,
			  node_fill_color(node->kind));

		/* 1-pixel border */
		draw_rect_border(pixels, width, height, nx, ny, nw, nh,
				 COLOR_BORDER);

		/* Label dots */
		draw_label(pixels, width, height, nx, ny, nw, nh, node->label);

		/* Port indicators */
		draw_ports(pixels, width, height, node, nx, ny, nw, nh);
	}

	return ANX_OK;
}
