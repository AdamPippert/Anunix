/*
 * arch_init.c — x86_64 architecture initialization for Framework devices.
 */

#include <anx/types.h>
#include <anx/arch.h>

void arch_early_init(void)
{
	/* TODO: Serial/VGA console, GDT, IDT, basic page tables */
}

void arch_init(void)
{
	/* TODO: APIC, ACPI parsing, full paging, PCI enumeration */
}

void arch_halt(void)
{
	for (;;)
		__asm__ volatile("hlt");
}

bool arch_irq_disable(void)
{
	uint64_t flags;

	__asm__ volatile(
		"pushfq\n\t"
		"pop %0\n\t"
		"cli"
		: "=r"(flags)
	);
	return (flags & 0x200) != 0; /* IF flag */
}

void arch_irq_enable(void)
{
	__asm__ volatile("sti");
}

void arch_irq_restore(bool state)
{
	if (state)
		arch_irq_enable();
}

anx_time_t arch_time_now(void)
{
	uint32_t lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	/* TODO: convert TSC ticks to nanoseconds */
	return ((uint64_t)hi << 32) | lo;
}

void arch_console_putc(char c)
{
	/* Serial port 0x3F8 (COM1) for early debug */
	__asm__ volatile("outb %0, %1" : : "a"((uint8_t)c), "Nd"((uint16_t)0x3F8));
}

void arch_console_puts(const char *s)
{
	while (*s)
		arch_console_putc(*s++);
}

void arch_mb(void)
{
	__asm__ volatile("mfence" ::: "memory");
}

void arch_rmb(void)
{
	__asm__ volatile("lfence" ::: "memory");
}

void arch_wmb(void)
{
	__asm__ volatile("sfence" ::: "memory");
}
