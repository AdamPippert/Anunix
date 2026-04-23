/*
 * anx/gui.h — Graphical user environment.
 *
 * Aether design language: deep navy wallpaper, floating topbar pill with
 * 1 px teal accent border, warm paper-white terminal text. Navy-to-teal
 * gradient brand palette derived from the Anunix logo.
 */

#ifndef ANX_GUI_H
#define ANX_GUI_H

#include <anx/types.h>

/* Aether color palette — 0x00RRGGBB */
#define ANX_COLOR_AX_BG       0x000B1A2B  /* deep navy:  desktop wallpaper */
#define ANX_COLOR_AX_PANEL    0x00163454  /* navy-800:   topbar / floating panels */
#define ANX_COLOR_AX_SURFACE  0x000E2338  /* navy-900:   terminal / window bg */
#define ANX_COLOR_AX_TEAL     0x003A94A6  /* teal-500:   accent, active border */
#define ANX_COLOR_AX_TEXT     0x00F7F5F1  /* warm white: terminal body text */
#define ANX_COLOR_WHITE       0x00FFFFFF  /* pure white: clock, headings */
#define ANX_COLOR_BLACK       0x00000000

/* Legacy aliases used by renderer_gpu.c and other external callers */
#define ANX_COLOR_SKY_BLUE    ANX_COLOR_AX_BG
#define ANX_COLOR_MIDNIGHT    ANX_COLOR_AX_SURFACE

/* Gap between shell elements — matches Aether 14 px grid spacing */
#define ANX_GUI_MARGIN  14

/* Initialize the graphical environment */
void anx_gui_init(void);

/* Draw a character at pixel position with scale factor */
void anx_gui_draw_char_scaled(uint32_t x, uint32_t y, char c,
			       uint32_t fg, uint32_t bg, uint32_t scale);

/* Draw a string at pixel position with scale factor */
void anx_gui_draw_string_scaled(uint32_t x, uint32_t y, const char *s,
				 uint32_t fg, uint32_t bg, uint32_t scale);

/* Fill buf with current local time as "HH:MM\0" (buflen >= 6) */
void anx_gui_get_time(char *buf, uint32_t buflen);

/* Update the time display in the top bar */
void anx_gui_update_time(void);

/* Write a character to the GUI terminal (fbcon replacement) */
void anx_gui_terminal_putc(char c);

/* Set UTC offset in hours (e.g., -7 for PDT, -8 for PST) */
void anx_gui_set_tz_offset(int32_t hours);

/* Check if GUI is active */
bool anx_gui_active(void);

/* Clear the GUI terminal area and reset the cursor */
void anx_gui_terminal_clear(void);

/* Disable the GUI (switches fbcon routing back to boring text mode) */
void anx_gui_disable(void);

/* Return the current UTC offset set via anx_gui_set_tz_offset() */
int32_t anx_gui_get_tz_offset(void);

#endif /* ANX_GUI_H */
