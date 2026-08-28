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
#include <anx/string.h>

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

/* Selectors: kernel CS=0x08, kernel DS=0x10, user CS=0x18|3, user DS=0x20|3,
 * TSS=0x28. Slots 5-6 are filled at runtime by tss_init() with the TSS's
 * 16-byte system descriptor (base address isn't known until link/load time). */
static uint64_t gdt[] = {
	0x0000000000000000ULL,	/* null */
	0x00AF9A000000FFFFULL,	/* kernel code 64-bit */
	0x00CF92000000FFFFULL,	/* kernel data */
	0x00AFFA000000FFFFULL,	/* user code 64-bit */
	0x00CFF2000000FFFFULL,	/* user data */
	0x0000000000000000ULL,	/* TSS descriptor low half (set by tss_init) */
	0x0000000000000000ULL,	/* TSS descriptor high half */
};

#define GDT_SEL_USER_CODE	0x1BULL	/* index 3, RPL 3 */
#define GDT_SEL_USER_DATA	0x23ULL	/* index 4, RPL 3 */
#define GDT_SEL_TSS		0x28ULL	/* index 5 */

struct gdt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static struct gdt_ptr gdtr;

/* --- TSS (needed so the CPU has a kernel stack to switch to on a ring
 * 3 -> ring 0 interrupt; without one, any interrupt taken while a user
 * program is running double-faults) --- */

struct anx_tss64 {
	uint32_t reserved0;
	uint64_t rsp0;
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist[7];
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iomap_base;
} __attribute__((packed));

static struct anx_tss64 tss;
static uint8_t tss_stack[16384] __attribute__((aligned(16)));

static void tss_init(void)
{
	uint64_t base = (uint64_t)&tss;
	uint64_t limit = sizeof(tss) - 1;
	uint64_t desc_lo, desc_hi;

	anx_memset(&tss, 0, sizeof(tss));
	tss.iomap_base = sizeof(tss);
	tss.rsp0 = (uint64_t)(tss_stack + sizeof(tss_stack));

	/* 16-byte TSS descriptor (Intel SDM 7.2.3): type 0x9 = available
	 * 64-bit TSS, DPL 0. */
	desc_lo = (limit & 0xFFFFULL) |
		  ((base & 0xFFFFFFULL) << 16) |
		  (0x89ULL << 40) |
		  (((limit >> 16) & 0xFULL) << 48) |
		  (((base >> 24) & 0xFFULL) << 56);
	desc_hi = (base >> 32) & 0xFFFFFFFFULL;

	gdt[5] = desc_lo;
	gdt[6] = desc_hi;

	__asm__ volatile(
		"movw %0, %%ax\n\t"
		"ltr %%ax\n\t"
		: : "i"((uint16_t)GDT_SEL_TSS) : "ax"
	);
}

/*
 * Mark the first 1 GiB (where the kernel and the exec load window both
 * live — see ANX_USER_LOAD_MIN/MAX in posix.c) user-accessible.
 *
 * Both boot paths (efi_stub.c and qemu_boot.S) build an identical
 * PML4[0] -> PDPT -> four 1-GiB-page scheme, just at different physical
 * addresses, so this reads CR3 at runtime instead of hardcoding either
 * boot path's page-table address. This is intentionally coarse — it
 * makes the WHOLE low 1 GiB (kernel included) readable/writable from
 * ring 3, not just the exec window. Anunix has no per-process address
 * spaces yet, so page-level (not gigabyte-level) protection is future
 * work; real memory isolation between kernel and user is not enforced
 * by this milestone.
 */
static void usermode_paging_init(void)
{
	uint64_t cr3, pml4_entry;
	uint64_t *pml4, *pdpt;

	__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
	pml4 = (uint64_t *)(cr3 & ~0xFFFULL);
	pml4_entry = pml4[0];
	if (!(pml4_entry & 0x1))
		return;
	/*
	 * The CPU ANDs the U/S bit across every level of the page-table
	 * walk — PML4E, PDPTE, and (if used) PDE/PTE — so a supervisor-only
	 * bit at ANY level denies user-mode access regardless of the
	 * others. Both levels here must be marked user-accessible.
	 */
	pml4[0] |= (1ULL << 2);
	pdpt = (uint64_t *)(pml4_entry & ~0xFFFULL);
	pdpt[0] |= (1ULL << 2);
}

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
static void (*pit_callback)(void);

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
			pit_ticks++;
			if (pit_callback)
				pit_callback();
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
extern void isr_stub_syscall(void);

void arch_exception_init(void)
{
	uint32_t i;

	/* Install kernel GDT */
	gdt_init();

	/* TSS (kernel stack for ring 3 -> ring 0 transitions) and the low
	 * 1 GiB user-accessible bit — both needed before any ring-3 code
	 * can run without immediately double-faulting. */
	tss_init();
	usermode_paging_init();

	/* Set up IDT entries from stub table (vectors 0-47) */
	for (i = 0; i < ISR_COUNT; i++)
		idt_set_gate(i, isr_stub_table[i]);

	/* Fill vectors 48-255 with a default handler */
	for (i = ISR_COUNT; i < IDT_ENTRIES; i++)
		idt_set_gate(i, (uint64_t)isr_stub_default);

	/* Syscall trap (int 0x80), DPL 3 so ring-3 code may invoke it */
	idt_set_gate(0x80, (uint64_t)isr_stub_syscall);
	idt[0x80].type_attr = 0xEE;	/* present, DPL 3, interrupt gate */

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

void arch_set_timer_callback(void (*fn)(void))
{
	pit_callback = fn;
}
