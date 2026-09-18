/* Keyboard-driven palette editor. Apply is live; Save persists the theme. */
#include <anx/color_editor.h>
#include <anx/alloc.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/input.h>
#include <anx/interface_plane.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/theme.h>
#include <anx/tools.h>
#include <anx/wm.h>

#define CE_ROW_H (ANX_FONT_HEIGHT + 8)
#define CE_HEADER_H (2 * CE_ROW_H)
#define CE_FOOTER_H (4 * CE_ROW_H)
/* Stable editor ink remains readable while users change the live palette. */
#define CE_BG 0x18202Bu
#define CE_PANEL 0x263344u
#define CE_TEXT 0xEDF2F7u
#define CE_DIM 0xBDCADD
#define CE_SELECTED 0x395875u
#define CE_SUCCESS 0x205C3Bu
#define CE_FAILURE 0x852E35u

static struct {
	struct anx_surface *surf;
	uint32_t *pixels;
	uint32_t width, height, count, selected, top;
	char hex[7];
	uint32_t length, digit, original;
	bool editing, replace;
	uint32_t status_bg;
	char status[64];
} g_ce;

static void ce_status(const char *text)
{
	anx_snprintf(g_ce.status, sizeof(g_ce.status), "%s", text);
	g_ce.status_bg = CE_PANEL;
}

static int ce_nibble(uint32_t c)
{
	if (c >= '0' && c <= '9') return (int)(c - '0');
	if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
	if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
	return -1;
}

static bool ce_color(uint32_t *color)
{
	return g_ce.length == 6 &&
		anx_theme_parse_color_checked(g_ce.hex, color) == ANX_OK;
}

static uint32_t ce_visible(void)
{
	if (g_ce.height < CE_HEADER_H + CE_FOOTER_H + CE_ROW_H)
		return 0;
	return (g_ce.height - CE_HEADER_H - CE_FOOTER_H) / CE_ROW_H;
}

static void ce_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
		    uint32_t color)
{
	uint32_t row, col;

	if (x >= g_ce.width || y >= g_ce.height)
		return;
	if (w > g_ce.width - x) w = g_ce.width - x;
	if (h > g_ce.height - y) h = g_ce.height - y;
	for (row = y; row < y + h; row++)
		for (col = x; col < x + w; col++)
			g_ce.pixels[row * g_ce.width + col] = color;
}

static void ce_text(uint32_t x, uint32_t y, const char *text, uint32_t fg)
{
	anx_font_blit_str(g_ce.pixels, g_ce.width, g_ce.height,
			  x, y, text, fg, ANX_FONT_TRANSPARENT);
}

static void ce_swatch(uint32_t x, uint32_t y, const char *label, uint32_t color)
{
	char text[32];
	ce_fill(x, y + 3, 28, 26, CE_TEXT);
	ce_fill(x + 1, y + 4, 26, 24, color);
	anx_snprintf(text, sizeof(text), "%s #%06x", label, color & 0xffffffu);
	ce_text(x + 36, y + 4, text, CE_TEXT);
}

void anx_wm_color_editor_redraw(void)
{
	uint32_t i, visible, footer, current, preview;
	uint32_t *slot;
	char line[64];

	if (!g_ce.surf || !g_ce.pixels)
		return;
	ce_fill(0, 0, g_ce.width, g_ce.height, CE_BG);
	visible = ce_visible();
	if (!visible || g_ce.width < 360) {
		ce_text(8, 8, "Enlarge to edit colors", CE_TEXT);
		ce_text(8, 40, "Esc: quit", CE_DIM);
		return;
	}
	if (g_ce.selected < g_ce.top)
		g_ce.top = g_ce.selected;
	if (g_ce.selected >= g_ce.top + visible)
		g_ce.top = g_ce.selected - visible + 1;
	footer = g_ce.height - CE_FOOTER_H;
	ce_fill(0, 0, g_ce.width, CE_HEADER_H, CE_PANEL);
	anx_snprintf(line, sizeof(line), "Palette %u/%u", g_ce.selected + 1,
		     g_ce.count);
	ce_text(8, 4, line, CE_TEXT);
	ce_text(g_ce.width - 92, 4, "S: Save", CE_TEXT);
	/* A fixed, high-contrast banner remains visible while the list scrolls. */
	ce_fill(0, CE_ROW_H, g_ce.width, CE_ROW_H, g_ce.status_bg);
	ce_text(8, CE_ROW_H + 4, g_ce.status, CE_TEXT);
	for (i = 0; i < visible && g_ce.top + i < g_ce.count; i++) {
		uint32_t index = g_ce.top + i;
		uint32_t y = CE_HEADER_H + i * CE_ROW_H;
		const char *name = anx_theme_color_name(index);
		uint32_t *color = anx_theme_color_slot(name);

		if (!color)
			continue;
		if (index == g_ce.selected)
			ce_fill(0, y, g_ce.width, CE_ROW_H, CE_SELECTED);
		ce_fill(8, y + 3, 30, 26, CE_TEXT);
		ce_fill(9, y + 4, 28, 24, *color);
		ce_text(48, y + 4, name, CE_TEXT);
		anx_snprintf(line, sizeof(line), "#%06x", *color & 0xFFFFFFu);
		ce_text(g_ce.width - 92, y + 4, line, CE_TEXT);
	}
	ce_fill(0, footer, g_ce.width, CE_FOOTER_H, CE_PANEL);
	slot = anx_theme_color_slot(anx_theme_color_name(g_ce.selected));
	current = slot ? *slot : 0;
	preview = current;
	if (g_ce.editing) {
		uint32_t x = 8 + (6 + g_ce.digit) * ANX_FONT_WIDTH;
		anx_snprintf(line, sizeof(line), "Hex: #%s", g_ce.hex);
		ce_fill(x, footer + 2, ANX_FONT_WIDTH, CE_ROW_H - 4, CE_SELECTED);
		ce_text(8, footer + 4, line, CE_TEXT);
		ce_fill(x, footer + CE_ROW_H - 4, ANX_FONT_WIDTH, 2, CE_TEXT);
		ce_color(&preview);
	} else {
		ce_text(8, footer + 4, "Enter: edit selected color", CE_TEXT);
	}
	ce_swatch(8, footer + CE_ROW_H, "Was", g_ce.editing ? g_ce.original : current);
	ce_swatch(g_ce.width / 2, footer + CE_ROW_H, "Now", preview);
	ce_text(8, footer + 2 * CE_ROW_H + 4, g_ce.editing ?
		"Left/Right digit; Up/Down +/-" : "Up/Down/PgUp/PgDn: select", CE_TEXT);
	ce_text(8, footer + 3 * CE_ROW_H + 4, g_ce.editing ?
		"Enter apply; Esc cancel; S save" : "S: Save   Esc: Quit", CE_DIM);
}

static void ce_render(void)
{
	anx_wm_color_editor_redraw();
	anx_iface_surface_commit(g_ce.surf);
}

static uint32_t *ce_selected_slot(void)
{
	return anx_theme_color_slot(anx_theme_color_name(g_ce.selected));
}

static void ce_begin(void)
{
	uint32_t *slot = ce_selected_slot();

	if (!slot)
		return;
	g_ce.original = *slot;
	anx_snprintf(g_ce.hex, sizeof(g_ce.hex), "%06x", *slot & 0xFFFFFFu);
	g_ce.length = 6;
	g_ce.digit = 0;
	g_ce.editing = true;
	g_ce.replace = true;
	ce_status("Preview only; Enter keeps it");
}

/* A draft previews immediately, but only Enter accepts it and S persists. */
static void ce_preview(void)
{
	uint32_t color = g_ce.original;
	uint32_t *slot = ce_selected_slot();

	if (!slot)
		return;
	ce_color(&color);
	if (*slot != color) {
		anx_theme_set_color(slot, color);
		anx_wm_repaint_all();
	}
}

static bool ce_apply(void)
{
	uint32_t color, *slot = ce_selected_slot();

	if (!slot || !ce_color(&color)) {
		ce_status("Need exactly 6 hex digits");
		g_ce.status_bg = CE_FAILURE;
		return false;
	}
	anx_theme_set_color(slot, color);
	anx_theme_mark_custom();
	g_ce.editing = false;
	ce_status("Applied live; S to save");
	anx_wm_repaint_all();
	return true;
}

static void ce_save(void)
{
	int rc;

	if (g_ce.editing && !ce_apply())
		return;
	ce_status("Saving theme...");
	ce_render();
	rc = anx_theme_save();
	if (rc == ANX_OK) {
		ce_status("SAVED: theme stored on disk");
		g_ce.status_bg = CE_SUCCESS;
	} else {
		anx_snprintf(g_ce.status, sizeof(g_ce.status),
			     "SAVE FAILED (%d); S retries", rc);
		g_ce.status_bg = CE_FAILURE;
	}
}

static void ce_edit_key(uint32_t key, uint32_t unicode)
{
	static const char digits[] = "0123456789abcdef";

	if (key == ANX_KEY_ESC) {
		uint32_t *slot = ce_selected_slot();
		if (slot) anx_theme_set_color(slot, g_ce.original);
		g_ce.editing = false;
		ce_status("Cancelled; original restored");
		anx_wm_repaint_all();
	} else if (key == ANX_KEY_ENTER) {
		ce_apply();
	} else if (key == ANX_KEY_LEFT || key == ANX_KEY_RIGHT) {
		g_ce.replace = false;
		if (key == ANX_KEY_LEFT && g_ce.digit) g_ce.digit--;
		if (key == ANX_KEY_RIGHT && g_ce.digit < 5 && g_ce.digit < g_ce.length)
			g_ce.digit++;
	} else if (key == ANX_KEY_UP || key == ANX_KEY_DOWN) {
		int n;
		if (g_ce.length != 6) {
			ce_status("Complete 6 digits before +/-");
			return;
		}
		n = ce_nibble((uint8_t)g_ce.hex[g_ce.digit]);
		n = (n + (key == ANX_KEY_UP ? 1 : 15)) & 15;
		g_ce.hex[g_ce.digit] = digits[n];
		g_ce.replace = false;
		ce_status("Preview only; Enter keeps it");
		ce_preview();
	} else if (key == ANX_KEY_BACKSPACE) {
		g_ce.replace = false;
		if (g_ce.length) g_ce.hex[--g_ce.length] = 0;
		if (g_ce.digit > g_ce.length) g_ce.digit = g_ce.length;
		ce_status("Type hex digits; Enter applies");
		ce_preview();
	} else if (unicode >= 32) {
		uint32_t i;
		int nibble = ce_nibble(unicode);
		if (nibble < 0) {
			ce_status("Hex only: 0-9 A-F");
			return;
		}
		if (g_ce.replace) {
			g_ce.length = g_ce.digit = 0;
			g_ce.hex[0] = 0;
			g_ce.replace = false;
		}
		if (g_ce.length < 6) {
			for (i = g_ce.length; i > g_ce.digit; i--)
				g_ce.hex[i] = g_ce.hex[i - 1];
			g_ce.length++;
		}
		g_ce.hex[g_ce.digit] = digits[nibble];
		g_ce.hex[g_ce.length] = 0;
		if (g_ce.digit < 5) g_ce.digit++;
		ce_status("Preview only; Enter keeps it");
		ce_preview();
	}
}

void anx_wm_color_editor_key(uint32_t key, uint32_t mods, uint32_t unicode)
{
	uint32_t step = ce_visible();

	if (!g_ce.surf || !g_ce.count)
		return;
	mods &= ~ANX_MOD_LOCKS;
	if (mods & (ANX_MOD_META | ANX_MOD_ALT))
		return;
	if (key == ANX_KEY_S && !(mods & ~(ANX_MOD_CTRL | ANX_MOD_SHIFT))) {
		ce_save();
	} else if (g_ce.editing) {
		if (!(mods & ANX_MOD_CTRL))
			ce_edit_key(key, unicode);
	} else if (!(mods & ANX_MOD_CTRL)) {
		switch (key) {
		case ANX_KEY_ESC:
			anx_wm_window_close(g_ce.surf);
			return;
		case ANX_KEY_ENTER: ce_begin(); break;
		case ANX_KEY_UP:
			if (g_ce.selected) g_ce.selected--;
			break;
		case ANX_KEY_DOWN:
			if (g_ce.selected + 1 < g_ce.count) g_ce.selected++;
			break;
		case ANX_KEY_HOME: g_ce.selected = 0; break;
		case ANX_KEY_END: g_ce.selected = g_ce.count - 1; break;
		case ANX_KEY_PAGEUP:
			g_ce.selected = g_ce.selected > step ? g_ce.selected - step : 0;
			break;
		case ANX_KEY_PAGEDOWN:
			g_ce.selected += step;
			if (g_ce.selected >= g_ce.count) g_ce.selected = g_ce.count - 1;
			break;
		default: break;
		}
	}
	ce_render();
}

struct anx_surface *anx_wm_color_editor_surface(void)
{
	return g_ce.surf;
}

static void ce_destroy(struct anx_surface *surf)
{
	bool restore = g_ce.editing;
	if (restore) {
		uint32_t *slot = ce_selected_slot();
		if (slot) anx_theme_set_color(slot, g_ce.original);
	}
	anx_wm_canvas_free(surf);
	anx_memset(&g_ce, 0, sizeof(g_ce));
	/* The destroy hook runs before removal from the surface registry. */
	surf->state = ANX_SURF_DESTROYED;
	if (restore) anx_wm_repaint_all();
}

static void ce_resize(struct anx_surface *surf)
{
	uint32_t *pixels = anx_wm_canvas_realloc(surf, surf->width, surf->height);

	if (!pixels)
		return;
	g_ce.pixels = pixels;
	g_ce.width = surf->width;
	g_ce.height = surf->height;
	ce_render();
}

void anx_wm_launch_color_editor(void)
{
	const struct anx_fb_info *fb = anx_fb_get_info();
	struct anx_content_node *cn;
	uint32_t w, h;

	if (g_ce.surf) {
		anx_wm_window_restore(g_ce.surf);
		anx_wm_window_focus(g_ce.surf);
		return;
	}
	if (!fb || !fb->available) {
		kprintf("[colors] no framebuffer\n");
		return;
	}
	w = fb->width < 640 ? fb->width : 640;
	h = fb->height < 640 ? fb->height : 640;
	anx_wm_window_fit(&w, &h);
	if (!w || !h)
		return;
	cn = anx_zalloc(sizeof(*cn));
	if (!cn)
		goto no_memory;
	cn->type = ANX_CONTENT_CANVAS;
	cn->data_len = w * h * 4;
	cn->data = anx_alloc(cn->data_len);
	if (!cn->data) {
		anx_free(cn);
		goto no_memory;
	}
	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
		(int32_t)(fb->width - w) / 2, (int32_t)(fb->height - h) / 2,
		w, h, &g_ce.surf) != ANX_OK) {
		anx_free(cn->data);
		anx_free(cn);
		goto no_memory;
	}
	g_ce.pixels = cn->data;
	g_ce.width = w;
	g_ce.height = h;
	g_ce.surf->on_destroy = ce_destroy;
	g_ce.surf->on_resize = ce_resize;
	while (anx_theme_color_name(g_ce.count)) g_ce.count++;
	anx_iface_surface_set_title(g_ce.surf, "Colors");
	ce_status("Live edits require S to save");
	anx_wm_color_editor_redraw();
	if (anx_iface_surface_map(g_ce.surf) != ANX_OK ||
	    anx_wm_window_open(g_ce.surf) != ANX_OK) {
		anx_iface_surface_destroy(g_ce.surf);
		anx_wm_notify("Colors: cannot open window");
		return;
	}
	ce_render();
	return;
no_memory:
	anx_wm_notify("Colors: not enough memory");
}
