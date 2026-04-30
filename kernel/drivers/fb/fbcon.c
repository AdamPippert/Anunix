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

static void fbcon_scroll_one_line(void)
{
	anx_fb_scroll(ANX_FONT_HEIGHT, FBCON_BG);
}

static void fbcon_newline(void)
{
	cur_x = 0;
	cur_y++;
	if (cur_y >= con_rows) {
		cur_y = con_rows - 1;
		fbcon_scroll_one_line();
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
