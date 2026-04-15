/*
 * exception.c — x86_64 IDT, GDT, PIC, PIT, and exception handling.
 *
 * Sets up a proper kernel GDT (replacing the boot GDT), installs
 * an IDT with 48 vectors (0-31 exceptions, 32-47 IRQs), initializes
 * the 8259 PIC, and configures the PIT for ~100 Hz timer ticks.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/kprintf.h>

/* --- I/O port access --- */

static inline void outb(uint8_t val, uint16_t port)
{
	__asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void io_wait(void)
{
	outb(0, 0x80);
}

/* --- IDT --- */

struct idt_entry {
	uint16_t offset_lo;
	uint16_t selector;
	uint8_t ist;
	uint8_t type_attr;
	uint16_t offset_mid;
	uint32_t offset_hi;
	uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

#define IDT_ENTRIES	256
#define ISR_COUNT	48

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtr;

/* ISR stub table from idt.S */
extern uint64_t isr_stub_table[ISR_COUNT];

static void idt_set_gate(int n, uint64_t handler)
{
	idt[n].offset_lo = handler & 0xFFFF;
	idt[n].selector = 0x08;		/* kernel code segment */
	idt[n].ist = 0;
	idt[n].type_attr = 0x8E;	/* present, DPL=0, interrupt gate */
	idt[n].offset_mid = (handler >> 16) & 0xFFFF;
	idt[n].offset_hi = (handler >> 32) & 0xFFFFFFFF;
	idt[n].reserved = 0;
}

/* --- GDT --- */

static uint64_t gdt[] = {
	0x0000000000000000ULL,	/* null */
	0x00AF9A000000FFFFULL,	/* kernel code 64-bit */
	0x00CF92000000FFFFULL,	/* kernel data */
	0x00AFFA000000FFFFULL,	/* user code 64-bit */
	0x00CFF2000000FFFFULL,	/* user data */
	/* TSS entries would go here for ring switching */
};

struct gdt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static struct gdt_ptr gdtr;

static void gdt_init(void)
{
	gdtr.limit = sizeof(gdt) - 1;
	gdtr.base = (uint64_t)gdt;

	__asm__ volatile(
		"lgdt %0\n\t"
		"pushq $0x08\n\t"
		"leaq 1f(%%rip), %%rax\n\t"
		"pushq %%rax\n\t"
		"lretq\n\t"
		"1:\n\t"
		"movw $0x10, %%ax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%fs\n\t"
		"movw %%ax, %%gs\n\t"
		"movw %%ax, %%ss\n\t"
		: : "m"(gdtr) : "rax", "memory"
	);
}

/* --- 8259 PIC --- */

#define PIC1_CMD	0x20
#define PIC1_DATA	0x21
#define PIC2_CMD	0xA0
#define PIC2_DATA	0xA1

static void pic_init(void)
{
	/* ICW1: begin initialization in cascade mode */
	outb(0x11, PIC1_CMD);
	io_wait();
	outb(0x11, PIC2_CMD);
	io_wait();

	/* ICW2: remap master to 32-39, slave to 40-47 */
	outb(32, PIC1_DATA);
	io_wait();
	outb(40, PIC2_DATA);
	io_wait();

	/* ICW3: master has slave on IRQ2, slave has cascade on IRQ2 */
	outb(4, PIC1_DATA);
	io_wait();
	outb(2, PIC2_DATA);
	io_wait();

	/* ICW4: 8086 mode */
	outb(0x01, PIC1_DATA);
	io_wait();
	outb(0x01, PIC2_DATA);
	io_wait();

	/*
	 * Mask ALL PIC IRQs. On real hardware with LAPIC/IOAPIC,
	 * the PIC is vestigial. Unmasking IRQ0 can cause spurious
	 * interrupts that interact badly with APIC routing.
	 * Timer ticks aren't needed yet — the shell polls for input.
	 */
	outb(0xFF, PIC1_DATA);
	outb(0xFF, PIC2_DATA);
}

/* --- PIT (Programmable Interval Timer) --- */

#define PIT_CMD		0x43
#define PIT_CH0		0x40
#define PIT_FREQ	1193182		/* Hz */
#define TARGET_HZ	100

static volatile uint64_t pit_ticks;

static void pit_init(void)
{
	uint16_t divisor = PIT_FREQ / TARGET_HZ;

	/* Channel 0, access lo/hi, mode 3 (square wave) */
	outb(0x36, PIT_CMD);
	outb((uint8_t)(divisor & 0xFF), PIT_CH0);
	outb((uint8_t)((divisor >> 8) & 0xFF), PIT_CH0);

	pit_ticks = 0;
}

/* --- Exception dispatch (called from idt.S) --- */

static const char *exception_names[] = {
	"Divide by zero", "Debug", "NMI", "Breakpoint",
	"Overflow", "Bound range", "Invalid opcode", "Device N/A",
	"Double fault", "Coproc overrun", "Invalid TSS", "Segment N/P",
	"Stack-segment", "General protection", "Page fault", "Reserved",
	"x87 FP", "Alignment check", "Machine check", "SIMD FP",
	"Virtualization", "Control protection",
};

/*
 * LAPIC EOI register — fixed at 0xFEE000B0 on all x86 systems
 * with a local APIC (which includes all AMD64 CPUs).
 * Writing 0 acknowledges the current interrupt.
 */
#define LAPIC_EOI	((volatile uint32_t *)0xFEE000B0ULL)

void anx_exception_dispatch(uint64_t vector, uint64_t error_code,
			     void *frame)
{
	(void)frame;

	if (vector == 32) {
		/* Timer IRQ (PIT via PIC) */
		pit_ticks++;
		outb(0x20, PIC1_CMD);
		return;
	}

	if (vector >= 33 && vector <= 47) {
		/* PIC hardware IRQ */
		if (vector >= 40)
			outb(0x20, PIC2_CMD);
		outb(0x20, PIC1_CMD);
		return;
	}

	if (vector >= 48 || vector == 0xFF) {
		/*
		 * Unexpected vector — likely LAPIC/IOAPIC/MSI interrupt
		 * left pending by UEFI firmware. EOI both PIC and LAPIC
		 * to clear it, then return. Safe even if the LAPIC is
		 * disabled — the write is to identity-mapped MMIO.
		 */
		outb(0x20, PIC2_CMD);
		outb(0x20, PIC1_CMD);
		*LAPIC_EOI = 0;
		return;
	}

	/* CPU exception (vectors 0-31) */
	kprintf("\n*** EXCEPTION %u: %s ***\n",
		(uint32_t)vector,
		vector < 22 ? exception_names[vector] : "Unknown");
	kprintf("  Error code: 0x%x\n", (uint32_t)error_code);
	kprintf("Halting.\n");
	arch_halt();
}

/* --- Public API --- */

/* Default handler for vectors not in stub table (from idt.S) */
extern void isr_stub_default(void);

void arch_exception_init(void)
{
	uint32_t i;

	/* Install kernel GDT */
	gdt_init();

	/* Set up IDT entries from stub table (vectors 0-47) */
	for (i = 0; i < ISR_COUNT; i++)
		idt_set_gate(i, isr_stub_table[i]);

	/*
	 * Fill vectors 48-255 with a default handler.
	 * UEFI firmware / LAPIC / IOAPIC can deliver interrupts on
	 * any vector. An empty IDT entry causes #GP -> double fault
	 * -> triple fault -> reboot.
	 */
	for (i = ISR_COUNT; i < IDT_ENTRIES; i++)
		idt_set_gate(i, (uint64_t)isr_stub_default);

	/* Load IDT */
	idtr.limit = sizeof(idt) - 1;
	idtr.base = (uint64_t)idt;
	__asm__ volatile("lidt %0" : : "m"(idtr));

	/* Initialize PIC (all IRQs masked) and PIT */
	pic_init();
	pit_init();

	/* Enable interrupts — safe now that all 256 IDT entries are filled */
	__asm__ volatile("sti");
}

uint64_t arch_timer_ticks(void)
{
	return pit_ticks;
}
