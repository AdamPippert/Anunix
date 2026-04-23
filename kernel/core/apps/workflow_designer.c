/*
 * workflow_designer.c — Visual workflow editor.
 *
 * A surface-based application for composing, editing, and running
 * Anunix workflows — analogous to a Marimo notebook for programs.
 *
 * Layout:
 *   ┌─────────────────────────────────────────────────┐
 *   │ TOOLBAR  wf-name │ [New][Open][Save][Run][+N][+E]│
 *   ├─────────────────────────────────────────────────┤
 *   │                                                  │
 *   │   [trigger]──→[model]──→[output]                 │
 *   │       ↑ selected (teal border)                   │
 *   │                                                  │
 *   ├─────────────────────────────────────────────────┤
 *   │ Properties: kind: model  prompt: "..."           │
 *   └─────────────────────────────────────────────────┘
 *
 * Keyboard navigation:
 *   Tab / Shift+Tab   — cycle selected node
 *   Arrow keys        — scroll viewport
 *   Enter             — edit selected node label
 *   Meta+R            — run current workflow
 *   Meta+S            — serialize + print DSL to kprintf
 *   Meta+N            — add node (cycles through kinds)
 *   Meta+E            — add edge from selected to next node
 *   Escape            — close designer
 */

#include <anx/types.h>
#include <anx/wm.h>
#include <anx/workflow.h>
#include <anx/workflow_library.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/fb.h>
#include <anx/gui.h>
#include <anx/theme.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define WD_TOOLBAR_H	36
#define WD_PROPS_H	40
#define WD_NODE_W	120
#define WD_NODE_H	40
#define WD_NODE_GAP_X	40
#define WD_NODE_GAP_Y	60
#define WD_NODES_PER_ROW 5

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct {
	struct anx_surface *surf;
	uint32_t           *pixels;
	uint32_t            pix_w;
	uint32_t            pix_h;

	anx_oid_t           wf_oid;
	bool                loaded;

	int32_t             selected;	/* node slot index, -1 = none */
	int32_t             scroll_x;
	int32_t             scroll_y;

	/* For edge-add mode: source node is 'edge_from', pick target next */
	bool                edge_mode;
	int32_t             edge_from;

	/* Light edit: typing new label into selected node */
	bool                label_edit;
	char                label_buf[ANX_WF_LABEL_MAX];
	uint32_t            label_len;
} g_wd;

/* ------------------------------------------------------------------ */
/* Pixel drawing into the designer surface                             */
/* ------------------------------------------------------------------ */

static void wd_fill(uint32_t x, uint32_t y,
		    uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row, col;

	for (row = y; row < y + h && row < g_wd.pix_h; row++)
		for (col = x; col < x + w && col < g_wd.pix_w; col++)
			g_wd.pixels[row * g_wd.pix_w + col] = color;
}

static void wd_rect_border(uint32_t x, uint32_t y,
			   uint32_t w, uint32_t h,
			   uint32_t border, uint32_t fill)
{
	wd_fill(x, y, w, h, fill);
	wd_fill(x, y, w, 2, border);
	wd_fill(x, y + h - 2, w, 2, border);
	wd_fill(x, y, 2, h, border);
	wd_fill(x + w - 2, y, 2, h, border);
}

static void wd_text(uint32_t x, uint32_t y, const char *s, uint32_t fg)
{
	/* Draw into the framebuffer at the surface's screen position */
	uint32_t sx = (uint32_t)g_wd.surf->x + x;
	uint32_t sy = (uint32_t)g_wd.surf->y + y;

	anx_gui_draw_string_scaled(sx, sy, s, fg, 0, 1);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static const char *kind_short(enum anx_wf_node_kind k)
{
	switch (k) {
	case ANX_WF_NODE_TRIGGER:	return "trigger";
	case ANX_WF_NODE_MODEL_CALL:	return "model";
	case ANX_WF_NODE_AGENT_CALL:	return "agent";
	case ANX_WF_NODE_RETRIEVAL:	return "retrieval";
	case ANX_WF_NODE_CELL_CALL:	return "cell";
	case ANX_WF_NODE_CONDITION:	return "condition";
	case ANX_WF_NODE_FAN_OUT:	return "fan-out";
	case ANX_WF_NODE_FAN_IN:	return "fan-in";
	case ANX_WF_NODE_TRANSFORM:	return "transform";
	case ANX_WF_NODE_HUMAN_REVIEW:	return "review";
	case ANX_WF_NODE_SUBFLOW:	return "subflow";
	case ANX_WF_NODE_OUTPUT:	return "output";
	default:			return "?";
	}
}

static void wd_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg     = theme->palette.background;
	uint32_t panel  = theme->palette.surface;
	uint32_t accent = theme->palette.accent;
	uint32_t text   = theme->palette.text_primary;
	uint32_t dim    = theme->palette.text_dim;
	struct anx_wf_object *wf = NULL;
	uint32_t i;
	uint32_t canvas_y = WD_TOOLBAR_H;
	uint32_t props_y  = g_wd.pix_h - WD_PROPS_H;

	/* Background */
	wd_fill(0, 0, g_wd.pix_w, g_wd.pix_h, bg);

	/* Toolbar */
	wd_fill(0, 0, g_wd.pix_w, WD_TOOLBAR_H, panel);
	wd_fill(0, WD_TOOLBAR_H - 1, g_wd.pix_w, 1, accent);
	wd_text(10, 10, g_wd.loaded ? "workflow:" : "no workflow", dim);
	if (g_wd.loaded) {
		wf = anx_wf_object_get(&g_wd.wf_oid);
		if (wf)
			wd_text(90, 10, wf->name, text);
	}

	/* Toolbar buttons — abbreviated labels */
	{
		const char *labels[] = {"[New]","[Open]","[Save]","[Run]","[+N]","[+E]"};
		uint32_t bx = g_wd.pix_w - 240;
		uint32_t li;

		for (li = 0; li < 6; li++) {
			wd_text(bx, 10, labels[li], accent);
			bx += 40;
		}
	}

	/* Graph area */
	wd_fill(0, canvas_y, g_wd.pix_w, props_y - canvas_y, bg);

	if (!g_wd.loaded || !wf) {
		wd_text(g_wd.pix_w / 2 - 60, canvas_y + 60,
			"Meta+N: new node  Meta+O: open", dim);
		goto props_area;
	}

	/* Draw nodes */
	for (i = 0; i < ANX_WF_MAX_NODES; i++) {
		const struct anx_wf_node *n = &wf->nodes[i];
		char label[ANX_WF_LABEL_MAX + 16];
		uint32_t col, row;
		uint32_t nx, ny;
		uint32_t border;
		bool selected = ((int32_t)i == g_wd.selected);

		if (n->id == 0)
			continue;

		col = (n->id - 1) % WD_NODES_PER_ROW;
		row = (n->id - 1) / WD_NODES_PER_ROW;
		nx  = 20 + col * (WD_NODE_W + WD_NODE_GAP_X) +
		      (uint32_t)(-g_wd.scroll_x);
		ny  = canvas_y + 20 + row * (WD_NODE_H + WD_NODE_GAP_Y) +
		      (uint32_t)(-g_wd.scroll_y);

		border = selected ? accent : dim;
		wd_rect_border(nx, ny, WD_NODE_W, WD_NODE_H, border, panel);

		anx_snprintf(label, sizeof(label), "%s", kind_short(n->kind));
		wd_text(nx + 6, ny + 4, label, dim);
		anx_snprintf(label, sizeof(label), "%s",
			     n->label[0] ? n->label : "(unnamed)");
		wd_text(nx + 6, ny + 20, label, text);
	}

	/* Draw edges as simple arrows between node boxes */
	for (i = 0; i < ANX_WF_MAX_EDGES; i++) {
		const struct anx_wf_edge *e = &wf->edges[i];
		const struct anx_wf_node *fn, *tn;
		uint32_t fx, fy, tx, ty;
		uint32_t fc, fr, tc, tr;

		if (e->from_node == 0 && e->to_node == 0)
			continue;
		if (e->from_node > ANX_WF_MAX_NODES ||
		    e->to_node   > ANX_WF_MAX_NODES)
			continue;

		fn = &wf->nodes[e->from_node - 1];
		tn = &wf->nodes[e->to_node   - 1];
		if (fn->id == 0 || tn->id == 0)
			continue;

		fc  = (fn->id - 1) % WD_NODES_PER_ROW;
		fr  = (fn->id - 1) / WD_NODES_PER_ROW;
		tc  = (tn->id - 1) % WD_NODES_PER_ROW;
		tr  = (tn->id - 1) / WD_NODES_PER_ROW;

		fx = 20 + fc * (WD_NODE_W + WD_NODE_GAP_X) + WD_NODE_W;
		fy = canvas_y + 20 + fr * (WD_NODE_H + WD_NODE_GAP_Y) +
		     WD_NODE_H / 2;
		tx = 20 + tc * (WD_NODE_W + WD_NODE_GAP_X);
		ty = canvas_y + 20 + tr * (WD_NODE_H + WD_NODE_GAP_Y) +
		     WD_NODE_H / 2;

		/* Draw a horizontal dashed line as the edge */
		{
			uint32_t ex;
			uint32_t ey = (fy + ty) / 2;

			for (ex = (fx < tx ? fx : tx);
			     ex < (fx > tx ? fx : tx);
			     ex += 2) {
				if (ex < g_wd.pix_w && ey < g_wd.pix_h)
					g_wd.pixels[ey * g_wd.pix_w + ex] = accent;
			}
		}
	}

props_area:
	/* Properties bar */
	wd_fill(0, props_y, g_wd.pix_w, WD_PROPS_H, panel);
	wd_fill(0, props_y, g_wd.pix_w, 1, accent);

	if (g_wd.loaded && wf && g_wd.selected >= 0) {
		const struct anx_wf_node *n = &wf->nodes[g_wd.selected];

		if (n->id != 0) {
			char info[128];
			anx_snprintf(info, sizeof(info),
				     "node %u  kind: %-12s  label: %s",
				     (uint32_t)n->id,
				     kind_short(n->kind),
				     n->label[0] ? n->label : "(none)");
			wd_text(10, props_y + 12, info, text);
		}
	} else {
		wd_text(10, props_y + 12,
			"Tab: select node  Meta+N: add  Meta+E: edge  Meta+R: run  Esc: close",
			dim);
	}

	/* Commit to screen */
	anx_iface_surface_commit(g_wd.surf);
}

/* ------------------------------------------------------------------ */
/* Input handling                                                      */
/* ------------------------------------------------------------------ */

static void wd_select_next(int delta)
{
	struct anx_wf_object *wf;
	int32_t i, next;

	if (!g_wd.loaded)
		return;
	wf = anx_wf_object_get(&g_wd.wf_oid);
	if (!wf || wf->node_count == 0) {
		g_wd.selected = -1;
		return;
	}

	next = g_wd.selected + delta;
	/* Wrap and skip empty slots */
	for (i = 0; i < (int32_t)ANX_WF_MAX_NODES; i++) {
		int32_t slot = ((next % (int32_t)ANX_WF_MAX_NODES) +
				(int32_t)ANX_WF_MAX_NODES) %
			       (int32_t)ANX_WF_MAX_NODES;
		if (wf->nodes[slot].id != 0) {
			g_wd.selected = slot;
			return;
		}
		next += delta;
	}
}

static enum anx_wf_node_kind g_next_kind;

void anx_wm_workflow_designer_key(uint32_t key, uint32_t mods)
{
	struct anx_wf_object *wf;

	switch (key) {
	case ANX_KEY_TAB:
		wd_select_next((mods & ANX_MOD_SHIFT) ? -1 : 1);
		wd_render();
		break;

	case ANX_KEY_ESC:
		anx_wm_window_close(g_wd.surf);
		g_wd.surf   = NULL;
		g_wd.loaded = false;
		return;

	case ANX_KEY_W:
		if (mods & ANX_MOD_META) {
			/* Serialize and log */
			if (g_wd.loaded) {
				char *buf = anx_alloc(8192);
				if (buf) {
					anx_wf_serialize(&g_wd.wf_oid, buf, 8192);
					kprintf("%s\n", buf);
					anx_free(buf);
				}
			}
		}
		break;

	case ANX_KEY_R:
		if ((mods & ANX_MOD_META) && g_wd.loaded) {
			kprintf("[workflow-designer] running workflow\n");
			anx_wf_run(&g_wd.wf_oid, NULL);
			wd_render();
		}
		break;

	case ANX_KEY_N:
		if ((mods & ANX_MOD_META) && g_wd.loaded) {
			struct anx_wf_node spec;
			uint16_t new_id = 0;

			anx_memset(&spec, 0, sizeof(spec));
			spec.kind = g_next_kind;
			anx_strlcpy(spec.label, "new", ANX_WF_LABEL_MAX);
			spec.canvas_x = 20;
			spec.canvas_y = 20;

			wf = anx_wf_object_get(&g_wd.wf_oid);
			if (wf) {
				spec.canvas_x = (int32_t)((uint32_t)wf->node_count *
						(WD_NODE_W + WD_NODE_GAP_X) + 20);
			}
			anx_wf_node_add(&g_wd.wf_oid, &spec, &new_id);

			/* Cycle kind for next add */
			g_next_kind = (g_next_kind + 1) %
				      ANX_WF_NODE_KIND_COUNT;
			g_wd.selected = (int32_t)(new_id - 1);
			wd_render();
		}
		break;

	case ANX_KEY_E:
		if ((mods & ANX_MOD_META) && g_wd.loaded &&
		    g_wd.selected >= 0) {
			if (!g_wd.edge_mode) {
				g_wd.edge_mode = true;
				g_wd.edge_from = g_wd.selected;
				kprintf("[workflow-designer] select target node\n");
			} else {
				/* Add edge from edge_from to selected */
				wf = anx_wf_object_get(&g_wd.wf_oid);
				if (wf && g_wd.selected != g_wd.edge_from) {
					uint16_t from = wf->nodes[g_wd.edge_from].id;
					uint16_t to   = wf->nodes[g_wd.selected].id;
					anx_wf_edge_add(&g_wd.wf_oid,
							from, 0, to, 0);
				}
				g_wd.edge_mode = false;
				wd_render();
			}
		}
		break;

	case ANX_KEY_UP:
		g_wd.scroll_y -= 20;
		if (g_wd.scroll_y < 0) g_wd.scroll_y = 0;
		wd_render();
		break;

	case ANX_KEY_DOWN:
		g_wd.scroll_y += 20;
		wd_render();
		break;

	case ANX_KEY_LEFT:
		g_wd.scroll_x -= 20;
		if (g_wd.scroll_x < 0) g_wd.scroll_x = 0;
		wd_render();
		break;

	case ANX_KEY_RIGHT:
		g_wd.scroll_x += 20;
		wd_render();
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Launch                                                              */
/* ------------------------------------------------------------------ */

void anx_wm_launch_workflow_designer(void)
{
	const struct anx_fb_info *fb;
	struct anx_content_node  *cn;
	uint32_t w, h, buf_size;

	if (g_wd.surf) {
		/* Already open — raise to front */
		anx_wm_window_focus(g_wd.surf);
		return;
	}

	fb = anx_fb_get_info();
	if (!fb || !fb->available) {
		kprintf("[workflow-designer] no framebuffer\n");
		return;
	}

	w = fb->width  * 3 / 4;
	h = fb->height * 3 / 4;

	buf_size     = w * h * 4;
	g_wd.pixels  = anx_alloc(buf_size);
	if (!g_wd.pixels)
		return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_wd.pixels);
		g_wd.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_wd.pixels;
	cn->data_len = buf_size;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     (int32_t)fb->width  / 8,
				     (int32_t)fb->height / 8,
				     w, h, &g_wd.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_wd.pixels);
		g_wd.pixels = NULL;
		return;
	}

	g_wd.pix_w    = w;
	g_wd.pix_h    = h;
	g_wd.selected = -1;
	g_wd.scroll_x = 0;
	g_wd.scroll_y = 0;
	g_wd.loaded   = false;
	g_wd.edge_mode = false;
	g_next_kind   = ANX_WF_NODE_TRIGGER;

	/* Open first registered workflow if any */
	{
		anx_oid_t wfs[4];
		uint32_t count = 0;

		if (anx_wf_list(wfs, 4, &count) == ANX_OK && count > 0) {
			g_wd.wf_oid = wfs[0];
			g_wd.loaded = true;
		}
	}

	anx_iface_surface_map(g_wd.surf);
	anx_wm_window_open(g_wd.surf);
	wd_render();
	kprintf("[workflow-designer] opened\n");
}
