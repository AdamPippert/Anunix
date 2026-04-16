/*
 * anx/gui.h — Graphical user environment.
 *
 * Simple tiled window manager with a time bar and terminal window.
 * Sky blue background, midnight blue terminal, white text.
 */

#ifndef ANX_GUI_H
#define ANX_GUI_H

#include <anx/types.h>

/* Color scheme */
#define ANX_COLOR_SKY_BLUE	0x0087CEEB
#define ANX_COLOR_MIDNIGHT	0x00191970
#define ANX_COLOR_WHITE		0x00FFFFFF
#define ANX_COLOR_BLACK		0x00000000

/* GUI padding */
#define ANX_GUI_MARGIN		30
#define ANX_GUI_TOPBAR_HEIGHT	40

/* Initialize the graphical environment */
void anx_gui_init(void);

/* Draw a character at pixel position with scale factor */
void anx_gui_draw_char_scaled(uint32_t x, uint32_t y, char c,
			       uint32_t fg, uint32_t bg, uint32_t scale);

/* Draw a string at pixel position with scale factor */
void anx_gui_draw_string_scaled(uint32_t x, uint32_t y, const char *s,
				 uint32_t fg, uint32_t bg, uint32_t scale);

/* Update the time display in the top bar */
void anx_gui_update_time(void);

/* Write a character to the GUI terminal (fbcon replacement) */
void anx_gui_terminal_putc(char c);

/* Set UTC offset in hours (e.g., -7 for PDT, -8 for PST) */
void anx_gui_set_tz_offset(int32_t hours);

/* Check if GUI is active */
bool anx_gui_active(void);

#endif /* ANX_GUI_H */
