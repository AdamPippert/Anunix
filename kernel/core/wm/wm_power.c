/*
 * wm_power.c — Power confirmation dialog.
 *
 * Restarting or halting is not undoable, so every path that can do it opens
 * this dialog first: the power icon in the menu bar, Ctrl+Alt+Delete, and
 * Meta+Escape. Nothing happens until the person chooses. Escape, N, or a
 * click outside cancels, and Cancel starts selected.
 *
 * The dialog is a canvas surface in the theme palette, built the same way
 * as the help overlay (wm_help.c).
 */

#include <anx/types.h>
#include <anx/wm.h>
#include <anx/interface_plane.h>
#include <anx/input.h>
#include <anx/theme.h>
#include <anx/font.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/fb.h>
#include <anx/arch.h>
#include <anx/bootlog.h>
#include <anx/kprintf.h>

#define PWR_W		460
#define PWR_H		140
#define PWR_PAD		16
#define BTN_W		104
#define BTN_H		32
#define BTN_GAP		14
#define BTN_Y		(PWR_H - PWR_PAD - BTN_H)

enum pwr_choice {
	PWR_RESTART = 0,
	PWR_HALT,
	PWR_CANCEL,
	PWR_CHOICES
};

static const char *const pwr_labels[PWR_CHOICES] = {
	"Restart", "Halt", "Cancel"
};

static struct {
	struct anx_surface *surf;
	uint32_t           *pixels;
	uint32_t            sel;
} g_pwr;

static void p_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		   uint32_t color)
{
	uint32_t r, c;

	for (r = y; r < y + h && r < (uint32_t)PWR_H; r++)
		for (c = x; c < x + w && c < (uint32_t)PWR_W; c++)
			g_pwr.pixels[r * PWR_W + c] = color;
}

static void p_str(uint32_t x, uint32_t y, const char *s, uint32_t fg,
		  uint32_t bg)
{
	anx_font_blit_str(g_pwr.pixels, PWR_W, PWR_H, x, y, s, fg, bg);
}

static uint32_t btn_x(uint32_t i)
{
	return PWR_PAD + i * (BTN_W + BTN_GAP);
}

bool anx_wm_power_active(void)
{
	return g_pwr.surf != NULL;
}

void anx_wm_power_close(void)
{
	if (!g_pwr.surf)
		return;
	anx_wm_overlay_destroy(g_pwr.surf);
	g_pwr.surf = NULL;
	if (g_pwr.pixels) {
		anx_free(g_pwr.pixels);
		g_pwr.pixels = NULL;
	}
}

static void pwr_render(void)
{
	const struct anx_theme *theme = anx_theme_get();
	uint32_t bg     = theme->palette.surface;
	uint32_t fg     = theme->palette.text_primary;
	uint32_t dim    = theme->palette.text_dim;
	uint32_t accent = theme->palette.accent;
	uint32_t border = theme->palette.border;
	uint32_t i;

	p_fill(0, 0, PWR_W, PWR_H, bg);
	for (i = 0; i < (uint32_t)PWR_W; i++) {
		g_pwr.pixels[i] = border;
		g_pwr.pixels[(PWR_H - 1) * PWR_W + i] = border;
	}
	for (i = 0; i < (uint32_t)PWR_H; i++) {
		g_pwr.pixels[i * PWR_W] = border;
		g_pwr.pixels[i * PWR_W + PWR_W - 1] = border;
	}

	p_fill(1, 1, PWR_W - 2, 22, accent);
	p_str(PWR_PAD, 1 + (22 - ANX_FONT_HEIGHT) / 2, "Power", 0x00FFFFFF,
	      accent);

	p_str(PWR_PAD, PWR_PAD + 24, "Restart or halt this machine?", fg,
	      ANX_FONT_TRANSPARENT);
	p_str(PWR_PAD, PWR_PAD + 44, "Unsaved cell state is lost.", dim,
	      ANX_FONT_TRANSPARENT);

	for (i = 0; i < PWR_CHOICES; i++) {
		uint32_t x = btn_x(i);
		uint32_t face = i == g_pwr.sel ? accent : border;
		uint32_t text = i == g_pwr.sel ? 0x00FFFFFF : fg;

		p_fill(x, BTN_Y, BTN_W, BTN_H, face);
		p_str(x + 16, BTN_Y + (BTN_H - ANX_FONT_HEIGHT) / 2,
		      pwr_labels[i], text, face);
	}

	if (g_pwr.surf)
		anx_iface_surface_commit(g_pwr.surf);
}

void anx_wm_power_open(void)
{
	const struct anx_fb_info *fb;
	struct anx_content_node *cn;
	uint32_t buf_size;
	int32_t sx, sy;

	if (anx_wm_power_active())
		return;

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return;

	buf_size = (uint32_t)(PWR_W * PWR_H) * 4u;
	g_pwr.pixels = anx_alloc(buf_size);
	if (!g_pwr.pixels)
		return;
	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_pwr.pixels);
		g_pwr.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_pwr.pixels;
	cn->data_len = buf_size;

	sx = ((int32_t)fb->width - PWR_W) / 2;
	sy = ((int32_t)fb->height - PWR_H) / 2;
	if (sx < 0)
		sx = 0;
	if (sy < 0)
		sy = 0;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn, sx, sy,
				     PWR_W, PWR_H, &g_pwr.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_pwr.pixels);
		g_pwr.pixels = NULL;
		return;
	}

	g_pwr.sel = PWR_CANCEL;		/* the safe choice starts selected */
	pwr_render();
	g_pwr.surf->no_focus = true;	/* keys reach it through the hotkey path */
	anx_iface_surface_map(g_pwr.surf);
	anx_iface_surface_raise(g_pwr.surf);
}

/* Carry out a choice. Reached only from an explicit selection. */
static void pwr_commit(uint32_t choice)
{
	anx_wm_power_close();

	if (choice == PWR_CANCEL)
		return;

	anx_bootlog_shutdown();
	if (choice == PWR_RESTART) {
		kprintf("[wm] restart requested\n");
		arch_reboot();
	}
	kprintf("[wm] halt requested\n");
	arch_halt();
}

bool anx_wm_power_key(uint32_t key)
{
	if (!anx_wm_power_active())
		return false;

	switch (key) {
	case ANX_KEY_ESC:
	case ANX_KEY_N:
		anx_wm_power_close();
		return true;
	case ANX_KEY_LEFT:
		g_pwr.sel = g_pwr.sel == 0 ? PWR_CHOICES - 1 : g_pwr.sel - 1;
		pwr_render();
		return true;
	case ANX_KEY_RIGHT:
	case ANX_KEY_TAB:
		g_pwr.sel = (g_pwr.sel + 1) % PWR_CHOICES;
		pwr_render();
		return true;
	case ANX_KEY_Y:
	case ANX_KEY_R:
		pwr_commit(PWR_RESTART);
		return true;
	case ANX_KEY_H:
		pwr_commit(PWR_HALT);
		return true;
	case ANX_KEY_ENTER:
		pwr_commit(g_pwr.sel);
		return true;
	default:
		return true;	/* modal: swallow every other key */
	}
}

bool anx_wm_power_pointer(int32_t x, int32_t y, uint32_t buttons,
			  bool move_only)
{
	int32_t lx, ly;
	uint32_t i;

	if (!anx_wm_power_active())
		return false;

	lx = x - g_pwr.surf->x;
	ly = y - g_pwr.surf->y;

	if (lx < 0 || ly < 0 || lx >= PWR_W || ly >= PWR_H) {
		/* A click outside cancels; plain movement passes through. */
		if (!move_only && (buttons & 3))
			anx_wm_power_close();
		return !move_only;
	}

	for (i = 0; i < PWR_CHOICES; i++) {
		int32_t bx = (int32_t)btn_x(i);

		if (lx < bx || lx >= bx + BTN_W ||
		    ly < BTN_Y || ly >= BTN_Y + BTN_H)
			continue;
		if (g_pwr.sel != i) {
			g_pwr.sel = i;
			pwr_render();
		}
		if (!move_only && (buttons & 1))
			pwr_commit(i);
		return true;
	}
	return true;
}
