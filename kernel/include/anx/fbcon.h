/*
 * anx/fbcon.h — Framebuffer text console.
 *
 * Provides a character-cell console on top of the framebuffer.
 * Tracks cursor position, handles scrolling, and supports
 * basic text output. Designed to be wired into arch_console_putc
 * alongside serial output.
 */

#ifndef ANX_FBCON_H
#define ANX_FBCON_H

#include <anx/types.h>

/* Initialize the framebuffer console (call after anx_fb_init) */
int anx_fbcon_init(void);

/* Query whether fbcon is active */
bool anx_fbcon_active(void);

/* Write a single character at current cursor, advance cursor */
void anx_fbcon_putc(char c);

/* Write a string */
void anx_fbcon_puts(const char *s);

/* Clear the console */
void anx_fbcon_clear(void);

/* Disable fbcon — WM takes ownership of the framebuffer */
void anx_fbcon_disable(void);

/* Get console dimensions in character cells */
uint32_t anx_fbcon_cols(void);
uint32_t anx_fbcon_rows(void);

/* Get current cursor position */
uint32_t anx_fbcon_cursor_x(void);
uint32_t anx_fbcon_cursor_y(void);

#endif /* ANX_FBCON_H */
