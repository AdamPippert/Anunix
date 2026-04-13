/*
 * anx/panic.h — Kernel panic and assertion macros.
 */

#ifndef ANX_PANIC_H
#define ANX_PANIC_H

#include <anx/types.h>

/* Halt the kernel with a diagnostic message */
void anx_panic(const char *file, int line, const char *msg)
	__attribute__((noreturn));

#define ANX_PANIC(msg) \
	anx_panic(__FILE__, __LINE__, (msg))

#define ANX_ASSERT(cond) \
	do { if (!(cond)) anx_panic(__FILE__, __LINE__, "assert: " #cond); } while (0)

#define ANX_ASSERT_MSG(cond, msg) \
	do { if (!(cond)) anx_panic(__FILE__, __LINE__, (msg)); } while (0)

#define ANX_BUG() \
	anx_panic(__FILE__, __LINE__, "BUG: unreachable code")

#endif /* ANX_PANIC_H */
