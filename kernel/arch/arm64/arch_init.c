/*
 * arch_init.c — ARM64 architecture initialization.
 *
 * For QEMU virt machine: PL011 UART at 0x09000000.
 * For real Apple Silicon: different UART base (deferred).
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/page.h>
#include <anx/fb.h>
#include <anx/hwprobe.h>
#include <anx/string.h>

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

	/* Exception vectors, GIC, and timer */
	arch_exception_init();
}

void arch_probe_hw(struct anx_hw_inventory *inv)
{
	/* QEMU virt: 1 CPU, no GPU/NPU */
	inv->cpu_count = 1;
	inv->ram_bytes = 512ULL * 1024 * 1024;	/* matches -m 512M */
	inv->accel_count = 0;
	/* TODO: parse device tree for real hardware */
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

/* --- Framebuffer (QEMU ramfb via fw_cfg) --- */
#if 0 /* Disabled until exception vectors are installed — fw_cfg MMIO faults without them */

/*
 * QEMU fw_cfg MMIO on virt machine.
 * Used to configure ramfb: a simple linear framebuffer device.
 */
#define FW_CFG_BASE		0x09020000ULL
#define FW_CFG_DATA		(FW_CFG_BASE + 0x000)
#define FW_CFG_SEL		(FW_CFG_BASE + 0x008)
#define FW_CFG_DMA_ADDR		(FW_CFG_BASE + 0x010)

#define FW_CFG_FILE_DIR		0x0019

/* DMA control bits */
#define FW_CFG_DMA_CTL_ERROR	(1 << 0)
#define FW_CFG_DMA_CTL_READ	(1 << 1)
#define FW_CFG_DMA_CTL_SKIP	(1 << 2)
#define FW_CFG_DMA_CTL_SELECT	(1 << 3)
#define FW_CFG_DMA_CTL_WRITE	(1 << 4)

/* DRM fourcc for XRGB8888 */
#define DRM_FORMAT_XRGB8888	0x34325258

/* Ramfb display resolution */
#define RAMFB_WIDTH		1024
#define RAMFB_HEIGHT		768

/* Byte-swap helpers (fw_cfg uses big-endian) */
static inline uint16_t bswap16(uint16_t v)
{
	return (v >> 8) | (v << 8);
}

static inline uint32_t bswap32(uint32_t v)
{
	return ((v & 0xFF000000) >> 24) |
	       ((v & 0x00FF0000) >> 8) |
	       ((v & 0x0000FF00) << 8) |
	       ((v & 0x000000FF) << 24);
}

static inline uint64_t bswap64(uint64_t v)
{
	return ((uint64_t)bswap32((uint32_t)v) << 32) |
	       bswap32((uint32_t)(v >> 32));
}

/* fw_cfg file directory entry */
struct fw_cfg_file {
	uint32_t size;		/* big-endian */
	uint16_t select;	/* big-endian */
	uint16_t reserved;
	char name[56];
};

/* fw_cfg DMA access structure — must be naturally aligned */
struct fw_cfg_dma_access {
	uint32_t control;	/* big-endian */
	uint32_t length;	/* big-endian */
	uint64_t address;	/* big-endian */
} __attribute__((aligned(16)));

/* ramfb configuration structure */
struct ramfb_cfg {
	uint64_t addr;		/* big-endian: framebuffer physical address */
	uint32_t fourcc;	/* big-endian: pixel format */
	uint32_t flags;		/* big-endian: 0 */
	uint32_t width;		/* big-endian */
	uint32_t height;	/* big-endian */
	uint32_t stride;	/* big-endian: bytes per scanline */
} __attribute__((packed));

static void fw_cfg_select(uint16_t selector)
{
	mmio_write32(FW_CFG_SEL, (uint32_t)bswap16(selector));
}

static uint8_t fw_cfg_read8(void)
{
	return (uint8_t)mmio_read32(FW_CFG_DATA);
}

static void fw_cfg_read_bytes(void *buf, uint32_t len)
{
	uint8_t *p = buf;
	uint32_t i;

	for (i = 0; i < len; i++)
		p[i] = fw_cfg_read8();
}

static void fw_cfg_dma_write(uint16_t selector,
			     void *data, uint32_t len)
{
	volatile struct fw_cfg_dma_access dma;

	dma.control = bswap32(FW_CFG_DMA_CTL_SELECT |
			      FW_CFG_DMA_CTL_WRITE |
			      ((uint32_t)selector << 16));
	dma.length = bswap32(len);
	dma.address = bswap64((uint64_t)(uintptr_t)data);

	/* Write DMA address (big-endian 64-bit) to trigger transfer */
	uint64_t dma_addr = (uint64_t)(uintptr_t)&dma;

	/* Ensure dma struct is visible in memory */
	__asm__ volatile("dsb sy" ::: "memory");

	mmio_write32(FW_CFG_DMA_ADDR, (uint32_t)(bswap64(dma_addr) >> 32));
	mmio_write32(FW_CFG_DMA_ADDR + 4, (uint32_t)bswap64(dma_addr));

	/* Wait for DMA completion (control goes to 0) */
	while (bswap32(dma.control) & ~FW_CFG_DMA_CTL_ERROR)
		__asm__ volatile("dsb sy" ::: "memory");
}

static int fw_cfg_find_file(const char *name, uint16_t *selector)
{
	uint32_t count;
	uint32_t i;

	fw_cfg_select(FW_CFG_FILE_DIR);

	/* Read file count (big-endian u32) */
	fw_cfg_read_bytes(&count, 4);
	count = bswap32(count);

	for (i = 0; i < count; i++) {
		struct fw_cfg_file entry;

		fw_cfg_read_bytes(&entry, sizeof(entry));

		if (anx_strcmp(entry.name, name) == 0) {
			*selector = bswap16(entry.select);
			return 0;
		}
	}

	return ANX_ENOENT;
}

/*
 * Static framebuffer memory.
 * 1024 * 768 * 4 = 3 MiB — placed in BSS, page-aligned.
 */
static uint8_t ramfb_mem[RAMFB_WIDTH * RAMFB_HEIGHT * 4]
	__attribute__((aligned(4096)));

#endif /* disabled fw_cfg block */

void arch_fb_detect(struct anx_fb_info *info)
{
	/*
	 * Framebuffer detection disabled: fw_cfg MMIO at 0x09020000
	 * faults without exception vectors installed.
	 *
	 * TODO: Re-enable ramfb detection after GIC + exception
	 * vector setup. Use 'make qemu-fb' for graphical testing.
	 */
	info->available = false;
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

const char *arch_boot_cmdline(void)
{
	/* ARM64 QEMU virt has no multiboot cmdline; use device tree in future */
	return NULL;
}
