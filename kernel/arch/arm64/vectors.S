/*
 * vectors.S — ARM64 exception vector table.
 *
 * ARMv8-A requires a 2048-byte-aligned table of 16 entries,
 * each 128 bytes (32 instructions). The table is installed
 * via VBAR_EL1 during arch_exception_init().
 *
 * Layout (4 groups × 4 exception types):
 *   Group 0: Current EL with SP_EL0
 *   Group 1: Current EL with SP_ELx (kernel uses this)
 *   Group 2: Lower EL, AArch64
 *   Group 3: Lower EL, AArch32
 *
 * Each group has: Synchronous, IRQ, FIQ, SError
 */

/*
 * Exception type IDs passed to the C handler.
 * Encoded as (group << 2) | type.
 */
.set EXC_SYNC,		0
.set EXC_IRQ,		1
.set EXC_FIQ,		2
.set EXC_SERROR,	3

/*
 * Register save frame: 272 bytes
 *   x0-x30 (31 × 8 = 248 bytes)
 *   ELR_EL1 (8 bytes)
 *   SPSR_EL1 (8 bytes)
 *   padding to 16-byte alignment (8 bytes)
 */
.set FRAME_SIZE,	272

.macro SAVE_REGS
	sub	sp, sp, #FRAME_SIZE
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]
	mrs	x0, elr_el1
	mrs	x1, spsr_el1
	stp	x0, x1, [sp, #248]
.endm

.macro RESTORE_REGS
	ldp	x0, x1, [sp, #248]
	msr	elr_el1, x0
	msr	spsr_el1, x1
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #FRAME_SIZE
.endm

/*
 * Vector entry macro. Each entry must be exactly 128 bytes
 * (0x80). We save registers, set up arguments for the C
 * handler, call it, restore, and return from exception.
 */
.macro VECTOR_ENTRY label, type_id
.align 7	/* 128-byte alignment within the table */
\label:
	SAVE_REGS
	mov	x0, #\type_id		/* exception type */
	mrs	x1, esr_el1		/* exception syndrome */
	mrs	x2, elr_el1		/* faulting address */
	mrs	x3, far_el1		/* fault address register */
	bl	anx_exception_handler
	RESTORE_REGS
	eret
.endm

/* --- Vector table --- */
.section .vectors, "ax"
.global anx_vectors
.align 11	/* 2048-byte alignment */

anx_vectors:
	/* Group 0: Current EL with SP_EL0 (not used — we use SP_ELx) */
	VECTOR_ENTRY vec_el0_sync,   (0 << 2 | EXC_SYNC)
	VECTOR_ENTRY vec_el0_irq,    (0 << 2 | EXC_IRQ)
	VECTOR_ENTRY vec_el0_fiq,    (0 << 2 | EXC_FIQ)
	VECTOR_ENTRY vec_el0_serror, (0 << 2 | EXC_SERROR)

	/* Group 1: Current EL with SP_ELx (kernel exceptions) */
	VECTOR_ENTRY vec_elx_sync,   (1 << 2 | EXC_SYNC)
	VECTOR_ENTRY vec_elx_irq,    (1 << 2 | EXC_IRQ)
	VECTOR_ENTRY vec_elx_fiq,    (1 << 2 | EXC_FIQ)
	VECTOR_ENTRY vec_elx_serror, (1 << 2 | EXC_SERROR)

	/* Group 2: Lower EL, AArch64 (future userland) */
	VECTOR_ENTRY vec_lower64_sync,   (2 << 2 | EXC_SYNC)
	VECTOR_ENTRY vec_lower64_irq,    (2 << 2 | EXC_IRQ)
	VECTOR_ENTRY vec_lower64_fiq,    (2 << 2 | EXC_FIQ)
	VECTOR_ENTRY vec_lower64_serror, (2 << 2 | EXC_SERROR)

	/* Group 3: Lower EL, AArch32 (unused) */
	VECTOR_ENTRY vec_lower32_sync,   (3 << 2 | EXC_SYNC)
	VECTOR_ENTRY vec_lower32_irq,    (3 << 2 | EXC_IRQ)
	VECTOR_ENTRY vec_lower32_fiq,    (3 << 2 | EXC_FIQ)
	VECTOR_ENTRY vec_lower32_serror, (3 << 2 | EXC_SERROR)
