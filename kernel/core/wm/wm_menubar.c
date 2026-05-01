/*
 * wm_menubar.c — Menu bar surface.
 *
 * Full-width surface pinned to top of screen. Pixel canvas updated on
 * workspace switch, network change, or clock tick.
 *
 * Layout (left → right):
 *   [10px margin] [ws1][ws2]...[ws9]  [clock HH:MM]  [● net] [⏻]
 *
 * Workspace dots: filled circle = active, hollow = occupied, dim = empty.
 * Network dot: teal = up, red = down, dim = unknown.
 * Power icon: right edge, shows shutdown menu on click (future).
 */

#include <anx/wm.h>
#include <anx/types.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/fb.h>
#include <anx/gui.h>
#include <anx/font.h>
#include <anx/theme.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/net.h>
#include <anx/virtio_net.h>
#include <anx/mt7925.h>

/* Exposed to wm.c so it can set g_menubar */
extern struct anx_surface *g_menubar;
extern uint32_t           *g_menubar_pixels;

/* ------------------------------------------------------------------ */
/* Pixel drawing helpers (direct into menubar pixel buffer)            */
/* ------------------------------------------------------------------ */

static uint32_t mb_width;
static uint32_t mb_height;

static void mb_fill_rect(uint32_t x, uint32_t y,
			  uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t row, col;

	if (!g_menubar_pixels)
		return;
	for (row = y; row < y + h && row < mb_height; row++) {
		for (col = x; col < x + w && col < mb_width; col++)
			g_menubar_pixels[row * mb_width + col] = color;
	}
}

static void mb_fill_rounded_rect(uint32_t x, uint32_t y,
				  uint32_t w, uint32_t h,
				  uint32_t radius, uint32_t color)
{
	uint32_t r, row_y, col_x, dx, dy;

	if (w == 0 || h == 0 || !g_menubar_pixels)
		return;
	r = radius;
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;

	for (row_y = y; row_y < y + h && row_y < mb_height; row_y++) {
		uint32_t row_off = row_y - y;
		uint32_t x_start = x;
		uint32_t x_end   = x + w;

		if (row_off < r) {
			dy = r - row_off;
			for (col_x = x; col_x < x + r; col_x++) {
				dx = r - (col_x - x);
				if (dx * dx + dy * dy > r * r)
					x_start = col_x + 1;
				else
					break;
			}
			for (col_x = x + w - 1; col_x >= x + w - r && col_x >= x; col_x--) {
				dx = r - (x + w - 1 - col_x);
				if (dx * dx + dy * dy > r * r)
					x_end = col_x;
				else
					break;
			}
		} else if (row_off >= h - r) {
			dy = r - (h - 1 - row_off);
			for (col_x = x; col_x < x + r; col_x++) {
				dx = r - (col_x - x);
				if (dx * dx + dy * dy > r * r)
					x_start = col_x + 1;
				else
					break;
			}
			for (col_x = x + w - 1; col_x >= x + w - r && col_x >= x; col_x--) {
				dx = r - (x + w - 1 - col_x);
				if (dx * dx + dy * dy > r * r)
					x_end = col_x;
				else
					break;
			}
		}

		if (x_start < x_end) {
			for (col_x = x_start; col_x < x_end && col_x < mb_width; col_x++)
				g_menubar_pixels[row_y * mb_width + col_x] = color;
		}
	}
}

static void mb_fill_circle(uint32_t cx, uint32_t cy, uint32_t r, uint32_t color)
{
	int32_t dx, dy;
	int32_t ir = (int32_t)r;

	if (!g_menubar_pixels)
		return;
	for (dy = -ir; dy <= ir; dy++) {
		for (dx = -ir; dx <= ir; dx++) {
			if (dx * dx + dy * dy <= ir * ir) {
				int32_t px = (int32_t)cx + dx;
				int32_t py = (int32_t)cy + dy;

				if (px >= 0 && py >= 0 &&
				    (uint32_t)px < mb_width &&
				    (uint32_t)py < mb_height)
					g_menubar_pixels[py * mb_width + px] = color;
			}
		}
	}
}

static void mb_draw_str(uint32_t x, uint32_t y, const char *s,
			uint32_t fg, uint32_t bg)
{
	if (g_menubar_pixels)
		anx_font_blit_str(g_menubar_pixels, mb_width, mb_height,
				  x, y, s, fg, bg);
}

/* Scan-line fill for a general triangle given three vertices. */
static void mb_fill_triangle(int32_t x0, int32_t y0,
			      int32_t x1, int32_t y1,
			      int32_t x2, int32_t y2,
			      uint32_t color)
{
	int32_t tmp, row;

	if (!g_menubar_pixels)
		return;

	/* Sort by ascending Y */
	if (y0 > y1) { tmp=x0;x0=x1;x1=tmp; tmp=y0;y0=y1;y1=tmp; }
	if (y0 > y2) { tmp=x0;x0=x2;x2=tmp; tmp=y0;y0=y2;y2=tmp; }
	if (y1 > y2) { tmp=x1;x1=x2;x2=tmp; tmp=y1;y1=y2;y2=tmp; }

	/* Degenerate: nothing to draw */
	if (y0 >= y2)
		return;

	for (row = y0; row <= y2; row++) {
		int32_t left, right, lx, rx, col, dy;

		if (row < 0 || (uint32_t)row >= mb_height)
			continue;

		/* Long edge (v0→v2): y0 < y2 guaranteed, no zero-divisor risk */
		dy = y2 - y0;
		lx = x0 + (x2 - x0) * (row - y0) / dy;

		/* Short edge: use same local 'dy' for both check and division */
		if (row <= y1) {
			dy = y1 - y0;
			if (dy > 0)
				rx = x0 + (x1 - x0) * (row - y0) / dy;
			else
				rx = x0;
		} else {
			dy = y2 - y1;
			if (dy > 0)
				rx = x1 + (x2 - x1) * (row - y1) / dy;
			else
				rx = x1;
		}

		left  = (lx < rx) ? lx : rx;
		right = (lx > rx) ? lx : rx;
		if (left  < 0)                  left  = 0;
		if (right >= (int32_t)mb_width) right = (int32_t)mb_width - 1;
		if (left > right)               continue;

		for (col = left; col <= right; col++)
			g_menubar_pixels[row * (int32_t)mb_width + col] = color;
	}
}

/* Draw the Anunix 'A' logo: outer triangle + inner V-cutout + eye dot. */
static void mb_draw_logo(uint32_t x, uint32_t cy, uint32_t color, uint32_t bg)
{
	int32_t ix  = (int32_t)x;
	int32_t icy = (int32_t)cy;
	int32_t h   = 12;  /* total logo height */
	int32_t hw  = 6;   /* half-width at base */

	/* Outer upward-pointing triangle */
	mb_fill_triangle(ix + hw, icy - h / 2,          /* apex */
			 ix,       icy + h / 2,          /* base left */
			 ix + hw * 2, icy + h / 2,       /* base right */
			 color);

	/* Inner V-cutout: makes the hollow 'A' interior */
	mb_fill_triangle(ix + hw,     icy - h / 2 + 4,  /* V apex */
			 ix + 2,      icy + h / 2,       /* cut left */
			 ix + hw * 2 - 2, icy + h / 2,  /* cut right */
			 bg);

	/* Eye dot above the V apex */
	mb_fill_circle((uint32_t)(ix + hw),
		       (uint32_t)(icy - h / 2 + 4),
		       2u, color);
}

/* ------------------------------------------------------------------ */
/* Menu bar surface creation                                           */
/* ------------------------------------------------------------------ */

int anx_wm_menubar_create(void)
{
	const struct anx_fb_info *fb;
	struct anx_content_node  *cn;
	uint32_t buf_size;

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return ANX_ENOENT;

	mb_width  = fb->width;
	mb_height = ANX_WM_MENUBAR_H;
	buf_size  = mb_width * mb_height * 4;

	g_menubar_pixels = anx_alloc(buf_size);
	if (!g_menubar_pixels)
		return ANX_ENOMEM;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_menubar_pixels);
		g_menubar_pixels = NULL;
		return ANX_ENOMEM;
	}

	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_menubar_pixels;
	cn->data_len = buf_size;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     0, 0, mb_width, mb_height,
				     &g_menubar) != ANX_OK) {
		anx_free(cn);
		anx_free(g_menubar_pixels);
		g_menubar_pixels = NULL;
		return ANX_ENOMEM;
	}

	anx_iface_surface_map(g_menubar);
	anx_iface_surface_raise(g_menubar);
	anx_wm_menubar_refresh();
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Redraw                                                              */
/* ------------------------------------------------------------------ */

void anx_wm_menubar_refresh(void)
{
	const struct anx_theme *theme;
	uint32_t bg, accent, dim, success, err_col;
	uint32_t ws, cy, dot_r, dot_x;
	char clock_str[8];
	uint32_t clock_x, text_y;

	if (!g_menubar || !g_menubar_pixels)
		return;

	theme   = anx_theme_get();
	bg      = theme->palette.surface;
	accent  = theme->palette.accent;
	dim     = theme->palette.text_dim;
	success = theme->palette.success;
	err_col = theme->palette.error;

	/* Floating pill with 1px teal border.
	 * 1. Desktop background fill.
	 * 2. Pill border (1px larger on each side) in muted teal.
	 * 3. Pill fill (bg color) on top. */
	{
		uint32_t pill_margin_x = 6;
		uint32_t pill_margin_y = 4;
		uint32_t pill_w = mb_width - pill_margin_x * 2;
		uint32_t pill_h = mb_height - pill_margin_y * 2;

		mb_fill_rect(0, 0, mb_width, mb_height,
			     theme->palette.background);
		mb_fill_rounded_rect(pill_margin_x - 1, pill_margin_y - 1,
				     pill_w + 2, pill_h + 2, 11,
				     0x00294F6Bu);	/* teal border */
		mb_fill_rounded_rect(pill_margin_x, pill_margin_y,
				     pill_w, pill_h, 10, bg);
	}

	/* Vertical centre for dots and icons (within pill) */
	cy    = mb_height / 2;
	dot_r = 4;
	text_y = (mb_height > ANX_FONT_HEIGHT)
		 ? (mb_height - ANX_FONT_HEIGHT) / 2
		 : 0;

	/* ---- Anunix logo (A-triangle) -------------------------------- */
	mb_draw_logo(14, cy, accent, bg);

	/* ---- Workspace dots (start after logo) ----------------------- */
	dot_x = 34;
	for (ws = 1; ws <= ANX_WM_WORKSPACES; ws++) {
		bool is_active   = (ws == anx_wm_workspace_active());
		bool is_occupied = anx_wm_workspace_occupied(ws);
		uint32_t color;

		if (is_active)
			color = accent;
		else if (is_occupied)
			color = theme->palette.text_primary;
		else
			color = dim;

		mb_fill_circle(dot_x, cy, dot_r, color);

		/* Hollow ring for occupied-but-inactive: paint centre with bg */
		if (is_occupied && !is_active)
			mb_fill_circle(dot_x, cy, dot_r - 2, bg);

		dot_x += 18;
	}

	/* ---- Clock + date (centred) — compute position first ---------- */
	{
		char date_str[8];
		char combined[16];
		uint32_t tw;

		anx_gui_get_time(clock_str, sizeof(clock_str));
		anx_gui_get_date(date_str,  sizeof(date_str));

		/* "Mon 26  14:30" — date + two spaces + time */
		anx_snprintf(combined, sizeof(combined), "%s  %s",
			     date_str, clock_str);

		tw      = (uint32_t)anx_strlen(combined) * ANX_FONT_WIDTH;
		clock_x = (mb_width > tw) ? (mb_width - tw) / 2 : 0;
		mb_draw_str(clock_x, text_y, combined,
			    theme->palette.text_primary, bg);
	}

	/* ---- Focused window title — clipped to stay left of clock ---- */
	{
		anx_oid_t         foc = anx_input_focus_get();
		struct anx_surface *s = NULL;

		if ((foc.hi || foc.lo) &&
		    anx_iface_surface_lookup(foc, &s) == ANX_OK &&
		    s && s->title[0]) {
			uint32_t tx        = dot_x + 16;
			uint32_t max_w     = (clock_x > tx + 8) ? clock_x - tx - 8 : 0;
			uint32_t max_chars = max_w / ANX_FONT_WIDTH;
			char     clipped[64];
			uint32_t tlen      = (uint32_t)anx_strlen(s->title);

			if (max_chars < 4)
				goto skip_title;

			if (tlen > max_chars) {
				/* Truncate with ellipsis */
				uint32_t copy = (max_chars > 2) ? max_chars - 1 : max_chars;
				uint32_t i;
				for (i = 0; i < copy && i < 63; i++)
					clipped[i] = s->title[i];
				if (copy < 63) clipped[copy++] = '~';
				clipped[copy] = '\0';
				mb_draw_str(tx, text_y, clipped,
					    theme->palette.text_dim, bg);
			} else {
				mb_draw_str(tx, text_y, s->title,
					    theme->palette.text_primary, bg);
			}
		skip_title:;
		}
	}

	/* ---- Network status: 6px glow dot + label -------------------- */
	{
		uint32_t net_x = mb_width - 80;
		uint32_t dot_color, glow_color;
		uint32_t local_ip = anx_ipv4_local_ip();
		const char *net_label;

		if (local_ip != 0) {
			dot_color = success;
			glow_color = 0x001A5028u; /* dim green glow */
			net_label = anx_mt7925_state() >= MT7925_STATE_ASSOC
				    ? "wifi" : "lan";
		} else if (anx_virtio_net_ready() ||
			   anx_mt7925_state() >= MT7925_STATE_ASSOC) {
			dot_color = theme->palette.warning;
			glow_color = 0x005A3800u; /* dim amber glow */
			net_label = "link";
		} else {
			dot_color = err_col;
			glow_color = 0x004A1010u; /* dim red glow */
			net_label = "off";
		}

		/* Outer glow ring then bright dot */
		mb_fill_circle(net_x, cy, 5u, glow_color);
		mb_fill_circle(net_x, cy, 3u, dot_color);
		mb_draw_str(net_x + 10, text_y, net_label, dim, bg);
	}

	/* ---- Power icon: proper power-button outline (circle + stem) -- */
	{
		uint32_t pw_cx = mb_width - 16;

		/* Circle ring */
		mb_fill_circle(pw_cx, cy, 5u, dim);
		mb_fill_circle(pw_cx, cy, 3u, bg);
		/* Stem: two pixels breaking the top of the ring */
		mb_fill_rect(pw_cx - 1, cy - 7, 3, 4, bg);
		mb_fill_rect(pw_cx - 1, cy - 7, 3, 3, dim);
	}

	/* Commit the updated canvas to the framebuffer */
	if (g_menubar->state == ANX_SURF_VISIBLE)
		anx_iface_surface_commit(g_menubar);
}
