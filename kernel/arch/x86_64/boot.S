/*
 * boot.S — x86_64 boot entry point.
 *
 * Assumes 64-bit long mode with identity-mapped memory.
 * UEFI provides this natively. For QEMU, qemu_boot.S handles
 * the multiboot handshake and 32->64 mode switch first.
 *
 * We set up our own stack, zero BSS, and jump to C.
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
