/*
 * wm_help.c — Keyboard shortcut help overlay (toggle with F1).
 *
 * Displays a centered read-only surface listing all WM hotkeys.
 * Dismissed by pressing F1, Escape, or clicking anywhere on screen.
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
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define HELP_W		480
#define HELP_ROW_H	22
#define HELP_PAD_X	12
#define HELP_PAD_Y	8
#define HELP_COL2_X	200   /* x-offset of description column */

/* ------------------------------------------------------------------ */
/* Hotkey table                                                        */
/* ------------------------------------------------------------------ */

static const struct {
	const char *keys;
	const char *desc;
} help_rows[] = {
	{ "Meta+1..9",        "Switch workspace"           },
	{ "Meta+Shift+1..9",  "Send window to workspace"   },
	{ "Meta+Tab",         "Cycle window focus"          },
	{ "Meta+Q",           "Close window"               },
	{ "Meta+M",           "Minimize / Restore"         },
	{ "Meta+F",           "Fullscreen toggle"          },
	{ "Meta+Return",      "Open terminal"              },
	{ "Meta+Space",       "Command search"             },
	{ "Meta+W",           "Workflow designer"          },
	{ "Meta+O",           "Object viewer"              },
	{ "Meta+C",           "Copy"                       },
	{ "Meta+V",           "Paste"                      },
	{ "Meta+Z",           "Undo"                       },
	{ "Meta+X",           "Cut"                        },
	{ "Meta+[",           "Tile left (snap left half)" },
	{ "Meta+]",           "Tile right (snap right half)"},
	{ "Meta+Shift+F",     "Float (restore from tile)"  },
	{ "Meta+Arrow",       "Nudge window position"      },
	{ "Meta+Shift+Arrow", "Resize window"              },
	{ "Meta+Shift+H",     "Halt system"                },
	{ "F1",               "Show / hide this help"      },
};
#define HELP_ROWS  ((uint32_t)(sizeof(help_rows) / sizeof(help_rows[0])))

#define HELP_H  (HELP_PAD_Y * 2 + 22 + HELP_ROWS * HELP_ROW_H + 4)

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct {
	struct anx_surface *surf;
	uint32_t           *pixels;
} g_help;

/* ------------------------------------------------------------------ */
/* Rendering helpers                                                   */
/* ------------------------------------------------------------------ */

static void h_draw_str(uint32_t x, uint32_t y, const char *s,
		        uint32_t fg, uint32_t bg)
{
	anx_font_blit_str(g_help.pixels, HELP_W, HELP_H, x, y, s, fg, bg);
}

static void h_fill_rect(uint32_t x, uint32_t y,
			 uint32_t w, uint32_t h, uint32_t color)
{
	uint32_t r, c;

	for (r = y; r < y + h && r < (uint32_t)HELP_H; r++)
		for (c = x; c < x + w && c < (uint32_t)HELP_W; c++)
			g_help.pixels[r * HELP_W + c] = color;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

bool anx_wm_help_active(void)
{
	return g_help.surf != NULL;
}

void anx_wm_help_close(void)
{
	if (!g_help.surf)
		return;
	anx_iface_surface_destroy(g_help.surf);
	g_help.surf = NULL;
	if (g_help.pixels) {
		anx_free(g_help.pixels);
		g_help.pixels = NULL;
	}
}

void anx_wm_help_toggle(void)
{
	const struct anx_fb_info *fb;
	struct anx_content_node  *cn;
	const struct anx_theme   *theme;
	uint32_t buf_size, i, row_y;
	int32_t sx, sy;

	if (anx_wm_help_active()) {
		anx_wm_help_close();
		return;
	}

	fb = anx_fb_get_info();
	if (!fb || !fb->available)
		return;

	buf_size = (uint32_t)(HELP_W * HELP_H) * 4u;
	g_help.pixels = anx_alloc(buf_size);
	if (!g_help.pixels)
		return;

	cn = anx_alloc(sizeof(*cn));
	if (!cn) {
		anx_free(g_help.pixels);
		g_help.pixels = NULL;
		return;
	}
	anx_memset(cn, 0, sizeof(*cn));
	cn->type     = ANX_CONTENT_CANVAS;
	cn->data     = g_help.pixels;
	cn->data_len = buf_size;

	sx = ((int32_t)fb->width  - HELP_W) / 2;
	sy = ((int32_t)fb->height - HELP_H) / 2;
	if (sx < 0) sx = 0;
	if (sy < 0) sy = 0;

	if (anx_iface_surface_create(ANX_ENGINE_RENDERER_GPU, cn,
				     sx, sy, HELP_W, HELP_H,
				     &g_help.surf) != ANX_OK) {
		anx_free(cn);
		anx_free(g_help.pixels);
		g_help.pixels = NULL;
		return;
	}

	theme = anx_theme_get();
	uint32_t bg     = theme->palette.surface;
	uint32_t fg     = theme->palette.text_primary;
	uint32_t dim    = theme->palette.text_dim;
	uint32_t accent = theme->palette.accent;
	uint32_t border = theme->palette.border;

	/* Background + border */
	h_fill_rect(0, 0, HELP_W, HELP_H, bg);
	for (i = 0; i < (uint32_t)HELP_W; i++) {
		g_help.pixels[i]                          = border;
		g_help.pixels[(HELP_H - 1) * HELP_W + i] = border;
	}
	for (i = 0; i < (uint32_t)HELP_H; i++) {
		g_help.pixels[i * HELP_W]              = border;
		g_help.pixels[i * HELP_W + HELP_W - 1] = border;
	}

	/* Header */
	h_fill_rect(1, 1, HELP_W - 2, 22, accent);
	h_draw_str(HELP_PAD_X, 1 + (22 - ANX_FONT_HEIGHT) / 2,
		   "Keyboard Shortcuts  (F1 or Esc to close)",
		   0x00FFFFFF, accent);

	/* Separator line below header */
	h_fill_rect(1, 23, HELP_W - 2, 2, border);

	/* Hotkey rows */
	row_y = HELP_PAD_Y + 24;
	for (i = 0; i < HELP_ROWS; i++, row_y += HELP_ROW_H) {
		uint32_t ty = row_y + (HELP_ROW_H - ANX_FONT_HEIGHT) / 2;

		if (i & 1)
			h_fill_rect(1, row_y, HELP_W - 2, HELP_ROW_H,
				    bg ^ 0x00060606u);
		h_draw_str(HELP_PAD_X, ty, help_rows[i].keys,  dim, ANX_FONT_TRANSPARENT);
		h_draw_str(HELP_COL2_X, ty, help_rows[i].desc, fg,  ANX_FONT_TRANSPARENT);
	}

	anx_iface_surface_map(g_help.surf);
	anx_iface_surface_raise(g_help.surf);
	anx_iface_surface_commit(g_help.surf);
}

/* Called from the WM key handler — returns true if consumed. */
bool anx_wm_help_key(uint32_t key)
{
	if (!anx_wm_help_active())
		return false;
	if (key == ANX_KEY_ESC || key == ANX_KEY_F1) {
		anx_wm_help_close();
		return true;
	}
	return true;  /* swallow all keys while overlay is open */
}
