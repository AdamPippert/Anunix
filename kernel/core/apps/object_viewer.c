/*
 * object_viewer.c — Object store browser.
 *
 * Keyboard-driven browser for ANX state objects — the file manager
 * equivalent in an Anunix environment. Shows all live objects with
 * their type, size, and OID. Supports inspection and deletion.
 *
 * Layout:
 *   ┌───────────────────────────────────────────────────┐
 *   │ Object Store                       [R]efresh [D]el│
 *   ├──────────────────────────────────────────────────-│
 *   │ OID (hex)              Type            Size        │
 *   │────────────────────────────────────────────────── │
 *   │▶ 0000000000000001      WORKFLOW         12K        │
 *   │  0000000000000002      JEPA_TRACE        8K        │
 *   │  0000000000000003      EXECUTION_TRACE   256B      │
 *   ├───────────────────────────────────────────────────┤
 *   │ refs: 2  policy: replicate-2  created: 5m ago     │
 *   └───────────────────────────────────────────────────┘
 *
 * Keys:
 *   Up/Down  — navigate rows
 *   Enter    — show detail pane
 *   D        — delete selected object
 *   R        — refresh listing
 *   Escape   — close
 */

#include <anx/types.h>
#include <anx/wm.h>
#include <anx/state_object.h>
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

#define OV_HEADER_H	36
#define OV_COL_H	24	/* header column row height */
#define OV_ROW_H	18	/* list row height */
#define OV_DETAIL_H	36
#define OV_MAX_ENTRIES	128

/* ------------------------------------------------------------------ */
/* Per-entry snapshot (populated during refresh)                       */
/* ------------------------------------------------------------------ */

struct ov_entry {
	anx_oid_t            oid;
	enum anx_object_type obj_type;
	uint64_t             payload_size;
	uint32_t             refcount;
};

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct {
	struct anx_surface *surf;
	uint32_t           *pixels;
	uint32_t            pix_w;
	uint32_t            pix_h;

	struct ov_entry     entries[OV_MAX_ENTRIES];
	uint32_t            count;
	int32_t             selected;
	int32_t             scroll_top;	/* first visible row index */
} g_ov;

/* ------------------------------------------------------------------ */
/* Object type name                                                    */
/* ------------------------------------------------------------------ */

static const char *obj_type_name(enum anx_object_type t)
{
	switch (t) {
	case ANX_OBJ_BYTE_DATA:       return "BYTE_DATA";
	case ANX_OBJ_STRUCTURED_DATA: return "STRUCT_DATA";
	case ANX_OBJ_EMBEDDING:       return "EMBEDDING";
	case ANX_OBJ_GRAPH_NODE:      return "GRAPH_NODE";
	case ANX_OBJ_MODEL_OUTPUT:    return "MODEL_OUT";
	case ANX_OBJ_EXECUTION_TRACE: return "EXEC_TRACE";
	case ANX_OBJ_CAPABILITY:      return "CAPABILITY";
	case ANX_OBJ_CREDENTIAL:      return "CREDENTIAL";
	case ANX_OBJ_SURFACE:         return "SURFACE";
	case ANX_OBJ_TENSOR:          return "TENSOR";
	case ANX_OBJ_WORKFLOW:        return "WORKFLOW";
	case ANX_OBJ_JEPA_TRACE:      return "JEPA_TRACE";
	case ANX_OBJ_LOOP_SESSION:    return "LOOP_SESSION";
	case ANX_OBJ_PLAN:            return "PLAN";
	default:                      return "unknown";
	}
}

static void fmt_size(uint64_t bytes, char *buf, uint32_t buflen)
{
	if (bytes >= 1024 * 1024)
		anx_snprintf(buf, buflen, "%lluM", bytes / (1024 * 1024));
	else if (bytes >= 1024)
		anx_snprintf(buf, buflen, "%lluK", bytes / 1024);
	else
		anx_snprintf(buf, buflen, "%lluB", bytes);
}

static void fmt_oid_short(const anx_oid_t *oid, char *buf, uint32_t buflen)
{
	/* Show last 8 hex digits of lo for brevity */
	static const char hx[] = "0123456789abcdef";
	uint64_t lo = oid->lo;
	int i;
	char tmp[17];

	for (i = 15; i >= 0; i--) {
		tmp[i] = hx[lo & 0xf];
		lo >>= 4;
	}
	tmp[16] = '\0';
	anx_snprintf(buf, buflen, "...%s", tmp + 12);
}

/* ------------------------------------------------------------------ */
/* Pixel drawing                                                       */
/* ------------------------------------------------------------------ */

static void ov_fill(uint32_t x, uint32_t y,
		    uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row, col;

	for (row = y; row < y + h && row < g_ov.pix_h; row++)
		for (col = x; col < x + w && col < g_ov.pix_w; col++)
			g_ov.pixels[row * g_ov.pix_w + col] = color;
}

static void ov_text(uint32_t x, uint32_t y, const char *s, uint32_t fg)
{
	uint32_t sx = (uint32_t)g_ov.surf->x + x;
	uint32_t sy = (uint32_t)g_ov.surf->y + y;
	anx_gui_draw_string_scaled(sx, sy, s, fg, 0, 1);
}

/* ------------------------------------------------------------------ */
/* Refresh object list                                                 */
/* ------------------------------------------------------------------ */

struct refresh_ctx {
	struct ov_entry *entries;
	uint32_t        *count;
};

static int refresh_cb(struct anx_state_object *obj, void *arg)
{
	struct refresh_ctx *ctx = (struct refresh_ctx *)arg;
	struct ov_entry    *e;

	if (*ctx->count >= OV_MAX_ENTRIES)
		return 0;

	e = &ctx->entries[(*ctx->count)++];
	e->oid          = obj->oid;
	e->obj_type     = obj->object_type;
	e->payload_size = obj->payload_size;
	e->refcount     = obj->refcount;
	return 0;
}

static void ov_refresh(void)
{
	struct refresh_ctx ctx;

	ctx.entries = g_ov.entries;
	ctx.count   = &g_ov.count;
	g_ov.count  = 0;

	anx_objstore_iterate(refresh_cb, &ctx);

	if (g_ov.selected >= (int32_t)g_ov.count)
		g_ov.selected = (int32_t)g_ov.count - 1;
	if (g_ov.selected < 0 && g_ov.count > 0)
		g_ov.selected = 0;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void ov_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg     = theme->palette.background;
	uint32_t panel  = theme->palette.surface;
	uint32_t accent = theme->palette.accent;
	uint32_t text   = theme->palette.text_primary;
	uint32_t dim    = theme->palette.text_dim;
	uint32_t sel_bg = 0x001F4060;	/* selected row highlight */

	uint32_t list_y   = OV_HEADER_H + OV_COL_H;
	uint32_t detail_y = g_ov.pix_h - OV_DETAIL_H;
	uint32_t visible  = (detail_y - list_y) / OV_ROW_H;
	uint32_t i;

	/* Background */
	ov_fill(0, 0, g_ov.pix_w, g_ov.pix_h, bg);

	/* Header */
	ov_fill(0, 0, g_ov.pix_w, OV_HEADER_H, panel);
	ov_fill(0, OV_HEADER_H - 1, g_ov.pix_w, 1, accent);
	ov_text(10, 10, "Object Store", text);
	{
		char cnt[32];
		anx_snprintf(cnt, sizeof(cnt), "%u objects", g_ov.count);
		ov_text(g_ov.pix_w - 180, 10, cnt, dim);
	}
	ov_text(g_ov.pix_w - 120, 10, "[R]efresh", accent);
	ov_text(g_ov.pix_w - 50, 10, "[D]el", accent);

	/* Column header */
	ov_fill(0, OV_HEADER_H, g_ov.pix_w, OV_COL_H, panel);
	ov_fill(0, OV_HEADER_H + OV_COL_H - 1, g_ov.pix_w, 1, dim);
	ov_text(10,                OV_HEADER_H + 4, "OID",  dim);
	ov_text(g_ov.pix_w / 2,   OV_HEADER_H + 4, "Type", dim);
	ov_text(g_ov.pix_w * 3/4, OV_HEADER_H + 4, "Size", dim);

	/* Row list */
	for (i = 0; i < visible; i++) {
		uint32_t idx = (uint32_t)g_ov.scroll_top + i;
		char oid_str[12], type_str[20], size_str[12];
		uint32_t row_y = list_y + i * OV_ROW_H;
		bool selected = ((int32_t)idx == g_ov.selected);

		if (idx >= g_ov.count)
			break;

		if (selected)
			ov_fill(0, row_y, g_ov.pix_w, OV_ROW_H, sel_bg);

		fmt_oid_short(&g_ov.entries[idx].oid, oid_str, sizeof(oid_str));
		anx_snprintf(type_str, sizeof(type_str), "%s",
			     obj_type_name(g_ov.entries[idx].obj_type));
		fmt_size(g_ov.entries[idx].payload_size,
			 size_str, sizeof(size_str));

		ov_text(4,                row_y + 3, selected ? "▶" : " ", accent);
		ov_text(14,               row_y + 3, oid_str,   text);
		ov_text(g_ov.pix_w / 2,  row_y + 3, type_str,  selected ? text : dim);
		ov_text(g_ov.pix_w * 3/4,row_y + 3, size_str,  dim);
	}

	/* Detail pane */
	ov_fill(0, detail_y, g_ov.pix_w, OV_DETAIL_H, panel);
	ov_fill(0, detail_y, g_ov.pix_w, 1, accent);

	if (g_ov.selected >= 0 && (uint32_t)g_ov.selected < g_ov.count) {
		struct ov_entry *e = &g_ov.entries[g_ov.selected];
		char info[128];

		anx_snprintf(info, sizeof(info),
			     "type: %-16s  size: %-8llu  refs: %u",
			     obj_type_name(e->obj_type),
			     e->payload_size,
			     e->refcount);
		ov_text(10, detail_y + 10, info, text);
	} else {
		ov_text(10, detail_y + 10,
			"Up/Down: select  Enter: inspect  D: delete  R: refresh  Esc: close",
			dim);
	}

	anx_iface_surface_commit(g_ov.surf);
}

/* ------------------------------------------------------------------ */
/* Input handling                                                      */
/* ------------------------------------------------------------------ */

void anx_wm_object_viewer_key(uint32_t key, uint32_t mods)
{
	(void)mods;

	switch (key) {
	case ANX_KEY_ESC:
		anx_wm_window_close(g_ov.surf);
		g_ov.surf = NULL;
		return;

	case ANX_KEY_UP:
		if (g_ov.selected > 0) {
			g_ov.selected--;
			if (g_ov.selected < g_ov.scroll_top)
				g_ov.scroll_top = g_ov.selected;
			ov_render();
		}
		break;

	case ANX_KEY_DOWN:
		if (g_ov.selected + 1 < (int32_t)g_ov.count) {
			g_ov.selected++;
			{
				uint32_t detail_y = g_ov.pix_h - OV_DETAIL_H;
				uint32_t list_y   = OV_HEADER_H + OV_COL_H;
				uint32_t visible  = (detail_y - list_y) / OV_ROW_H;

				if (g_ov.selected >= g_ov.scroll_top + (int32_t)visible)
					g_ov.scroll_top = g_ov.selected - (int32_t)visible + 1;
			}
			ov_render();
		}
		break;

	case ANX_KEY_R:
		ov_refresh();
		ov_render();
		break;

	case ANX_KEY_D:
		if (g_ov.selected >= 0 && (uint32_t)g_ov.selected < g_ov.count) {
			struct ov_entry *e = &g_ov.entries[g_ov.selected];
			struct anx_state_object *obj = anx_objstore_lookup(&e->oid);

			if (obj) {
				/* Only delete objects with no remaining references */
				if (obj->refcount <= 1) {
					anx_objstore_release(obj);
					/* Mark for deletion via lifecycle policy */
					kprintf("[object-viewer] released OID hi=%llu lo=%llu\n",
						(unsigned long long)e->oid.hi,
						(unsigned long long)e->oid.lo);
					ov_refresh();
					ov_render();
				} else {
					uint32_t refs = obj->refcount;
					anx_objstore_release(obj);
					kprintf("[object-viewer] object has %u refs, not deleting\n",
						refs);
				}
			}
		}
		break;

	case ANX_KEY_ENTER:
		if (g_ov.selected >= 0 && (uint32_t)g_ov.selected < g_ov.count) {
			struct ov_entry *e = &g_ov.entries[g_ov.selected];
			kprintf("[object-viewer] OID hi=%llu lo=%llu  type=%s  size=%llu  refs=%u\n",
				(unsigned long long)e->oid.hi,
				(unsigned long long)e->oid.lo,
				obj_type_name(e->obj_type),
				(unsigned long long)e->payload_size,
				e->refcount);
		}
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Launch                                                              */
/* ------------------------------------------------------------------ */

void anx_wm_launch_object_viewer(void)
{
	const struct anx_fb_info *fb;
	struct anx_content_node  *cn;
	uint32_t w, h, buf_size;

	if (g_ov.surf) {
		anx_wm_window_focus(g_ov.surf);
		return;
	}

	fb = anx_fb_get_info();
	if (!fb || !fb->available) {
		kprintf("[object-viewer] no framebuffer\n");
		return;
	}

	w = fb->width  * 2 / 3;
	h = fb->height * 2 / 3;

	buf_size    = w * h * 4;
	g_ov.pixels = anx_alloc(buf_size);
	if (!g_ov.pixels)
		return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_ov.pixels);
		g_ov.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_ov.pixels;
	cn->data_len = buf_size;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     (int32_t)fb->width  / 6,
				     (int32_t)fb->height / 6,
				     w, h, &g_ov.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_ov.pixels);
		g_ov.pixels = NULL;
		return;
	}

	g_ov.pix_w      = w;
	g_ov.pix_h      = h;
	g_ov.selected   = 0;
	g_ov.scroll_top = 0;

	ov_refresh();
	anx_iface_surface_map(g_ov.surf);
	anx_wm_window_open(g_ov.surf);
	ov_render();
	kprintf("[object-viewer] opened (%u objects)\n", g_ov.count);
}
