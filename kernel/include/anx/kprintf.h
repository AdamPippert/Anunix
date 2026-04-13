/*
 * anx/kprintf.h — Kernel printf for diagnostics.
 */

#ifndef ANX_KPRINTF_H
#define ANX_KPRINTF_H

#include <anx/types.h>

/* Minimal printf — supports %s, %d, %u, %x, %p, %c, %% */
int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* ANX_KPRINTF_H */
