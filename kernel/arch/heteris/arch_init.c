/*
 * arch_init.c — Heteris architecture initialization.
 *
 * Implements the arch.h contract for the Heteris RV64IM + AI extension ISA.
 * UART is memory-mapped at 0x10000000 (simple TX-only model for simulation).
 * AI tiles are configured during arch_init().
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/page.h>

/* Linker-defined heap region */
extern char _heap_start[];
extern char _heap_end[];

/* --- UART (simulation model: write byte to TX register) --- */

#define UART_BASE	0x10000000ULL

static inline void mmio_write8(uint64_t addr, uint8_t val)
{
	*(volatile uint8_t *)addr = val;
}

/* --- AI Extension constants --- */

#define HETERIS_TILECFG_BASE	0x80002000ULL
#define HETERIS_NUM_TILES	16

static inline void mmio_write32(uint64_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

/*
 * Configure a tile via memory-mapped tile config register.
 * cfg format: [7:0]=enable mask, [15:8]=dtype, [23:16]=stride
 */
static void tile_configure(unsigned tile, uint32_t cfg)
{
	if (tile < HETERIS_NUM_TILES)
		mmio_write32(HETERIS_TILECFG_BASE + tile * 4, cfg);
}

/* --- Initialization --- */

void arch_early_init(void)
{
	/* UART is always-on in simulation, no init needed */
}

void arch_init(void)
{
	/* Initialize page allocator with linker-defined heap */
	anx_page_init((uintptr_t)_heap_start, (uintptr_t)_heap_end);

	/* Configure all 16 AI tiles for BF16 by default */
	for (unsigned t = 0; t < HETERIS_NUM_TILES; t++)
		tile_configure(t, 0x0101); /* enable=0x01, dtype=BF16(0x01) */
}

void arch_halt(void)
{
	for (;;)
		__asm__ volatile("wfi");
}

bool arch_irq_disable(void)
{
	uint64_t mstatus;

	__asm__ volatile("csrr %0, mstatus" : "=r"(mstatus));
	__asm__ volatile("csrc mstatus, %0" : : "r"(1ULL << 3)); /* clear MIE */
	return (mstatus & (1ULL << 3)) != 0; /* true if IRQs were enabled */
}

void arch_irq_enable(void)
{
	__asm__ volatile("csrs mstatus, %0" : : "r"(1ULL << 3)); /* set MIE */
}

void arch_irq_restore(bool state)
{
	if (state)
		arch_irq_enable();
}

anx_time_t arch_time_now(void)
{
	uint64_t time;

	__asm__ volatile("rdtime %0" : "=r"(time));
	return time;
}

/* --- Console I/O --- */

void arch_console_putc(char c)
{
	mmio_write8(UART_BASE, (uint8_t)c);
}

void arch_console_puts(const char *s)
{
	while (*s)
		arch_console_putc(*s++);
}

int arch_console_getc(void)
{
	/* No input in simulation model */
	return -1;
}

bool arch_console_has_input(void)
{
	return false;
}

/* --- Memory barriers (RISC-V fence) --- */

void arch_mb(void)
{
	__asm__ volatile("fence iorw, iorw" ::: "memory");
}

void arch_rmb(void)
{
	__asm__ volatile("fence ir, ir" ::: "memory");
}

void arch_wmb(void)
{
	__asm__ volatile("fence ow, ow" ::: "memory");
}
