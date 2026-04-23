/*
 * display.c — Display diagnostics and visual mode switching.
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/fb.h>
#include <anx/fbcon.h>
#include <anx/gui.h>
#include <anx/theme.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* ------------------------------------------------------------------ */
/* clear                                                                */
/* ------------------------------------------------------------------ */

void cmd_clear(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	if (anx_gui_active())
		anx_gui_terminal_clear();
	else
		anx_fbcon_clear();
}

/* ------------------------------------------------------------------ */
/* mode — runtime visual mode switching with state-object persistence  */
/* ------------------------------------------------------------------ */

struct mode_snapshot {
	struct anx_theme theme;
	int32_t          tz_offset;
};

static anx_oid_t g_mode_snapshot_oid;
static bool      g_mode_snapshot_valid;

static void mode_save_snapshot(void)
{
	struct mode_snapshot snap;
	struct anx_so_create_params p;
	struct anx_state_object *obj;
	int r;

	snap.theme     = *anx_theme_get();
	snap.tz_offset = anx_gui_get_tz_offset();

	if (g_mode_snapshot_valid) {
		anx_so_delete(&g_mode_snapshot_oid, true);
		g_mode_snapshot_valid = false;
	}

	p.object_type    = ANX_OBJ_BYTE_DATA;
	p.schema_uri     = NULL;
	p.schema_version = NULL;
	p.payload        = &snap;
	p.payload_size   = sizeof(snap);
	p.parent_oids    = NULL;
	p.parent_count   = 0;
	anx_memset(&p.creator_cell, 0, sizeof(p.creator_cell));

	obj = NULL;
	r   = anx_so_create(&p, &obj);
	if (r == ANX_OK && obj) {
		g_mode_snapshot_oid   = obj->oid;
		g_mode_snapshot_valid = true;
		anx_objstore_release(obj);
	}
}

static void mode_restore_snapshot(void)
{
	struct anx_object_handle h;
	struct mode_snapshot snap;
	int r;

	if (!g_mode_snapshot_valid) {
		anx_theme_set_mode(ANX_THEME_PRETTY);
		return;
	}

	r = anx_so_open(&g_mode_snapshot_oid, ANX_OPEN_READ, &h);
	if (r != ANX_OK) {
		anx_theme_set_mode(ANX_THEME_PRETTY);
		return;
	}

	r = anx_so_read_payload(&h, 0, &snap, sizeof(snap));
	anx_so_close(&h);

	if (r == ANX_OK) {
		anx_theme_restore(&snap.theme);
		anx_gui_set_tz_offset(snap.tz_offset);
	} else {
		anx_theme_set_mode(ANX_THEME_PRETTY);
	}
}

int cmd_mode(int argc, char **argv)
{
	if (argc < 2) {
		kprintf("mode: %s\n",
			anx_theme_get_mode() == ANX_THEME_PRETTY
			? "pretty" : "boring");
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "boring") == 0) {
		if (anx_theme_get_mode() == ANX_THEME_BORING) {
			kprintf("mode: already boring\n");
			return ANX_OK;
		}
		mode_save_snapshot();
		anx_gui_disable();
		anx_fbcon_clear();
		anx_theme_set_mode(ANX_THEME_BORING);
		kprintf("mode: boring\n");
		return ANX_OK;
	}

	if (anx_strcmp(argv[1], "pretty") == 0) {
		if (anx_theme_get_mode() == ANX_THEME_PRETTY) {
			kprintf("mode: already pretty\n");
			return ANX_OK;
		}
		mode_restore_snapshot();
		anx_gui_init();
		kprintf("mode: pretty\n");
		return ANX_OK;
	}

	kprintf("usage: mode [pretty|boring]\n");
	return ANX_EINVAL;
}

/* ------------------------------------------------------------------ */
/* fb_info                                                              */
/* ------------------------------------------------------------------ */

void cmd_fb_info(int argc, char **argv)
{
	const struct anx_fb_info *fb;

	(void)argc;
	(void)argv;

	fb = anx_fb_get_info();
	if (!fb || !fb->available) {
		kprintf("{\"available\":false}\n");
		return;
	}
	kprintf("{\"available\":true,\"width\":%u,\"height\":%u,"
		"\"pitch\":%u,\"bpp\":%u}\n",
		fb->width, fb->height, fb->pitch, (uint32_t)fb->bpp);
}

/* ------------------------------------------------------------------ */
/* gop_list                                                             */
/* ------------------------------------------------------------------ */

void cmd_gop_list(int argc, char **argv)
{
	const struct anx_gop_mode *modes;
	uint8_t count, current;
	uint8_t i;

	(void)argc;
	(void)argv;

	modes = anx_fb_get_gop_modes(&count, &current);
	if (count == 0) {
		kprintf("gop_list: no GOP mode data (QEMU or multiboot2 path)\n");
		return;
	}

	kprintf("GOP modes available at boot (%u total, * = selected):\n",
		(uint32_t)count);
	for (i = 0; i < count; i++) {
		kprintf("  %s[%u] %ux%u (GOP mode %u)\n",
			(i == current) ? "*" : " ",
			(uint32_t)i,
			modes[i].width,
			modes[i].height,
			modes[i].mode_number);
	}
}

/* ------------------------------------------------------------------ */
/* fb_test — paint a recognizable pattern to verify fb geometry        */
/* ------------------------------------------------------------------ */

/*
 * Paints 8 equal-height color bars left-to-right:
 *   black, white, red, green, blue, cyan, magenta, yellow
 * This is the classic SMPTE-style test signal and immediately
 * reveals pitch/width/bpp errors (bars will be skewed or wrong color).
 */
static const uint32_t test_colors[8] = {
	0x00000000, /* black   */
	0x00FFFFFF, /* white   */
	0x00FF0000, /* red     */
	0x0000FF00, /* green   */
	0x000000FF, /* blue    */
	0x0000FFFF, /* cyan    */
	0x00FF00FF, /* magenta */
	0x00FFFF00, /* yellow  */
};

void cmd_fb_test(int argc, char **argv)
{
	const struct anx_fb_info *fb;
	uint32_t bar_w, x, y, bar;

	(void)argc;
	(void)argv;

	fb = anx_fb_get_info();
	if (!fb || !fb->available) {
		kprintf("fb_test: framebuffer not available\n");
		return;
	}

	bar_w = fb->width / 8;

	for (bar = 0; bar < 8; bar++) {
		uint32_t x0 = bar * bar_w;
		uint32_t x1 = (bar == 7) ? fb->width : x0 + bar_w;

		for (y = 0; y < fb->height; y++) {
			uint32_t *row = anx_fb_row_ptr(y);

			for (x = x0; x < x1; x++)
				row[x] = test_colors[bar];
		}
	}

	kprintf("fb_test: painted 8-bar pattern at %ux%u\n",
		fb->width, fb->height);
}
