/*
 * boot.S — x86_64 boot entry point for UEFI-booted systems.
 *
 * UEFI hands us control in 64-bit long mode with identity-mapped memory.
 * We set up our own stack and jump to the C kernel entry point.
 */

.section .text.boot
.global _start

_start:
	/* Disable interrupts */
	cli

	/* Set up stack pointer */
	leaq	_stack_top(%rip), %rsp

	/* Zero BSS */
	leaq	_bss_start(%rip), %rdi
	leaq	_bss_end(%rip), %rcx
	subq	%rdi, %rcx
	shrq	$3, %rcx		/* count in qwords */
	xorq	%rax, %rax
	rep	stosq

	/* Jump to C kernel entry */
	call	kernel_main

	/* Should not return — halt loop */
1:
	hlt
	jmp	1b

/*
 * Boot stack — 16 KiB, 16-byte aligned.
 */
.section .bss
.align 16
_stack_bottom:
	.space 16384
_stack_top:
