/*
 * idt.S — x86_64 ISR stubs for exceptions and IRQs.
 *
 * Each stub pushes a uniform frame (error code + vector number)
 * then jumps to a common handler that saves all GP registers
 * and calls the C dispatch function.
 *
 * Exceptions 8, 10-14, 17, 21, 29, 30 push an error code;
 * all others get a dummy 0 pushed for a uniform frame.
 */

.section .text

/*
 * ISR stub macros.
 * Frame on entry to common_stub:
 *   [rsp+0]  vector number (pushed by stub)
 *   [rsp+8]  error code (pushed by CPU or stub)
 *   [rsp+16] RIP (pushed by CPU)
 *   [rsp+24] CS
 *   [rsp+32] RFLAGS
 *   [rsp+40] RSP (if privilege change)
 *   [rsp+48] SS  (if privilege change)
 */

/* Exception without error code — push dummy 0 */
.macro ISR_NOERR n
.global isr_stub_\n
isr_stub_\n:
	pushq	$0		/* dummy error code */
	pushq	$\n		/* vector number */
	jmp	common_stub
.endm

/* Exception with CPU-pushed error code */
.macro ISR_ERR n
.global isr_stub_\n
isr_stub_\n:
	pushq	$\n		/* vector number (error code already on stack) */
	jmp	common_stub
.endm

/*
 * Common stub: save all GP registers, call C handler, restore, iretq.
 * On entry: [rsp] = vector, [rsp+8] = error code, then CPU frame.
 */
common_stub:
	/* Save GP registers (128 bytes) */
	pushq	%rax
	pushq	%rbx
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%rbp
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	/* Call C handler: anx_exception_dispatch(vector, error_code, frame) */
	movq	120(%rsp), %rdi		/* vector number */
	movq	128(%rsp), %rsi		/* error code */
	movq	%rsp, %rdx		/* pointer to saved registers */
	call	anx_exception_dispatch

	/* Restore GP registers */
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rbp
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rbx
	popq	%rax

	/* Pop vector number and error code */
	addq	$16, %rsp

	iretq

/* --- Exception stubs (0-31) --- */
ISR_NOERR 0	/* Divide by zero */
ISR_NOERR 1	/* Debug */
ISR_NOERR 2	/* NMI */
ISR_NOERR 3	/* Breakpoint */
ISR_NOERR 4	/* Overflow */
ISR_NOERR 5	/* Bound range */
ISR_NOERR 6	/* Invalid opcode */
ISR_NOERR 7	/* Device not available */
ISR_ERR   8	/* Double fault */
ISR_NOERR 9	/* Coprocessor segment overrun */
ISR_ERR   10	/* Invalid TSS */
ISR_ERR   11	/* Segment not present */
ISR_ERR   12	/* Stack-segment fault */
ISR_ERR   13	/* General protection fault */
ISR_ERR   14	/* Page fault */
ISR_NOERR 15	/* Reserved */
ISR_NOERR 16	/* x87 FP exception */
ISR_ERR   17	/* Alignment check */
ISR_NOERR 18	/* Machine check */
ISR_NOERR 19	/* SIMD FP exception */
ISR_NOERR 20	/* Virtualization exception */
ISR_ERR   21	/* Control protection */
ISR_NOERR 22	/* Reserved */
ISR_NOERR 23	/* Reserved */
ISR_NOERR 24	/* Reserved */
ISR_NOERR 25	/* Reserved */
ISR_NOERR 26	/* Reserved */
ISR_NOERR 27	/* Reserved */
ISR_NOERR 28	/* Hypervisor injection */
ISR_ERR   29	/* VMM communication */
ISR_ERR   30	/* Security exception */
ISR_NOERR 31	/* Reserved */

/* --- IRQ stubs (32-47) --- */
ISR_NOERR 32	/* PIT timer (IRQ 0) */
ISR_NOERR 33	/* Keyboard (IRQ 1) */
ISR_NOERR 34	/* Cascade (IRQ 2) */
ISR_NOERR 35	/* COM2 (IRQ 3) */
ISR_NOERR 36	/* COM1 (IRQ 4) */
ISR_NOERR 37	/* LPT2 (IRQ 5) */
ISR_NOERR 38	/* Floppy (IRQ 6) */
ISR_NOERR 39	/* LPT1 / spurious (IRQ 7) */
ISR_NOERR 40	/* RTC (IRQ 8) */
ISR_NOERR 41	/* Free (IRQ 9) */
ISR_NOERR 42	/* Free (IRQ 10) */
ISR_NOERR 43	/* Free (IRQ 11) */
ISR_NOERR 44	/* PS/2 mouse (IRQ 12) */
ISR_NOERR 45	/* FPU (IRQ 13) */
ISR_NOERR 46	/* Primary ATA (IRQ 14) */
ISR_NOERR 47	/* Secondary ATA (IRQ 15) */

/*
 * Default stub for vectors 48-255 (LAPIC, MSI, spurious).
 * Pushes a sentinel vector so the C dispatch can identify it
 * as unexpected and do a safe EOI + return.
 */
.global isr_stub_default
isr_stub_default:
	pushq	$0		/* dummy error code */
	pushq	$0xFF		/* sentinel: "unexpected vector" */
	jmp	common_stub

/*
 * ISR stub table — array of 48 function pointers for IDT setup.
 * The C code indexes this to fill IDT entries.
 * Vectors 48-255 use isr_stub_default (set by C code).
 */
.section .rodata
.global isr_stub_table
.align 8
isr_stub_table:
.irp i, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	.quad isr_stub_\i
.endr
.irp i, 16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.quad isr_stub_\i
.endr
.irp i, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
	.quad isr_stub_\i
.endr
