/*
 * anx/kprintf.h — Kernel printf for diagnostics.
 */

#ifndef ANX_KPRINTF_H
#define ANX_KPRINTF_H

#include <anx/types.h>

/* Minimal printf — supports %s, %d, %u, %x, %p, %c, %% */
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Single character output to all consoles (serial + framebuffer) */
void kputc(char c);

/* Set a capture buffer — kprintf output goes to buffer AND console */
void anx_kprintf_capture_start(char *buf, uint32_t buf_size);

/* Stop capturing, return bytes captured */
uint32_t anx_kprintf_capture_stop(void);

#endif /* ANX_KPRINTF_H */
