/* Meta+Enter's per-surface terminal, through its real event handler. */
#include <anx/types.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/input.h>
#include <anx/interface_plane.h>
#include <anx/string.h>
#include <anx/theme.h>
#include <anx/wm.h>

#define CHECK(c) do { if (!(c)) { rc = __LINE__; goto out; } } while (0)

static uint32_t native_fb[640 * 480];
static uint32_t expected[640 * ANX_FONT_HEIGHT];

static void key(struct anx_surface *surf, uint32_t code, uint32_t unicode)
{
	struct anx_event ev = {0};

	ev.type = ANX_EVENT_KEY_DOWN;
	ev.data.key.keycode = code;
	ev.data.key.unicode = unicode;
	surf->on_event(surf, &ev);
}

static void type(struct anx_surface *surf, const char *text)
{
	while (*text) key(surf, ANX_KEY_NONE, (uint8_t)*text++);
}

static void submit(struct anx_surface *surf, const char *text)
{
	type(surf, text);
	key(surf, ANX_KEY_ENTER, 0);
}

static bool row_is(struct anx_surface *surf, uint32_t row, const char *text)
{
	uint32_t i, y = 4 + row * ANX_FONT_HEIGHT;
	const struct anx_theme *theme = anx_theme_get();
	uint32_t *pixels = surf->content_root->data;

	if (surf->buf_w > 640 || y + ANX_FONT_HEIGHT > surf->buf_h) return false;
	for (i = 0; i < surf->buf_w * ANX_FONT_HEIGHT; i++)
		expected[i] = theme->palette.background;
	anx_font_blit_str(expected, surf->buf_w, ANX_FONT_HEIGHT, 6, 0,
			  text, theme->palette.text_primary, theme->palette.background);
	return !anx_memcmp(pixels + y * surf->buf_w, expected,
			   surf->buf_w * ANX_FONT_HEIGHT * sizeof(uint32_t));
}

static bool cursor_at(struct anx_surface *surf, uint32_t row, uint32_t col)
{
	uint32_t *pixels = surf->content_root->data;
	uint32_t i, x = 6 + col * ANX_FONT_WIDTH, y = 4 + row * ANX_FONT_HEIGHT;

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

int test_native_terminal(void)
{
	struct anx_fb_info old_fb = *anx_fb_get_info();
	struct anx_fb_info fb = {
		.addr = (uint64_t)(uintptr_t)native_fb,
		.width = 640, .height = 480, .pitch = 640 * 4,
		.bpp = 32, .available = true,
	};
	struct anx_wm_tiling_config old_tiling = anx_wm_tiling;
	struct anx_surface *first = NULL, *second = NULL;
	uint32_t i;
	int rc = 0;

	CHECK(anx_fb_init(&fb) == ANX_OK);
	CHECK(anx_iface_init() == ANX_OK);
	CHECK(anx_renderer_gpu_register() == ANX_OK);
	anx_theme_init(ANX_THEME_PRETTY);
	anx_wm_tiling.enabled = false;
	anx_wm_hotkeys_init();
	/* Launch exactly the implementation used by Meta+Enter. */
	CHECK(anx_wm_hotkey_dispatch(ANX_MOD_META, ANX_KEY_ENTER));
	first = anx_wm_focused_window();
	CHECK(first && first->on_event && !anx_strcmp(first->title, "ansh"));
	CHECK(first->y - (int32_t)ANX_WM_DECOR_H - (int32_t)anx_wm_tiling.border_w >=
	      (int32_t)ANX_WM_MENUBAR_H);
	CHECK((uint32_t)first->y + first->height + anx_wm_tiling.border_w <=
	      fb.height - ANX_WM_TASKBAR_H);
	resize(first, 640, 200);
	submit(first, "clear");
	CHECK(cursor_at(first, 0, 5));
	CHECK(row_is(first, 5, ""));
	submit(first, "echo TOP");
	CHECK(row_is(first, 0, "anx> echo TOP"));
	CHECK(row_is(first, 1, "TOP"));
	CHECK(cursor_at(first, 2, 5));
	CHECK(row_is(first, 5, ""));

	/* A font change must refresh already-open cached terminal pixels. */
	CHECK(anx_theme_apply_config_checked("font_family=cascadia-mono") == ANX_OK);
	anx_wm_repaint_all();
	CHECK(row_is(first, 1, "TOP"));

	/* Separate terminals recall shared commands while retaining separate drafts. */
	CHECK(anx_wm_hotkey_dispatch(ANX_MOD_META, ANX_KEY_ENTER));
	second = anx_wm_focused_window();
	CHECK(second && second != first && second->on_event);
	resize(second, 640, 240);
	submit(second, "clear");
	submit(first, "clear");
	submit(first, "echo SHARED");
	type(first, "echo ONE");
	type(second, "echo TWO");
	key(first, ANX_KEY_UP, 0);
	key(second, ANX_KEY_UP, 0);
	key(first, ANX_KEY_DOWN, 0);
	key(second, ANX_KEY_DOWN, 0);
	key(first, ANX_KEY_ENTER, 0);
	key(second, ANX_KEY_ENTER, 0);
	CHECK(row_is(first, 3, "ONE"));
	CHECK(row_is(second, 1, "TWO"));
	key(second, ANX_KEY_UP, 0);
	key(second, ANX_KEY_UP, 0);
	key(second, ANX_KEY_UP, 0);
	key(second, ANX_KEY_ENTER, 0);
	CHECK(row_is(second, 3, "SHARED"));

	/* The same stored output reflows when the actual surface is resized. */
	submit(first, "clear");
	submit(first, "echo abcdefghijklmno");
	resize(first, 132, 200);
	CHECK(row_is(first, 3, "abcdefghij"));
	CHECK(row_is(first, 4, "klmno"));
	CHECK(cursor_at(first, 5, 5));
	resize(first, 400, 200);
	CHECK(row_is(first, 1, "abcdefghijklmno"));
	CHECK(cursor_at(first, 2, 5));
	submit(first, "clear");
	resize(first, 84, 140);
	for (i = 0; i < 50; i++) key(first, ANX_KEY_NONE, 'x');
	CHECK(cursor_at(first, 4, 1));
	key(first, ANX_KEY_BACKSPACE, 0);
	CHECK(cursor_at(first, 4, 0));
	for (i = 0; i < 49; i++) key(first, ANX_KEY_BACKSPACE, 0);

	/* Cursor editing must execute the changed command, not merely move pixels. */
	resize(first, 640, 200);
	submit(first, "clear");
	type(first, "echo AXC");
	key(first, ANX_KEY_LEFT, 0);
	key(first, ANX_KEY_LEFT, 0);
	CHECK(cursor_at(first, 0, 11));
	key(first, ANX_KEY_DELETE, 0);
	type(first, "B");
	key(first, ANX_KEY_HOME, 0);
	key(first, ANX_KEY_BACKSPACE, 0);
	for (i = 0; i < 5; i++) key(first, ANX_KEY_RIGHT, 0);
	type(first, "Z");
	key(first, ANX_KEY_BACKSPACE, 0);
	key(first, ANX_KEY_END, 0);
	key(first, ANX_KEY_DELETE, 0);
	key(first, ANX_KEY_ENTER, 0);
	CHECK(row_is(first, 1, "ABC"));
	key(first, ANX_KEY_UP, 0);
	key(first, ANX_KEY_LEFT, 0);
	key(first, ANX_KEY_BACKSPACE, 0);
	type(first, "D");
	key(first, ANX_KEY_ENTER, 0);
	CHECK(row_is(first, 3, "ADC"));
	submit(first, "clear");
	resize(first, 84, 140);
	for (i = 0; i < 50; i++) type(first, "x");
	key(first, ANX_KEY_HOME, 0);
	CHECK(cursor_at(first, 0, 5));
	key(first, ANX_KEY_END, 0);
	CHECK(cursor_at(first, 4, 1));
	for (i = 0; i < 50; i++) key(first, ANX_KEY_BACKSPACE, 0);

	/* Discarded output must never reappear after paging past ring capacity. */
	resize(first, 640, 200);
	submit(first, "echo OLD_OUTPUT");
	for (i = 0; i < 205; i++) key(first, ANX_KEY_ENTER, 0);
	for (i = 0; i < 40; i++) key(first, ANX_KEY_PAGEUP, 0);
	CHECK(row_is(first, 0, "anx> "));
	for (i = 0; i < 40; i++) key(first, ANX_KEY_PAGEDOWN, 0);
	CHECK(cursor_at(first, 6, 5));
	/* External close releases the slot; a new Meta+Enter gets a clean draft. */
	CHECK(anx_wm_window_close(first) == ANX_OK);
	first = NULL;
	CHECK(anx_wm_hotkey_dispatch(ANX_MOD_META, ANX_KEY_ENTER));
	first = anx_wm_focused_window();
	CHECK(first && first->on_event && first != second);
	submit(first, "clear");
	CHECK(cursor_at(first, 0, 5));
	/* Tiny allocations must not underflow row calculations. */
	resize(first, 1, 1);
	CHECK(first->buf_w == 1 && first->buf_h == 1);
	key(first, ANX_KEY_PAGEUP, 0);
out:
	if (first) anx_wm_window_close(first);
	if (second) anx_wm_window_close(second);
	anx_wm_tiling = old_tiling;
	anx_fb_init(&old_fb);
	return rc ? -rc : 0;
}
