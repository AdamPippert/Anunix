/*
 * boot.S — ARM64 (AArch64) boot entry point for Apple Silicon.
 *
 * This is the first code that runs. It sets up a minimal stack
 * and jumps to the C kernel entry point.
 */

.section .text.boot
.global _start

_start:
	/* Disable interrupts */
	msr	daifset, #0xf

	/* Set up stack pointer — grows downward from top of boot stack */
	adrp	x0, _stack_top
	add	x0, x0, :lo12:_stack_top
	mov	sp, x0

	/* Zero BSS */
	adrp	x0, _bss_start
	add	x0, x0, :lo12:_bss_start
	adrp	x1, _bss_end
	add	x1, x1, :lo12:_bss_end
1:
	cmp	x0, x1
	b.ge	2f
	str	xzr, [x0], #8
	b	1b
2:
	/* Jump to C kernel entry */
	bl	kernel_main

	/* Should not return — halt loop */
3:
	wfe
	b	3b

/*
 * Boot stack — 16 KiB, 16-byte aligned.
 */
.section .bss
.align 16
_stack_bottom:
	.space 16384
_stack_top:
