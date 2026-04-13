/*
 * main.c — Anunix kernel entry point.
 *
 * Called by architecture-specific boot code after basic hardware init.
 * This is the architecture-independent starting point for the kernel.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/kprintf.h>

#define ANX_VERSION "0.1.0"

void kernel_main(void)
{
	kprintf("Anunix %s booting\n", ANX_VERSION);

	/* Architecture-specific full init */
	arch_init();

	kprintf("arch init complete\n");

	/* TODO: Initialize subsystems in dependency order:
	 *   1. State Object Layer  (RFC-0002)
	 *   2. Execution Cell Runtime (RFC-0003)
	 *   3. Memory Control Plane (RFC-0004)
	 *   4. Routing + Scheduler (RFC-0005)
	 *   5. Network Plane (RFC-0006)
	 *   6. Capability Objects (RFC-0007)
	 *   7. POSIX compatibility layer
	 */

	kprintf("kernel init complete, halting\n");
	arch_halt();
}
