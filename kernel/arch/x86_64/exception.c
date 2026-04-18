/*
 * exception.c — x86_64 IDT, GDT, PIC, PIT, and exception handling.
 *
 * Sets up a proper kernel GDT (replacing the boot GDT), installs
 * an IDT with 256 vectors, initializes the 8259 PIC, configures
 * the PIT for ~100 Hz timer ticks, and provides dynamic IRQ
 * registration for device drivers.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/io.h>
#include <anx/irq.h>
#include <anx/kprintf.h>

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

static uint8_t pic1_mask = 0xFF;
static uint8_t pic2_mask = 0xFF;

static void pic_init(void)
{
	/* ICW1: begin initialization in cascade mode */
	anx_outb(0x11, PIC1_CMD);
	anx_io_wait();
	anx_outb(0x11, PIC2_CMD);
	anx_io_wait();

	/* ICW2: remap master to 32-39, slave to 40-47 */
	anx_outb(32, PIC1_DATA);
	anx_io_wait();
	anx_outb(40, PIC2_DATA);
	anx_io_wait();

	/* ICW3: master has slave on IRQ2, slave has cascade on IRQ2 */
	anx_outb(4, PIC1_DATA);
	anx_io_wait();
	anx_outb(2, PIC2_DATA);
	anx_io_wait();

	/* ICW4: 8086 mode */
	anx_outb(0x01, PIC1_DATA);
	anx_io_wait();
	anx_outb(0x01, PIC2_DATA);
	anx_io_wait();

	/* Mask all IRQs initially — drivers unmask as needed */
	anx_outb(pic1_mask, PIC1_DATA);
	anx_outb(pic2_mask, PIC2_DATA);
}

/* --- Dynamic IRQ handler table (shared IRQ support) --- */

#define PIC_IRQ_COUNT		16
#define PIC_IRQ_BASE		32	/* vector offset for IRQ 0 */
#define IRQ_HANDLERS_PER_LINE	4	/* max shared handlers per IRQ */

static struct {
	anx_irq_handler_t handler;
	void *arg;
} irq_handlers[PIC_IRQ_COUNT][IRQ_HANDLERS_PER_LINE];

int anx_irq_register(uint8_t irq, anx_irq_handler_t handler, void *arg)
{
	uint32_t slot;

	if (irq >= PIC_IRQ_COUNT || handler == NULL)
		return ANX_EINVAL;

	for (slot = 0; slot < IRQ_HANDLERS_PER_LINE; slot++) {
		if (!irq_handlers[irq][slot].handler) {
			irq_handlers[irq][slot].handler = handler;
			irq_handlers[irq][slot].arg     = arg;
			return ANX_OK;
		}
	}
	return ANX_ENOMEM;	/* all slots full */
}

void anx_irq_unmask(uint8_t irq)
{
	if (irq < 8) {
		pic1_mask &= ~(1 << irq);
		anx_outb(pic1_mask, PIC1_DATA);
	} else if (irq < 16) {
		pic2_mask &= ~(1 << (irq - 8));
		anx_outb(pic2_mask, PIC2_DATA);
		/* Unmask cascade (IRQ2) on master */
		pic1_mask &= ~(1 << 2);
		anx_outb(pic1_mask, PIC1_DATA);
	}
}

void anx_irq_mask(uint8_t irq)
{
	if (irq < 8) {
		pic1_mask |= (1 << irq);
		anx_outb(pic1_mask, PIC1_DATA);
	} else if (irq < 16) {
		pic2_mask |= (1 << (irq - 8));
		anx_outb(pic2_mask, PIC2_DATA);
	}
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
	anx_outb(0x36, PIT_CMD);
	anx_outb((uint8_t)(divisor & 0xFF), PIT_CH0);
	anx_outb((uint8_t)((divisor >> 8) & 0xFF), PIT_CH0);

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
 */
#define LAPIC_EOI	((volatile uint32_t *)0xFEE000B0ULL)

void anx_exception_dispatch(uint64_t vector, uint64_t error_code,
			     void *frame)
{
	(void)frame;

	/* PIC IRQs (vectors 32-47) */
	if (vector >= PIC_IRQ_BASE &&
	    vector < PIC_IRQ_BASE + PIC_IRQ_COUNT) {
		uint8_t irq = (uint8_t)(vector - PIC_IRQ_BASE);

		if (irq == 0) {
			/* Timer IRQ — always handled internally */
			pit_ticks++;
		} else {
			uint32_t slot;

			for (slot = 0; slot < IRQ_HANDLERS_PER_LINE; slot++) {
				if (irq_handlers[irq][slot].handler)
					irq_handlers[irq][slot].handler(
						irq, irq_handlers[irq][slot].arg);
			}
		}

		/* EOI */
		if (irq >= 8)
			anx_outb(0x20, PIC2_CMD);
		anx_outb(0x20, PIC1_CMD);
		return;
	}

	if (vector >= 48 || vector == 0xFF) {
		/*
		 * Unexpected vector — LAPIC/IOAPIC/MSI interrupt
		 * left pending by firmware. Safe EOI and return.
		 */
		anx_outb(0x20, PIC2_CMD);
		anx_outb(0x20, PIC1_CMD);
		*LAPIC_EOI = 0;
		return;
	}

	/* CPU exception (vectors 0-31) */
	kprintf("\n*** EXCEPTION %u: %s ***\n",
		(uint32_t)vector,
		vector < 22 ? exception_names[vector] : "Unknown");
	kprintf("  Error code: 0x%x\n", (uint32_t)error_code);
	/* Dump RIP for debugging.  frame points to saved GP regs (15 qwords),
	 * then vector (8), error code (8), then CPU-pushed RIP/CS/RFLAGS/... */
	{
		uint64_t *stack = (uint64_t *)frame;
		uint64_t rip = stack[17];

		kprintf("  RIP:  0x%x\n", (uint32_t)rip);
	}
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

	/* Fill vectors 48-255 with a default handler */
	for (i = ISR_COUNT; i < IDT_ENTRIES; i++)
		idt_set_gate(i, (uint64_t)isr_stub_default);

	/* Load IDT */
	idtr.limit = sizeof(idt) - 1;
	idtr.base = (uint64_t)idt;
	__asm__ volatile("lidt %0" : : "m"(idtr));

	/* Initialize PIC (all IRQs masked) and PIT */
	pic_init();
	pit_init();

	/* Unmask IRQ0 (PIT timer) so arch_timer_ticks() works */
	anx_irq_unmask(0);

	/* Enable interrupts */
	__asm__ volatile("sti");
}

uint64_t arch_timer_ticks(void)
{
	return pit_ticks;
}
