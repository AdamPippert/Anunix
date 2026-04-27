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

/* Draw a single character at scale 1 directly into the menubar pixel buffer. */
static void mb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
{
	const uint16_t *glyph = anx_font_glyph(c);
	uint32_t row, col;

	if (!g_menubar_pixels)
		return;
	for (row = 0; row < ANX_FONT_HEIGHT && (y + row) < mb_height; row++) {
		uint16_t bits = glyph[row];

		for (col = 0; col < ANX_FONT_WIDTH && (x + col) < mb_width; col++)
			g_menubar_pixels[(y + row) * mb_width + (x + col)] =
				(bits & (0x800u >> col)) ? fg : bg;
	}
}

static void mb_draw_str(uint32_t x, uint32_t y, const char *s,
			uint32_t fg, uint32_t bg)
{
	for (; *s; s++, x += ANX_FONT_WIDTH)
		mb_draw_char(x, y, *s, fg, bg);
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

	/* Background fill */
	mb_fill_rect(0, 0, mb_width, mb_height, bg);

	/* Teal accent line at bottom */
	mb_fill_rect(0, mb_height - 2, mb_width, 2, accent);

	/* Vertical centre for dots and icons */
	cy    = mb_height / 2;
	dot_r = 4;
	text_y = (mb_height > ANX_FONT_HEIGHT)
		 ? (mb_height - ANX_FONT_HEIGHT) / 2
		 : 0;

	/* ---- Workspace dots ------------------------------------------ */
	dot_x = 16;
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

		dot_x += 20;
	}

	/* ---- Focused window title ------------------------------------ */
	{
		anx_oid_t         foc = anx_input_focus_get();
		struct anx_surface *s = NULL;

		if ((foc.hi || foc.lo) &&
		    anx_iface_surface_lookup(foc, &s) == ANX_OK &&
		    s && s->title[0]) {
			uint32_t tx = dot_x + 16;

			mb_draw_str(tx, text_y, s->title,
				    theme->palette.text_primary, bg);
		}
	}

	/* ---- Clock + date (centred) ---------------------------------- */
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

	/* ---- Network status dot + short label ------------------------ */
	{
		uint32_t net_x = mb_width - 72;
		uint32_t dot_color;
		uint32_t local_ip = anx_ipv4_local_ip();
		const char *net_label;

		if (local_ip != 0) {
			dot_color = success;
			net_label = anx_mt7925_state() >= MT7925_STATE_ASSOC
				    ? "wifi" : "lan";
		} else if (anx_virtio_net_ready() ||
			   anx_mt7925_state() >= MT7925_STATE_ASSOC) {
			dot_color = theme->palette.warning;
			net_label = "link";
		} else {
			dot_color = err_col;
			net_label = "off";
		}

		mb_fill_circle(net_x, cy, 4, dot_color);
		mb_draw_str(net_x + 8, text_y, net_label, dim, bg);
	}

	/* ---- Power icon (simple rectangle) ---------------------------- */
	{
		uint32_t pw_x = mb_width - 18;

		mb_fill_rect(pw_x,     cy - 5, 10, 10, dim);
		mb_fill_rect(pw_x + 3, cy - 9,  4,  6, dim);
	}

	/* Commit the updated canvas to the framebuffer */
	if (g_menubar->state == ANX_SURF_VISIBLE)
		anx_iface_surface_commit(g_menubar);
}
