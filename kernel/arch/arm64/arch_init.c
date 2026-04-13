/*
 * arch_init.c — ARM64 architecture initialization for Apple Silicon.
 */

#include <anx/types.h>
#include <anx/arch.h>

void arch_early_init(void)
{
	/* TODO: UART init for early console, MMU setup, etc. */
}

void arch_init(void)
{
	/* TODO: GIC (interrupt controller), timers, full MMU */
}

void arch_halt(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

bool arch_irq_disable(void)
{
	uint64_t daif;

	__asm__ volatile("mrs %0, daif" : "=r"(daif));
	__asm__ volatile("msr daifset, #0xf");
	return (daif & 0x3c0) == 0; /* true if IRQs were enabled */
}

void arch_irq_enable(void)
{
	__asm__ volatile("msr daifclr, #0xf");
}

void arch_irq_restore(bool state)
{
	if (state)
		arch_irq_enable();
}

anx_time_t arch_time_now(void)
{
	uint64_t cnt;

	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
	/* TODO: convert to nanoseconds using cntfrq_el0 */
	return cnt;
}

void arch_console_putc(char c)
{
	/* TODO: UART output — platform-specific for Apple Silicon */
	(void)c;
}

void arch_console_puts(const char *s)
{
	while (*s)
		arch_console_putc(*s++);
}

void arch_mb(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

void arch_rmb(void)
{
	__asm__ volatile("dsb ld" ::: "memory");
}

void arch_wmb(void)
{
	__asm__ volatile("dsb st" ::: "memory");
}
