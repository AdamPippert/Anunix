/*
 * arch_init.c — x86_64 architecture initialization for Framework devices.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/page.h>
#include <anx/fb.h>
#include <anx/hwprobe.h>

/* Linker-defined heap region */
extern char _heap_start[];
extern char _heap_end[];

/* --- Serial port (COM1) --- */

#define COM1_PORT	0x3F8
#define COM1_DATA	(COM1_PORT + 0)	/* data register */
#define COM1_IER	(COM1_PORT + 1)	/* interrupt enable */
#define COM1_FCR	(COM1_PORT + 2)	/* FIFO control */
#define COM1_LCR	(COM1_PORT + 3)	/* line control */
#define COM1_MCR	(COM1_PORT + 4)	/* modem control */
#define COM1_LSR	(COM1_PORT + 5)	/* line status */
#define COM1_DLL	(COM1_PORT + 0)	/* divisor latch low (DLAB=1) */
#define COM1_DLH	(COM1_PORT + 1)	/* divisor latch high (DLAB=1) */

static inline void outb(uint8_t val, uint16_t port)
{
	__asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t val;
	__asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static void serial_init(void)
{
	outb(0x00, COM1_IER);		/* disable interrupts */
	outb(0x80, COM1_LCR);		/* enable DLAB (set baud divisor) */
	outb(0x01, COM1_DLL);		/* 115200 baud (divisor 1) */
	outb(0x00, COM1_DLH);
	outb(0x03, COM1_LCR);		/* 8N1, DLAB off */
	outb(0xC7, COM1_FCR);		/* enable FIFO, clear, 14-byte threshold */
	outb(0x0B, COM1_MCR);		/* DTR + RTS + OUT2 */
}

/* --- Initialization --- */

void arch_early_init(void)
{
	serial_init();
}

void arch_init(void)
{
	/* Initialize page allocator with linker-defined heap */
	anx_page_init((uintptr_t)_heap_start, (uintptr_t)_heap_end);

	/* IDT, GDT, PIC, PIT timer */
	arch_exception_init();
}

void arch_probe_hw(struct anx_hw_inventory *inv)
{
	/* QEMU default: 1 CPU, no discrete GPU */
	inv->cpu_count = 1;
	inv->ram_bytes = 512ULL * 1024 * 1024;
	inv->accel_count = 0;
	/* TODO: parse ACPI/CPUID for real hardware */
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

/* --- Console I/O --- */

void arch_console_putc(char c)
{
	/* Wait for transmit buffer empty */
	while ((inb(COM1_LSR) & 0x20) == 0)
		;
	outb((uint8_t)c, COM1_DATA);
}

void arch_console_puts(const char *s)
{
	while (*s)
		arch_console_putc(*s++);
}

int arch_console_getc(void)
{
	/* Poll until data ready */
	while ((inb(COM1_LSR) & 0x01) == 0)
		;
	return (int)inb(COM1_DATA);
}

bool arch_console_has_input(void)
{
	return (inb(COM1_LSR) & 0x01) != 0;
}

/* --- Framebuffer detection --- */

/*
 * Multiboot framebuffer info stashed at 0x1000 by qemu_boot.S trampoline.
 * See qemu_boot.S for the layout.
 */
#define MB_FB_MAGIC_ADDR	0x1000
#define MB_FB_MAGIC_VAL		0x414E5846	/* "ANXF" */
#define MB_FB_FLAGS_ADDR	0x1004
#define MB_FB_ADDR_ADDR		0x1008
#define MB_FB_PITCH_ADDR	0x1010
#define MB_FB_WIDTH_ADDR	0x1014
#define MB_FB_HEIGHT_ADDR	0x1018
#define MB_FB_BPP_ADDR		0x101C
#define MB_FB_TYPE_ADDR		0x101D

void arch_fb_detect(struct anx_fb_info *info)
{
	volatile uint32_t *magic = (volatile uint32_t *)MB_FB_MAGIC_ADDR;
	volatile uint32_t *flags = (volatile uint32_t *)MB_FB_FLAGS_ADDR;

	info->available = false;

	/* Check magic to confirm trampoline stored info */
	if (*magic != MB_FB_MAGIC_VAL)
		return;

	/* Check multiboot flags bit 12 (framebuffer info present) */
	if (!(*flags & (1 << 12)))
		return;

	/* Check type is linear framebuffer (type 1 = direct RGB) */
	if (*(volatile uint8_t *)MB_FB_TYPE_ADDR != 1)
		return;

	info->addr   = *(volatile uint64_t *)MB_FB_ADDR_ADDR;
	info->pitch  = *(volatile uint32_t *)MB_FB_PITCH_ADDR;
	info->width  = *(volatile uint32_t *)MB_FB_WIDTH_ADDR;
	info->height = *(volatile uint32_t *)MB_FB_HEIGHT_ADDR;
	info->bpp    = *(volatile uint8_t *)MB_FB_BPP_ADDR;
	info->available = (info->addr != 0 && info->bpp == 32);
}

/* --- Memory barriers --- */

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
