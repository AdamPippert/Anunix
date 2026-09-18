/* Real key routing and canvas assertions for the two desktop shell views. */
#include <anx/types.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/input.h>
#include <anx/interface_plane.h>
#include <anx/model_client.h>
#include <anx/shell.h>
#include <anx/string.h>
#include <anx/theme.h>
#include <anx/wm.h>
#include <anx/kprintf.h>

#define CHECK(c) do { if (!(c)) { rc = __LINE__; goto out; } } while (0)
#define ROW_H (ANX_FONT_HEIGHT + 4)

static uint32_t shell_ui_fb[640 * 480];
static uint32_t expected_row[640 * ANX_FONT_HEIGHT];

static void key(bool agent, uint32_t code, uint32_t unicode)
{
	if (agent) anx_wm_agent_key_event(code, 0, unicode);
	else anx_wm_terminal_key_event(code, 0, unicode);
}

static void type(bool agent, const char *text)
{
	while (*text) key(agent, ANX_KEY_NONE, (uint8_t)*text++);
}

static void submit(bool agent, const char *text)
{
	type(agent, text);
	key(agent, ANX_KEY_ENTER, 0);
}

static void flush(bool agent)
{
	if (agent) anx_wm_agent_flush_if_dirty();
	else anx_wm_terminal_flush_if_dirty();
}

/* Compare whole rows, so stale text and unwanted input strips are observable. */
static bool row_is(struct anx_surface *surf, uint32_t x, uint32_t y,
		   const char *text, uint32_t fg)
{
	uint32_t i, bg = anx_theme_get()->palette.background;
	uint32_t *pixels = surf->content_root->data;

	if (surf->buf_w > 640 || y + ANX_FONT_HEIGHT > surf->buf_h)
		return false;
	for (i = 0; i < surf->buf_w * ANX_FONT_HEIGHT; i++) expected_row[i] = bg;
	anx_font_blit_str(expected_row, surf->buf_w, ANX_FONT_HEIGHT,
			  x, 0, text, fg, bg);
	return !anx_memcmp(pixels + y * surf->buf_w, expected_row,
			   surf->buf_w * ANX_FONT_HEIGHT * sizeof(uint32_t));
}

static bool cursor_at(struct anx_surface *surf, uint32_t x, uint32_t y)
{
	uint32_t *pixels = surf->content_root->data;
	uint32_t i;

	if (x + 2 > surf->buf_w || y + ANX_FONT_HEIGHT > surf->buf_h) return false;
	for (i = 0; i < ANX_FONT_HEIGHT; i++)
		if (pixels[(y + i) * surf->buf_w + x] != anx_theme_get()->palette.accent)
			return false;
	return true;
}

static void resize(struct anx_surface *surf, uint32_t w, uint32_t h)
{
	surf->width = w;
	surf->height = h;
	surf->on_resize(surf);
}

int test_shell_ui(void)
{
	struct anx_fb_info old_fb = *anx_fb_get_info();
	struct anx_fb_info fb = {
		.addr = (uint64_t)(uintptr_t)shell_ui_fb,
		.width = 640, .height = 480, .pitch = 640 * 4,
		.bpp = 32, .available = true,
	};
	struct anx_wm_tiling_config old_tiling = anx_wm_tiling;
	struct anx_surface *term = NULL, *agent = NULL;
	uint32_t i, fg;
	char text[64];
	int rc = 0;

	CHECK(anx_fb_init(&fb) == ANX_OK);
	CHECK(anx_iface_init() == ANX_OK);
	CHECK(anx_renderer_gpu_register() == ANX_OK);
	CHECK(anx_model_client_configure(NULL) == ANX_OK);
	anx_theme_init(ANX_THEME_PRETTY);
	fg = anx_theme_get()->palette.text_primary;
	anx_wm_tiling.enabled = false;
	anx_wm_terminal_open();
	term = anx_wm_terminal_surface();
	CHECK(term);
	resize(term, 640, 200);
	submit(false, "clear");
	flush(false);
	CHECK(cursor_at(term, 4 + 2 * ANX_FONT_WIDTH, 8));
	submit(false, "echo UI_ALPHA");
	flush(false);
	CHECK(row_is(term, 4, 8, "> echo UI_ALPHA", fg));
	CHECK(row_is(term, 4, 8 + ROW_H, "UI_ALPHA", fg));
	CHECK(cursor_at(term, 4 + 2 * ANX_FONT_WIDTH, 8 + 2 * ROW_H));
	CHECK(row_is(term, 4, 8 + 4 * ROW_H, "", fg));

	/* Agent recalls a command entered in the terminal; Down restores its draft. */
	anx_wm_agent_open();
	agent = anx_wm_agent_surface();
	CHECK(agent);
	resize(agent, 640, 220);
	submit(true, "clear");
	anx_shell_history_record("echo UI_SHARED");
	type(true, "echo UI_DRAFT");
	key(true, ANX_KEY_UP, 0);
	key(true, ANX_KEY_DOWN, 0);
	key(true, ANX_KEY_ENTER, 0);
	flush(true);
	CHECK(row_is(agent, 12, 12 + ROW_H, "UI_DRAFT", fg));
	CHECK(cursor_at(agent, 12 + 2 * ANX_FONT_WIDTH, 12 + 2 * ROW_H));
	key(true, ANX_KEY_UP, 0);
	key(true, ANX_KEY_UP, 0);
	key(true, ANX_KEY_ENTER, 0);
	flush(true);
	CHECK(row_is(agent, 12, 12 + 3 * ROW_H, "UI_SHARED", fg));

	/* Terminal recalls the command just submitted from Agent. */
	key(false, ANX_KEY_UP, 0);
	key(false, ANX_KEY_ENTER, 0);
	flush(false);
	CHECK(row_is(term, 4, 8 + 3 * ROW_H, "UI_SHARED", fg));

	/* Reflow output after resize, and keep a long input's cursor visible. */
	submit(false, "clear");
	submit(false, "echo abcdefghijklmno");
	resize(term, 128, 240);
	CHECK(row_is(term, 4, 8 + 3 * ROW_H, "abcdefghij", fg));
	CHECK(row_is(term, 4, 8 + 4 * ROW_H, "klmno", fg));
	resize(term, 400, 200);
	CHECK(row_is(term, 4, 8 + ROW_H, "abcdefghijklmno", fg));
	submit(false, "clear");
	resize(term, 88, 140);
	for (i = 0; i < 50; i++) key(false, ANX_KEY_NONE, 'x');
	CHECK(cursor_at(term, 4 + (52 % 6) * ANX_FONT_WIDTH, 8 + 3 * ROW_H));
	anx_wm_terminal_clear_input();
	submit(true, "clear");
	resize(agent, 88, 140);
	for (i = 0; i < 50; i++) key(true, ANX_KEY_NONE, 'x');
	flush(true);
	CHECK(cursor_at(agent, 12 + (52 % 5) * ANX_FONT_WIDTH, 12 + 3 * ROW_H));
	key(true, ANX_KEY_ESC, 0);

	/* Ring wrap retains only the newest 200 terminal/300 Agent logical lines. */
	resize(term, 640, 200);
	for (i = 0; i < 210; i++) {
		anx_snprintf(text, sizeof(text), "row%03u", i);
		anx_wm_terminal_print(text);
	}
	for (i = 0; i < 50; i++) key(false, ANX_KEY_PAGEUP, 0);
	CHECK(row_is(term, 4, 8, "row010", fg));
	for (i = 0; i < 50; i++) key(false, ANX_KEY_PAGEDOWN, 0);
	CHECK(cursor_at(term, 4 + 2 * ANX_FONT_WIDTH, 8 + 5 * ROW_H));
	resize(agent, 640, 220);
	for (i = 0; i < 310; i++) key(true, ANX_KEY_ENTER, 0);
	for (i = 0; i < 60; i++) key(true, ANX_KEY_PAGEUP, 0);
	flush(true);
	CHECK(row_is(agent, 12, 12, "> ", anx_theme_get()->palette.accent));
	for (i = 0; i < 60; i++) key(true, ANX_KEY_PAGEDOWN, 0);
	flush(true);
	CHECK(cursor_at(agent, 12 + 2 * ANX_FONT_WIDTH, 12 + 6 * ROW_H));
out:
	if (anx_wm_terminal_surface()) anx_wm_window_close(anx_wm_terminal_surface());
	if (anx_wm_agent_surface()) anx_wm_window_close(anx_wm_agent_surface());
	anx_wm_tiling = old_tiling;
	anx_fb_init(&old_fb);
	return rc ? -rc : 0;
}
