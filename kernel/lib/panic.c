/*
 * panic.c — Kernel panic implementation.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/kprintf.h>
#include <anx/panic.h>

void anx_panic(const char *file, int line, const char *msg)
{
	arch_irq_disable();
	kprintf("\n*** KERNEL PANIC ***\n");
	kprintf("%s:%d: %s\n", file, line, msg);
	arch_halt();
}
