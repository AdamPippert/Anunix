/*
 * arch_init.c — ARM64 architecture initialization.
 *
 * For QEMU virt machine: PL011 UART at 0x09000000.
 * For real Apple Silicon: different UART base (deferred).
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/page.h>

/* Linker-defined heap region */
extern char _heap_start[];
extern char _heap_end[];

/* --- PL011 UART (QEMU virt machine) --- */

#define PL011_BASE	0x09000000ULL

#define PL011_DR	(PL011_BASE + 0x000)	/* data register */
#define PL011_FR	(PL011_BASE + 0x018)	/* flag register */
#define PL011_IBRD	(PL011_BASE + 0x024)	/* integer baud rate */
#define PL011_FBRD	(PL011_BASE + 0x028)	/* fractional baud rate */
#define PL011_LCR_H	(PL011_BASE + 0x02C)	/* line control */
#define PL011_CR	(PL011_BASE + 0x030)	/* control register */
#define PL011_IMSC	(PL011_BASE + 0x038)	/* interrupt mask */

/* Flag register bits */
#define PL011_FR_TXFF	(1 << 5)	/* TX FIFO full */
#define PL011_FR_RXFE	(1 << 4)	/* RX FIFO empty */

static inline void mmio_write32(uint64_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uint64_t addr)
{
	return *(volatile uint32_t *)addr;
}

static void pl011_init(void)
{
	/* Disable UART */
	mmio_write32(PL011_CR, 0);

	/* Clear pending interrupts */
	mmio_write32(PL011_IMSC, 0);

	/*
	 * Set baud rate: 115200 at 24 MHz UART clock (QEMU default).
	 * Divisor = 24000000 / (16 * 115200) = 13.0208
	 * IBRD = 13, FBRD = round(0.0208 * 64) = 1
	 */
	mmio_write32(PL011_IBRD, 13);
	mmio_write32(PL011_FBRD, 1);

	/* 8N1, enable FIFOs */
	mmio_write32(PL011_LCR_H, (3 << 5) | (1 << 4));

	/* Enable UART, TX, RX */
	mmio_write32(PL011_CR, (1 << 0) | (1 << 8) | (1 << 9));
}

/* --- Initialization --- */

void arch_early_init(void)
{
	pl011_init();
}

void arch_init(void)
{
	/* Initialize page allocator with linker-defined heap */
	anx_page_init((uintptr_t)_heap_start, (uintptr_t)_heap_end);

	/* TODO: GIC, timers, full MMU */
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

/* --- Console I/O --- */

void arch_console_putc(char c)
{
	/* Wait for TX FIFO space */
	while (mmio_read32(PL011_FR) & PL011_FR_TXFF)
		;
	mmio_write32(PL011_DR, (uint32_t)c);
}

void arch_console_puts(const char *s)
{
	while (*s)
		arch_console_putc(*s++);
}

int arch_console_getc(void)
{
	/* Poll until RX FIFO has data */
	while (mmio_read32(PL011_FR) & PL011_FR_RXFE)
		;
	return (int)(mmio_read32(PL011_DR) & 0xFF);
}

bool arch_console_has_input(void)
{
	return (mmio_read32(PL011_FR) & PL011_FR_RXFE) == 0;
}

/* --- Memory barriers --- */

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
