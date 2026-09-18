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

static uint32_t mb_width;
static uint32_t mb_height;

#define MB_SCALE 150u
#define MB_FONT_W (ANX_FONT_WIDTH * MB_SCALE / 100)
#define MB_FONT_H (ANX_FONT_HEIGHT * MB_SCALE / 100)
#define MB_LOGO_X 21u
#define MB_LOGO_END 39u
#define MB_DOT_X 51u
#define MB_DOT_STEP 27u
#define MB_DOT_R 6u
#define MB_DOT_HIT 10u
#define MB_GAP 12u

struct mb_layout {
	uint32_t first_ws, ws_count;
	uint32_t clock_x, clock_chars;
	uint32_t title_x, title_chars;
	uint32_t net_x;
	bool logo, power, network, net_label;
};

/* Keep the clock centred. Narrow bars sacrifice labels, then workspace dots. */
static struct mb_layout mb_layout(void)
{
	struct mb_layout l = {0};
	uint32_t left_limit, right_limit, active = anx_wm_workspace_active();
	uint32_t dots_end = MB_DOT_X + (ANX_WM_WORKSPACES - 1) * MB_DOT_STEP + MB_DOT_R;

	l.logo = mb_width >= 48;
	l.power = mb_width >= 84;
	right_limit = l.power ? mb_width - 36 : mb_width;
	if (mb_width >= 2 * (dots_end + MB_GAP) + 13 * MB_FONT_W)
		l.clock_chars = 13; /* "Mon 26  14:30" */
	else if (mb_width >= 2 * (MB_LOGO_END + MB_GAP) + 5 * MB_FONT_W)
		l.clock_chars = 5;
	if (l.clock_chars)
		l.clock_x = (mb_width - l.clock_chars * MB_FONT_W) / 2;

	l.net_label = mb_width >= 480;
	l.net_x = mb_width >= 160 ? mb_width - (l.net_label ? 120 : 72) : 0;
	l.network = l.net_x >= MB_LOGO_END + MB_GAP + 8;
	if (l.clock_chars && l.net_x < l.clock_x + l.clock_chars * MB_FONT_W + MB_GAP + 8)
		l.network = false;
	if (l.network)
		right_limit = l.net_x - 8;
	left_limit = l.clock_chars ? l.clock_x : right_limit;
	while (l.ws_count < ANX_WM_WORKSPACES &&
	       MB_DOT_X + l.ws_count * MB_DOT_STEP + MB_DOT_HIT + MB_GAP <= left_limit)
		l.ws_count++;
	l.first_ws = 1;
	if (l.ws_count && l.ws_count < ANX_WM_WORKSPACES) {
		if (active > l.ws_count / 2)
			l.first_ws = active - l.ws_count / 2;
		if (l.first_ws > ANX_WM_WORKSPACES - l.ws_count + 1)
			l.first_ws = ANX_WM_WORKSPACES - l.ws_count + 1;
	}
	l.title_x = MB_DOT_X + l.ws_count * MB_DOT_STEP + 24;
	if (l.clock_chars && l.clock_x > l.title_x + MB_GAP)
		l.title_chars = (l.clock_x - l.title_x - MB_GAP) / MB_FONT_W;
	return l;
}

int anx_wm_menubar_hit(int32_t x, int32_t y)
{
	struct mb_layout l;
	uint32_t i;
	int32_t cy;

	if (!g_menubar)
		return 0;
	mb_width = g_menubar->width;
	mb_height = g_menubar->height;
	l = mb_layout();
	cy = (int32_t)mb_height / 2;

	if (x < 0 || y < 0 || (uint32_t)x >= mb_width || (uint32_t)y >= mb_height)
		return 0;
	if (l.logo && x < (int32_t)MB_LOGO_END)
		return -1;
	if (l.power && x >= (int32_t)mb_width - 36)
		return -2;
	for (i = 0; i < l.ws_count; i++) {
		int32_t cx = (int32_t)(MB_DOT_X + i * MB_DOT_STEP);
		if (x >= cx - (int32_t)MB_DOT_HIT && x <= cx + (int32_t)MB_DOT_HIT &&
		    y >= cy - (int32_t)MB_DOT_HIT && y <= cy + (int32_t)MB_DOT_HIT)
			return (int)(l.first_ws + i);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Pixel drawing helpers (direct into menubar pixel buffer)            */

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
			uint32_t fg)
{
	if (g_menubar_pixels)
		anx_font_blit_str_scaled(g_menubar_pixels, mb_width, mb_height,
					 x, y, s, fg, ANX_FONT_TRANSPARENT, MB_SCALE);
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
		int32_t left, right, lx, rx, col;
		int64_t dy;

		if (row < 0 || (uint32_t)row >= mb_height)
			continue;

		/* Interpolate edges in 64-bit: garbage/degenerate coordinates
		 * during an early repaint could overflow a 32-bit multiply and
		 * make the idiv quotient exceed INT32, raising #DE. 64-bit math
		 * and explicit divisor guards make the fill total-function; any
		 * out-of-range result is clamped by the bounds checks below. */
		dy = (int64_t)y2 - y0;
		if (dy <= 0)
			continue;
		lx = (int32_t)((int64_t)x0 + (int64_t)(x2 - x0) * (row - y0) / dy);

		if (row <= y1) {
			dy = (int64_t)y1 - y0;
			if (dy > 0)
				rx = (int32_t)((int64_t)x0 + (int64_t)(x1 - x0) * (row - y0) / dy);
			else
				rx = x0;
		} else {
			dy = (int64_t)y2 - y1;
			if (dy > 0)
				rx = (int32_t)((int64_t)x1 + (int64_t)(x2 - x1) * (row - y1) / dy);
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
	int32_t h   = 18;  /* total logo height */
	int32_t hw  = 9;   /* half-width at base */

	/* Outer upward-pointing triangle */
	mb_fill_triangle(ix + hw, icy - h / 2,          /* apex */
			 ix,       icy + h / 2,          /* base left */
			 ix + hw * 2, icy + h / 2,       /* base right */
			 color);

	/* Inner V-cutout: makes the hollow 'A' interior */
	mb_fill_triangle(ix + hw,     icy - h / 2 + 6,  /* V apex */
			 ix + 3,      icy + h / 2,       /* cut left */
			 ix + hw * 2 - 3, icy + h / 2,  /* cut right */
			 bg);

	/* Eye dot above the V apex */
	mb_fill_circle((uint32_t)(ix + hw),
		       (uint32_t)(icy - h / 2 + 6),
		       3u, color);
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

	g_menubar->no_focus = true;
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
	uint32_t text_y;
	struct mb_layout layout;

	if (!g_menubar || !g_menubar_pixels)
		return;

	mb_width = g_menubar->width;
	mb_height = g_menubar->height;
	layout = mb_layout();
	theme   = anx_theme_get();
	bg      = theme->palette.surface;
	accent  = theme->palette.accent;
	dim     = theme->palette.text_dim;
	success = theme->palette.success;
	err_col = theme->palette.error;

	/*
	 * A floating pill: the desktop shows around it, the border picks
	 * up the theme, and the fill is the same gradient the title bars
	 * use, with a bevel along the top edge.
	 */
	{
		uint32_t pill_margin_x = mb_width >= 20 ? 9 : 0;
		uint32_t pill_margin_y = 6;
		uint32_t pill_w = mb_width - pill_margin_x * 2;
		uint32_t pill_h = mb_height - pill_margin_y * 2;
		struct anx_shape pill = anx_theme_window_shape(15);
		struct anx_shape edge = anx_theme_window_shape(16);

		mb_fill_rect(0, 0, mb_width, mb_height,
			     theme->palette.background);
		anx_wm_buf_gradient(g_menubar_pixels, mb_width, mb_height,
				    pill_margin_x ? pill_margin_x - 1 : 0, pill_margin_y - 1,
				    pill_w + 2, pill_h + 2, &edge,
				    theme->palette.border,
				    theme->palette.border);
		anx_wm_buf_gradient(g_menubar_pixels, mb_width, mb_height,
				    pill_margin_x, pill_margin_y,
				    pill_w, pill_h, &pill,
				    theme->palette.bar_from,
				    theme->palette.bar_to);
		if (pill_w > 30)
			anx_wm_buf_blend(g_menubar_pixels, mb_width, mb_height,
					 pill_margin_x + 15, pill_margin_y,
					 pill_w - 30, 1, 0x00FFFFFF, 40);
		(void)bg;
	}

	/* Vertical centre for dots and icons (within pill) */
	cy    = mb_height / 2;
	dot_r = MB_DOT_R;
	text_y = (mb_height > MB_FONT_H)
		 ? (mb_height - MB_FONT_H) / 2
		 : 0;

	/* ---- Anunix logo (A-triangle) -------------------------------- */
	if (layout.logo)
		mb_draw_logo(MB_LOGO_X, cy, accent, bg);

	/* ---- Workspace dots (start after logo) ----------------------- */
	dot_x = MB_DOT_X;
	for (ws = layout.first_ws; ws < layout.first_ws + layout.ws_count; ws++) {
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
			mb_fill_circle(dot_x, cy, dot_r - 3, bg);

		dot_x += MB_DOT_STEP;
	}

	/* Clock stays centred; omit the date when the full clock would crowd dots. */
	if (layout.clock_chars) {
		char date_str[8], combined[16];
		anx_gui_get_time(clock_str, sizeof(clock_str));
		if (layout.clock_chars == 13) {
			anx_gui_get_date(date_str, sizeof(date_str));
			anx_snprintf(combined, sizeof(combined), "%s  %s", date_str, clock_str);
		} else {
			anx_strlcpy(combined, clock_str, sizeof(combined));
		}
		mb_draw_str(layout.clock_x, text_y, combined, theme->palette.text_primary);
	}

	/* The focused title never runs into the central clock or status area. */
	if (layout.title_chars >= 4) {
		anx_oid_t foc = anx_input_focus_get();
		struct anx_surface *s = NULL;
		if ((foc.hi || foc.lo) && anx_iface_surface_lookup(foc, &s) == ANX_OK &&
		    s && s->title[0]) {
			char clipped[64];
			uint32_t len = (uint32_t)anx_strlen(s->title);
			uint32_t count = layout.title_chars;
			if (count >= sizeof(clipped)) count = sizeof(clipped) - 1;
			if (count > len) count = len;
			anx_memcpy(clipped, s->title, count);
			if (len > count) clipped[count - 1] = '~';
			clipped[count] = 0;
			mb_draw_str(layout.title_x, text_y, clipped, theme->palette.text_primary);
		}
	}

	/* ---- Network status: scaled glow dot and optional label ------ */
	if (layout.network) {
		uint32_t net_x = layout.net_x;
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
		mb_fill_circle(net_x, cy, 8u, glow_color);
		mb_fill_circle(net_x, cy, 5u, dot_color);
		if (layout.net_label)
			mb_draw_str(net_x + 15, text_y, net_label, dim);
	}

	/* ---- Power icon: scaled outline and stem --------------------- */
	if (layout.power) {
		uint32_t pw_cx = mb_width - 24;

		/* Circle ring */
		mb_fill_circle(pw_cx, cy, 8u, dim);
		mb_fill_circle(pw_cx, cy, 5u, bg);
		/* Stem: two pixels breaking the top of the ring */
		mb_fill_rect(pw_cx - 2, cy - 10, 5, 6, bg);
		mb_fill_rect(pw_cx - 2, cy - 10, 5, 5, dim);
	}

	/* Commit the updated canvas to the framebuffer */
	if (g_menubar->state == ANX_SURF_VISIBLE) {
		/* The renderer hides the cursor first; the WM loop redraws it. */
		anx_iface_surface_commit(g_menubar);
	}
}
