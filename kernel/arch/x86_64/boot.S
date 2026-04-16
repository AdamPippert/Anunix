/*
 * boot.S — x86_64 boot entry point.
 *
 * Supports two entry paths:
 *
 * 1. Multiboot2 EFI64 (real hardware / UEFI):
 *    GRUB enters _start directly in 64-bit long mode.
 *    RAX = 0x36d76289 (multiboot2 magic)
 *    RBX = physical address of multiboot2 info structure
 *    UEFI identity mapping covers all physical memory.
 *
 * 2. QEMU trampoline (qemu_boot.S):
 *    32-bit multiboot wrapper switches to long mode, copies
 *    kernel to 0x100000, and jumps here. Multiboot1 framebuffer
 *    info stashed at physical address 0x1000.
 *
 * Both paths provide: 64-bit mode, identity-mapped memory,
 * interrupts disabled. We set up our own stack, zero BSS,
 * save boot info, and jump to C.
 */

/* ---- Multiboot2 header ---- */

/*
 * Must appear within the first 32 KiB of the kernel binary.
 * Architecture = 0 (i386 / x86 family) per spec, even for
 * 64-bit kernels — the EFI64 entry tag selects 64-bit entry.
 */
#define MB2_MAGIC	0xe85250d6
#define MB2_ARCH	0		/* i386 / x86 family */

.section .multiboot2, "a"
.align 8
mb2_start:
	.long	MB2_MAGIC
	.long	MB2_ARCH
	.long	mb2_end - mb2_start
	.long	-(MB2_MAGIC + MB2_ARCH + (mb2_end - mb2_start))

	/* Information request tag — ask for framebuffer and memory map */
	.align 8
	.short	1		/* type: information request */
	.short	0		/* flags: required */
	.long	20		/* size: 8 + 3*4 */
	.long	6		/* mbi tag: memory map */
	.long	7		/* mbi tag: boot loader name */
	.long	8		/* mbi tag: framebuffer */

	/* Framebuffer request tag — 0/0/0 = let GRUB pick best mode */
	.align 8
	.short	5		/* type: framebuffer */
	.short	1		/* flags: optional (bit 0 set) */
	.long	20		/* size */
	.long	0		/* width: no preference */
	.long	0		/* height: no preference */
	.long	0		/* depth: no preference */

	/* EFI amd64 entry address — GRUB uses this on UEFI */
	.align 8
	.short	9		/* type: entry_address_efi64 */
	.short	0		/* flags */
	.long	12		/* size */
	.long	_start		/* 64-bit entry point */

	/* End tag */
	.align 8
	.short	0		/* type: end */
	.short	0
	.long	8
mb2_end:

/* ---- 64-bit entry point ---- */

.section .text.boot
.global _start

_start:
	cli

	/* Debug: 'F' = kernel _start reached */
	movw	$0x4F46, 0xB800A

	/*
	 * Save boot info before anything clobbers RAX/RBX.
	 * Callee-saved registers survive BSS zeroing.
	 */
	movq	%rax, %r12	/* multiboot magic (or garbage from QEMU path) */
	movq	%rbx, %r13	/* multiboot info pointer */

	/* Set up stack pointer */
	leaq	_stack_top(%rip), %rsp

	/* Zero BSS */
	leaq	_bss_start(%rip), %rdi
	leaq	_bss_end(%rip), %rcx
	subq	%rdi, %rcx
	shrq	$3, %rcx	/* count in qwords */
	xorq	%rax, %rax
	rep	stosq

	/* Store boot info to globals (in .data, not zeroed by BSS clear) */
	leaq	_mb_magic(%rip), %rax
	movl	%r12d, (%rax)
	leaq	_mb_info(%rip), %rax
	movq	%r13, (%rax)

	/* Jump to C kernel entry */
	call	kernel_main

	/* Should not return — halt loop */
1:
	hlt
	jmp	1b

/* ---- Boot data ---- */

/*
 * Multiboot info saved by _start. Placed in .data (not .bss)
 * so BSS zeroing doesn't clobber them.
 */
.section .data
.global _mb_magic
.global _mb_info
_mb_magic:
	.long	0
_mb_info:
	.quad	0

/*
 * Boot stack — 64 KiB, 16-byte aligned.
 * Sized for deep call chains: JPEG decode, GUI rendering,
 * HTTP/TCP networking, and the shell input loop.
 */
.section .bss
.align 16
_stack_bottom:
	.space 65536
_stack_top:
