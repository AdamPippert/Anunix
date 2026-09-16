/*
 * fbcon.c — Framebuffer text console.
 *
 * Provides a character-cell text console on top of the framebuffer.
 * Tracks cursor position, handles line wrapping and scrolling.
 * Designed to be wired into arch_console_putc alongside serial.
 */

#include <anx/types.h>
#include <anx/fb.h>
#include <anx/font.h>
#include <anx/fbcon.h>
#include <anx/gui.h>

/* Console colors */
#define FBCON_FG	0x00CCCCCC	/* light gray text */
#define FBCON_BG	0x00000000	/* black background */

#define TAB_WIDTH	8

static bool fbcon_ready;
static uint32_t con_cols;
static uint32_t con_rows;
static uint32_t cur_x;		/* cursor column (character cells) */
static uint32_t cur_y;		/* cursor row (character cells) */

int anx_fbcon_init(void)
{
	const struct anx_fb_info *info;

	if (!anx_fb_available())
		return ANX_EINVAL;

	info = anx_fb_get_info();
	con_cols = info->width / ANX_FONT_WIDTH;
	con_rows = info->height / ANX_FONT_HEIGHT;
	cur_x = 0;
	cur_y = 0;
	fbcon_ready = true;

	anx_fb_clear(FBCON_BG);

	return ANX_OK;
}

bool anx_fbcon_active(void)
{
	return fbcon_ready;
}

uint32_t anx_fbcon_cols(void)
{
	return con_cols;
}

uint32_t anx_fbcon_rows(void)
{
	return con_rows;
}

uint32_t anx_fbcon_cursor_x(void)
{
	return cur_x;
}

uint32_t anx_fbcon_cursor_y(void)
{
	return cur_y;
}

/*
 * Reaching the bottom of a framebuffer console can be handled two ways, and
 * on a high-resolution panel they cost wildly different amounts.
 *
 * Scrolling moves the visible framebuffer up. That is a read followed by a
 * write of nearly the whole screen -- about 16 MiB each way on a 2560x1600
 * panel -- and the read side comes back from video memory, which is far
 * slower than main memory. Every line of boot output paid for one.
 *
 * Paging throws the screen away and starts again at the top. It never reads
 * the framebuffer at all: one linear write per screenful, and writes to a
 * write-combining framebuffer are the fast direction. A screen of output
 * costs one clear instead of dozens of moves.
 *
 * Boot output is paged. Nothing above the current screen is being read by
 * anyone mid-boot, and the serial log and the persisted boot log both keep
 * the full text, so the history is not lost -- only the pixels are.
 *
 * anx_fbcon_set_paging() exists so an interactive console can choose
 * scrolling, where losing the screen above the cursor would matter.
 */
#define FBCON_SCROLL_ROWS	12

static bool con_paging = true;

void anx_fbcon_set_paging(bool paging)
{
	con_paging = paging;
}

static void fbcon_scroll_rows(uint32_t rows)
{
	if (rows == 0)
		return;
	if (rows > con_rows)
		rows = con_rows;
	anx_fb_scroll(rows * ANX_FONT_HEIGHT, FBCON_BG);
}

static void fbcon_newline(void)
{
	cur_x = 0;
	cur_y++;
	if (cur_y < con_rows)
		return;

	if (con_paging) {
		anx_fb_clear(FBCON_BG);
		cur_y = 0;
	} else {
		uint32_t rows = FBCON_SCROLL_ROWS;

		if (rows >= con_rows)
			rows = con_rows > 1 ? con_rows - 1 : 1;
		fbcon_scroll_rows(rows);
		cur_y = con_rows - rows;
	}
}

static void fbcon_draw_at_cursor(char c)
{
	uint32_t px = cur_x * ANX_FONT_WIDTH;
	uint32_t py = cur_y * ANX_FONT_HEIGHT;

	anx_font_draw_char(px, py, c, FBCON_FG, FBCON_BG);
}

void anx_fbcon_putc(char c)
{
	if (!fbcon_ready)
		return;

	/* Route through GUI terminal when active */
	if (anx_gui_active()) {
		anx_gui_terminal_putc(c);
		return;
	}

	switch (c) {
	case '\n':
		fbcon_newline();
		return;

	case '\r':
		cur_x = 0;
		return;

	case '\b':
		if (cur_x > 0) {
			cur_x--;
			/* Erase the character at the new position */
			fbcon_draw_at_cursor(' ');
		}
		return;

	case '\t': {
		uint32_t next = (cur_x + TAB_WIDTH) & ~(TAB_WIDTH - 1);

		if (next >= con_cols)
			next = con_cols - 1;
		while (cur_x < next) {
			fbcon_draw_at_cursor(' ');
			cur_x++;
		}
		return;
	}

	default:
		break;
	}

	/* Printable character */
	fbcon_draw_at_cursor(c);
	cur_x++;

	if (cur_x >= con_cols)
		fbcon_newline();
}

void anx_fbcon_puts(const char *s)
{
	while (*s)
		anx_fbcon_putc(*s++);
}

void anx_fbcon_clear(void)
{
	if (!fbcon_ready)
		return;

	anx_fb_clear(FBCON_BG);
	cur_x = 0;
	cur_y = 0;
}

void anx_fbcon_disable(void)
{
	fbcon_ready = false;
	anx_fb_clear(0x00000000);	/* black — WM will paint over */
}
