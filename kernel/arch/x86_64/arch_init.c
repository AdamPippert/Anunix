/*
 * arch_init.c — x86_64 architecture initialization for Framework devices.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/io.h>
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

static bool com1_present;

/*
 * Detect COM1 by writing to the scratch register (offset +7)
 * and reading it back. If no UART exists, reads return 0xFF.
 */
#define COM1_SCRATCH	(COM1_PORT + 7)

static void serial_init(void)
{
	/* Probe for COM1 existence */
	anx_outb(0xA5, COM1_SCRATCH);
	if (anx_inb(COM1_SCRATCH) != 0xA5) {
		com1_present = false;
		return;
	}
	anx_outb(0x5A, COM1_SCRATCH);
	if (anx_inb(COM1_SCRATCH) != 0x5A) {
		com1_present = false;
		return;
	}

	com1_present = true;

	anx_outb(0x00, COM1_IER);		/* disable interrupts */
	anx_outb(0x80, COM1_LCR);		/* enable DLAB (set baud divisor) */
	anx_outb(0x01, COM1_DLL);		/* 115200 baud (divisor 1) */
	anx_outb(0x00, COM1_DLH);
	anx_outb(0x03, COM1_LCR);		/* 8N1, DLAB off */
	anx_outb(0xC7, COM1_FCR);		/* enable FIFO, clear, 14-byte threshold */
	anx_outb(0x0B, COM1_MCR);		/* DTR + RTS + OUT2 */
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
	if (!com1_present)
		return;
	/* Wait for transmit buffer empty */
	while ((anx_inb(COM1_LSR) & 0x20) == 0)
		;
	anx_outb((uint8_t)c, COM1_DATA);
}

void arch_console_puts(const char *s)
{
	while (*s)
		arch_console_putc(*s++);
}

int arch_console_getc(void)
{
	if (!com1_present)
		return -1;
	/* Poll until data ready */
	while ((anx_inb(COM1_LSR) & 0x01) == 0)
		;
	return (int)anx_inb(COM1_DATA);
}

bool arch_console_has_input(void)
{
	if (!com1_present)
		return false;
	return (anx_inb(COM1_LSR) & 0x01) != 0;
}

/* --- Framebuffer detection --- */

/*
 * Boot info saved by boot.S _start entry point.
 * On multiboot2 (UEFI): _mb_magic = 0x36d76289, _mb_info = info pointer.
 * On QEMU trampoline:   _mb_magic = garbage, _mb_info = garbage.
 */
extern uint32_t _mb_magic;
extern uint64_t _mb_info;

#define MB2_MAGIC		0x36d76289

/* Multiboot2 tag types */
#define MB2_TAG_END		0
#define MB2_TAG_FRAMEBUFFER	8

struct mb2_tag {
	uint32_t type;
	uint32_t size;
};

struct mb2_tag_framebuffer {
	uint32_t type;
	uint32_t size;
	uint64_t addr;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint8_t  bpp;
	uint8_t  fb_type;	/* 1 = direct RGB */
	uint8_t  reserved;
};

static bool fb_detect_mb2(struct anx_fb_info *info)
{
	uint8_t *ptr;
	uint8_t *end;
	uint32_t total_size;

	if (_mb_magic != MB2_MAGIC || _mb_info == 0)
		return false;

	ptr = (uint8_t *)(uintptr_t)_mb_info;
	total_size = *(uint32_t *)ptr;
	end = ptr + total_size;
	ptr += 8;	/* skip total_size + reserved */

	while (ptr + sizeof(struct mb2_tag) <= end) {
		struct mb2_tag *tag = (struct mb2_tag *)ptr;

		if (tag->type == MB2_TAG_END)
			break;

		if (tag->type == MB2_TAG_FRAMEBUFFER &&
		    tag->size >= sizeof(struct mb2_tag_framebuffer)) {
			struct mb2_tag_framebuffer *fb =
				(struct mb2_tag_framebuffer *)ptr;

			if (fb->fb_type != 1)	/* not direct RGB */
				break;

			info->addr   = fb->addr;
			info->pitch  = fb->pitch;
			info->width  = fb->width;
			info->height = fb->height;
			info->bpp    = fb->bpp;
			info->available = (info->addr != 0 &&
					   info->bpp == 32);
			return true;
		}

		/* Tags are 8-byte aligned */
		ptr += (tag->size + 7) & ~7u;
	}

	return false;
}

/*
 * Multiboot1 framebuffer info stashed at 0x1000 by qemu_boot.S trampoline.
 * See qemu_boot.S for the layout.
 */
#define MB1_FB_MAGIC_ADDR	0x1000
#define MB1_FB_MAGIC_VAL	0x414E5846	/* "ANXF" */
#define MB1_FB_FLAGS_ADDR	0x1004
#define MB1_FB_ADDR_ADDR	0x1008
#define MB1_FB_PITCH_ADDR	0x1010
#define MB1_FB_WIDTH_ADDR	0x1014
#define MB1_FB_HEIGHT_ADDR	0x1018
#define MB1_FB_BPP_ADDR		0x101C
#define MB1_FB_TYPE_ADDR	0x101D

static bool fb_detect_mb1(struct anx_fb_info *info)
{
	volatile uint32_t *magic = (volatile uint32_t *)MB1_FB_MAGIC_ADDR;
	volatile uint32_t *flags = (volatile uint32_t *)MB1_FB_FLAGS_ADDR;

	if (*magic != MB1_FB_MAGIC_VAL)
		return false;
	if (!(*flags & (1 << 12)))
		return false;
	if (*(volatile uint8_t *)MB1_FB_TYPE_ADDR != 1)
		return false;

	info->addr   = *(volatile uint64_t *)MB1_FB_ADDR_ADDR;
	info->pitch  = *(volatile uint32_t *)MB1_FB_PITCH_ADDR;
	info->width  = *(volatile uint32_t *)MB1_FB_WIDTH_ADDR;
	info->height = *(volatile uint32_t *)MB1_FB_HEIGHT_ADDR;
	info->bpp    = *(volatile uint8_t *)MB1_FB_BPP_ADDR;
	info->available = (info->addr != 0 && info->bpp == 32);
	return info->available;
}

void arch_fb_detect(struct anx_fb_info *info)
{
	info->available = false;

	/* Try multiboot2 first (UEFI / real hardware) */
	if (fb_detect_mb2(info))
		return;

	/* Fall back to multiboot1 trampoline info (QEMU) */
	fb_detect_mb1(info);
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
