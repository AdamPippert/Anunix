/*
 * arch_init.c — x86_64 architecture initialization for Framework devices.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/io.h>
#include <anx/irq.h>
#include <anx/page.h>
#include <anx/fb.h>
#include <anx/hwprobe.h>
#include <anx/input.h>
#include <anx/usb_mouse.h>

/* Linker-defined heap region */
extern char _heap_start[];
extern char _heap_end[];

/* Forward declarations for init helpers defined later in this file */
static void kbd_init(void);
static void mouse_init(void);

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

static void kbd_init(void);

void arch_init(void)
{
	/* Initialize page allocator with linker-defined heap */
	anx_page_init((uintptr_t)_heap_start, (uintptr_t)_heap_end);

	/* IDT, GDT, PIC, PIT timer */
	arch_exception_init();

	/* PS/2 keyboard (IRQ1) and mouse (IRQ12) */
	kbd_init();
	mouse_init();
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

/* --- PS/2 Keyboard --- */

#define KBD_DATA_PORT	0x60
#define KBD_STATUS_PORT	0x64
#define KBD_BUF_SIZE	64

static char kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head, kbd_tail;
static bool kbd_shift;

/* US QWERTY scancode set 1 → ASCII (lowercase) */
static const char scancode_map[128] = {
	0, 0x1B, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
	'\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
	0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
	0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
	'*', 0, ' ',
};

static const char scancode_shift[128] = {
	0, 0x1B, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
	'\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
	0, 'A','S','D','F','G','H','J','K','L',':','"','~',
	0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
	'*', 0, ' ',
};

static void kbd_irq_handler(uint32_t irq, void *arg)
{
	uint8_t scancode;
	char c;
	uint32_t next;

	(void)irq;
	(void)arg;

	scancode = anx_inb(KBD_DATA_PORT);

	/* Handle shift key */
	if (scancode == 0x2A || scancode == 0x36) {
		kbd_shift = true;
		anx_input_ps2_key(scancode, 0);
		return;
	}
	if (scancode == 0xAA || scancode == 0xB6) {
		kbd_shift = false;
		anx_input_ps2_key(scancode, 0);
		return;
	}

	/* Forward all scancodes (including releases) to input layer */
	{
		uint8_t raw = scancode & 0x7F;
		uint32_t unicode = 0;

		if (!(scancode & 0x80) && raw < 128) {
			char ch = kbd_shift ? scancode_shift[raw] : scancode_map[raw];
			unicode = (uint32_t)(unsigned char)ch;
		}
		anx_input_ps2_key(scancode, unicode);
	}

	/* Key releases: only needed for input layer above */
	if (scancode & 0x80)
		return;

	/* Convert to ASCII for arch_console_getc() ring buffer */
	if (scancode >= 128)
		return;
	c = kbd_shift ? scancode_shift[scancode] : scancode_map[scancode];
	if (c == 0)
		return;

	/* Buffer the character */
	next = (kbd_head + 1) % KBD_BUF_SIZE;
	if (next != kbd_tail) {
		kbd_buf[kbd_head] = c;
		kbd_head = next;
	}
}

static void kbd_init(void)
{
	kbd_head = 0;
	kbd_tail = 0;
	kbd_shift = false;

	/* Flush any pending scancodes */
	while (anx_inb(KBD_STATUS_PORT) & 0x01)
		anx_inb(KBD_DATA_PORT);

	/* Register IRQ1 handler and unmask */
	anx_irq_register(1, kbd_irq_handler, NULL);
	anx_irq_unmask(1);
}

/* --- PS/2 Mouse (auxiliary PS/2 port, IRQ 12) --- */

/*
 * The PS/2 controller shares data (0x60) and status/command (0x64) ports with
 * the keyboard.  Mouse bytes are tagged by the controller — bit 5 of status is
 * set when the byte in the output buffer came from the aux port.
 *
 * Standard PS/2 mouse sends 3-byte packets:
 *   Byte 0: flags (buttons, overflow, sign bits)
 *   Byte 1: X movement (signed, 9-bit with sign in byte 0 bit 4)
 *   Byte 2: Y movement (signed, 9-bit with sign in byte 0 bit 5)
 *
 * Initialization sequence:
 *   1. Enable auxiliary device (command 0xA8 to controller).
 *   2. Get compaq status byte, set bit 1 (enable aux IRQ), send back.
 *   3. Send "enable streaming" command (0xF4) to mouse.
 *   4. Register IRQ 12 handler and unmask.
 */

static uint8_t  mouse_packet[3];
static uint32_t mouse_packet_idx;

static void ps2_aux_write(uint8_t val)
{
	/* Wait for controller input buffer empty */
	while (anx_inb(KBD_STATUS_PORT) & 0x02)
		;
	anx_outb(0xD4, KBD_STATUS_PORT);	/* "write to aux device" */
	while (anx_inb(KBD_STATUS_PORT) & 0x02)
		;
	anx_outb(val, KBD_DATA_PORT);
}

static uint8_t ps2_ctrl_read(void)
{
	while (!(anx_inb(KBD_STATUS_PORT) & 0x01))
		;
	return anx_inb(KBD_DATA_PORT);
}

static void mouse_irq_handler(uint32_t irq, void *arg)
{
	uint8_t data;
	int8_t  dx, dy;
	uint8_t flags;

	(void)irq;
	(void)arg;

	/* Discard if byte came from keyboard (aux bit clear) */
	if (!(anx_inb(KBD_STATUS_PORT) & 0x20)) {
		anx_inb(KBD_DATA_PORT);
		return;
	}

	data = anx_inb(KBD_DATA_PORT);
	mouse_packet[mouse_packet_idx++] = data;

	if (mouse_packet_idx < 3)
		return;

	mouse_packet_idx = 0;

	flags  = mouse_packet[0];
	dx     = (int8_t)mouse_packet[1];
	dy     = (int8_t)mouse_packet[2];

	/* Discard packet if overflow bits are set */
	if (flags & 0xC0)
		return;

	{
		struct anx_hid_mouse_report rpt;
		struct anx_fb_info fbi;
		uint32_t sw = 1920, sh = 1080;

		/* Y is inverted in PS/2 (positive = up in PS/2 = down on screen) */
		rpt.buttons = flags & 0x07;
		rpt.x       = dx;
		rpt.y       = -dy;
		rpt.wheel   = 0;

		arch_fb_detect(&fbi);
		if (fbi.available) { sw = fbi.width; sh = fbi.height; }

		anx_usb_mouse_report(&rpt, sw, sh);
	}
}

static void mouse_init(void)
{
	uint8_t status;

	/* Enable aux port */
	while (anx_inb(KBD_STATUS_PORT) & 0x02)
		;
	anx_outb(0xA8, KBD_STATUS_PORT);

	/* Enable aux IRQ: get compaq status, set bit 1, write back */
	while (anx_inb(KBD_STATUS_PORT) & 0x02)
		;
	anx_outb(0x20, KBD_STATUS_PORT);		/* read config byte */
	status = ps2_ctrl_read();
	status |= 0x02;					/* enable aux IRQ */
	status &= ~0x20;				/* clear aux clock disable */
	while (anx_inb(KBD_STATUS_PORT) & 0x02)
		;
	anx_outb(0x60, KBD_STATUS_PORT);		/* write config byte */
	while (anx_inb(KBD_STATUS_PORT) & 0x02)
		;
	anx_outb(status, KBD_DATA_PORT);

	/* Enable streaming */
	ps2_aux_write(0xF4);
	ps2_ctrl_read();				/* ACK */

	mouse_packet_idx = 0;

	anx_irq_register(12, mouse_irq_handler, NULL);
	anx_irq_unmask(12);
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
	/* Check keyboard buffer first (IRQ-driven) */
	if (kbd_head != kbd_tail) {
		char c = kbd_buf[kbd_tail];

		kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
		return (int)c;
	}

	/* Fall back to serial */
	if (!com1_present)
		return -1;
	while ((anx_inb(COM1_LSR) & 0x01) == 0) {
		/* Check keyboard while waiting for serial */
		if (kbd_head != kbd_tail) {
			char c = kbd_buf[kbd_tail];

			kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
			return (int)c;
		}
	}
	return (int)anx_inb(COM1_DATA);
}

bool arch_console_has_input(void)
{
	/* Keyboard buffer has data */
	if (kbd_head != kbd_tail)
		return true;
	/* Serial has data */
	if (com1_present && (anx_inb(COM1_LSR) & 0x01) != 0)
		return true;
	return false;
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

/* --- Boot command line --- */

/*
 * Multiboot1 cmdline saved by qemu_boot.S trampoline at 0x1020.
 * Returns pointer to the command line string, or NULL if unavailable.
 */
#define MB1_CMDLINE_ADDR	0x1020

const char *arch_boot_cmdline(void)
{
	volatile uint64_t *cmdline_ptr = (volatile uint64_t *)MB1_CMDLINE_ADDR;

	if (*cmdline_ptr == 0)
		return NULL;
	return (const char *)(uintptr_t)*cmdline_ptr;
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
