/*
 * boot.S — Heteris (RV64IM + AI extensions) boot entry point.
 *
 * This is the first code that runs on the OS lane.
 * Sets up stack, zeros BSS, jumps to C kernel entry.
 *
 * Runs in M-mode on the RV64 control core.
 * AI lane is idle until arch_init() configures tiles.
 */

.section .text.boot
.global _start

_start:
	/* Disable interrupts (clear MIE in mstatus) */
	csrci	mstatus, 0x8

	/* Set up stack pointer — grows downward from _stack_top */
	la	sp, _stack_top

	/* Zero BSS */
	la	t0, _bss_start
	la	t1, _bss_end
1:
	bge	t0, t1, 2f
	sd	zero, 0(t0)
	addi	t0, t0, 8
	j	1b
2:
	/* Jump to C kernel entry */
	call	kernel_main

	/* Should not return — halt via WFI loop */
3:
	wfi
	j	3b

/*
 * Boot stack — 16 KiB, 16-byte aligned.
 */
.section .bss
.align 16
_stack_bottom:
	.space 16384
_stack_top:
