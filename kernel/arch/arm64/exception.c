/*
 * exception.c — ARM64 exception handling, GICv2, and generic timer.
 *
 * For QEMU virt machine:
 *   GICv2 distributor at 0x08000000
 *   GICv2 CPU interface at 0x08010000
 *   Virtual timer IRQ: PPI 27 (INTID 27)
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/irq.h>
#include <anx/kprintf.h>

/* --- GICv2 register map (QEMU virt) --- */

#define GICD_BASE	0x08000000ULL
#define GICC_BASE	0x08010000ULL

/* Distributor registers */
#define GICD_CTLR	(GICD_BASE + 0x000)
#define GICD_ISENABLER0	(GICD_BASE + 0x100)
#define GICD_IPRIORITYR	(GICD_BASE + 0x400)
#define GICD_ITARGETSR	(GICD_BASE + 0x800)

/* CPU interface registers */
#define GICC_CTLR	(GICC_BASE + 0x000)
#define GICC_PMR	(GICC_BASE + 0x004)
#define GICC_IAR	(GICC_BASE + 0x00C)
#define GICC_EOIR	(GICC_BASE + 0x010)

/* Virtual timer PPI — INTID 27 */
#define TIMER_IRQ	27

static inline void mmio_write32(uint64_t addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read32(uint64_t addr)
{
	return *(volatile uint32_t *)addr;
}

/* --- Dynamic IRQ dispatch table --- */

#define ANX_IRQ_MAX	16

static anx_irq_handler_t irq_handlers[ANX_IRQ_MAX];
static void              *irq_args[ANX_IRQ_MAX];

/* --- Timer state --- */

static volatile uint64_t timer_ticks;
static uint64_t timer_interval;		/* ticks per interrupt */

/* --- GICv2 initialization --- */

static void gicv2_init(void)
{
	/* Enable distributor */
	mmio_write32(GICD_CTLR, 1);

	/* Enable timer IRQ (INTID 27 is in ISENABLER0, bit 27) */
	mmio_write32(GICD_ISENABLER0, 1U << TIMER_IRQ);

	/* Set priority for timer IRQ (lower = higher priority) */
	{
		uint32_t reg = mmio_read32(GICD_IPRIORITYR + (TIMER_IRQ / 4) * 4);
		uint32_t shift = (TIMER_IRQ % 4) * 8;

		reg &= ~(0xFFU << shift);
		reg |= (0x80U << shift);	/* priority 128 */
		mmio_write32(GICD_IPRIORITYR + (TIMER_IRQ / 4) * 4, reg);
	}

	/* Target timer IRQ to CPU 0 */
	{
		uint32_t reg = mmio_read32(GICD_ITARGETSR + (TIMER_IRQ / 4) * 4);
		uint32_t shift = (TIMER_IRQ % 4) * 8;

		reg &= ~(0xFFU << shift);
		reg |= (0x01U << shift);	/* CPU 0 */
		mmio_write32(GICD_ITARGETSR + (TIMER_IRQ / 4) * 4, reg);
	}

	/* Enable CPU interface */
	mmio_write32(GICC_CTLR, 1);

	/* Accept all priority levels */
	mmio_write32(GICC_PMR, 0xFF);
}

/* --- Generic timer --- */

static void timer_init(void)
{
	uint64_t freq;

	/* Read counter frequency */
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));

	/* 10ms interval */
	timer_interval = freq / 100;
	if (timer_interval == 0)
		timer_interval = 1000000;	/* fallback: ~1ms at 1GHz */

	/* Set timer value and enable */
	__asm__ volatile("msr cntv_tval_el0, %0" : : "r"(timer_interval));
	__asm__ volatile("msr cntv_ctl_el0, %0" : : "r"((uint64_t)1));

	timer_ticks = 0;
}

static void timer_isr(void)
{
	timer_ticks++;

	/* Reload timer for next interrupt */
	__asm__ volatile("msr cntv_tval_el0, %0" : : "r"(timer_interval));
}

/* --- Exception handler (called from vectors.S) --- */

static const char *exc_type_names[] = {
	"Synchronous", "IRQ", "FIQ", "SError",
};

static const char *exc_group_names[] = {
	"EL0/SP_EL0", "ELx/SP_ELx", "Lower64", "Lower32",
};

void anx_exception_handler(uint64_t type, uint64_t esr,
			    uint64_t elr, uint64_t far)
{
	uint64_t group = (type >> 2) & 3;
	uint64_t exc = type & 3;

	if (exc == 1) {
		/* IRQ — read interrupt ID from GIC */
		uint32_t iar = mmio_read32(GICC_IAR);
		uint32_t irq = iar & 0x3FF;

		if (irq == TIMER_IRQ) {
			timer_isr();
		} else if (irq >= 32 && irq < 32 + ANX_IRQ_MAX &&
		           irq_handlers[irq - 32] != NULL) {
			irq_handlers[irq - 32]((uint32_t)(irq - 32),
			                       irq_args[irq - 32]);
		} else if (irq < 1020) {
			kprintf("IRQ %u (unhandled)\n", irq);
		}
		/* else: spurious (1023), ignore */

		/* End of interrupt */
		if (irq < 1020)
			mmio_write32(GICC_EOIR, iar);

		return;
	}

	/* Synchronous exception or SError — print and halt */
	kprintf("\n*** EXCEPTION: %s from %s ***\n",
		exc_type_names[exc], exc_group_names[group]);
	kprintf("  ESR_EL1:  0x%x\n", (uint32_t)esr);
	kprintf("  ELR_EL1:  0x%x%x\n",
		(uint32_t)(elr >> 32), (uint32_t)elr);
	kprintf("  FAR_EL1:  0x%x%x\n",
		(uint32_t)(far >> 32), (uint32_t)far);

	uint32_t ec = (uint32_t)(esr >> 26) & 0x3F;

	kprintf("  EC:       0x%x", ec);
	if (ec == 0x25)
		kprintf(" (data abort, current EL)");
	else if (ec == 0x21)
		kprintf(" (instruction abort, current EL)");
	else if (ec == 0x15)
		kprintf(" (SVC)");
	else if (ec == 0x20)
		kprintf(" (instruction abort, lower EL)");
	else if (ec == 0x24)
		kprintf(" (data abort, lower EL)");
	kprintf("\n");

	kprintf("Halting.\n");
	arch_halt();
}

/* --- Public API --- */

/* Defined in vectors.S */
extern char anx_vectors[];

void arch_exception_init(void)
{
	/* Install vector table */
	__asm__ volatile("msr vbar_el1, %0" : : "r"(anx_vectors));
	__asm__ volatile("isb");

	/* Initialize GIC and timer */
	gicv2_init();
	timer_init();

	/* Unmask IRQs (keep FIQ, SError masked) */
	__asm__ volatile("msr daifclr, #2");
}

uint64_t arch_timer_ticks(void)
{
	return timer_ticks;
}

/* --- Dynamic IRQ dispatch (GICv2 SPIs, INTIDs 32-1019) --- */

int anx_irq_register(uint8_t irq, anx_irq_handler_t handler, void *arg)
{
	if (irq >= ANX_IRQ_MAX || handler == NULL)
		return -1;
	irq_handlers[irq] = handler;
	irq_args[irq]     = arg;
	return 0;
}

void anx_irq_unmask(uint8_t irq)
{
	/* SPI base = INTID 32; enable in GICD_ISENABLER word 1 */
	uint32_t intid = 32 + irq;

	if (irq >= ANX_IRQ_MAX)
		return;
	mmio_write32(GICD_BASE + 0x100 + (intid / 32) * 4,
	             1U << (intid % 32));
}

void anx_irq_mask(uint8_t irq)
{
	uint32_t intid = 32 + irq;

	if (irq >= ANX_IRQ_MAX)
		return;
	/* GICD_ICENABLER: clear-enable */
	mmio_write32(GICD_BASE + 0x180 + (intid / 32) * 4,
	             1U << (intid % 32));
}
