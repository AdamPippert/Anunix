/*
 * qemu_boot.S — 32-bit Multiboot bootstrap for QEMU testing.
 *
 * Compiled as i386, linked as ELF32. QEMU's multiboot loader
 * requires a 32-bit ELF.
 *
 * Layout in memory:
 *   0x100000  multiboot header + 32-bit entry (mode switch code)
 *   0x101000  BSS: page tables (12 KiB) + temp stack (16 KiB)
 *   0x108000  embedded 64-bit kernel binary (.incbin)
 *   0x110000  trampoline: copies kernel to 0x100000 and jumps
 *
 * This file is ONLY used for QEMU testing. Real hardware uses
 * UEFI which provides 64-bit mode directly.
 */

.set MAGIC,	0x1BADB002
.set FLAGS,	0x00000007	/* align modules + meminfo + video mode */
.set CHECKSUM,	-(MAGIC + FLAGS)

/* --- Multiboot header (must be in first 8 KiB) --- */
.section .multiboot, "a"
.align 4
	.long MAGIC
	.long FLAGS
	.long CHECKSUM
	/* Address fields (unused — ELF headers used instead) */
	.long 0			/* header_addr */
	.long 0			/* load_addr */
	.long 0			/* load_end_addr */
	.long 0			/* bss_end_addr */
	.long 0			/* entry_addr */
	/* Video mode request: linear framebuffer, 1024x768x32 */
	.long 0			/* mode_type = 0 (linear graphics) */
	.long 1024		/* width */
	.long 768		/* height */
	.long 32		/* depth (bits per pixel) */

/* --- 32-bit entry point --- */
.section .text
.global _start
.code32

_start:
	cli
	movl	%ebx, %esi		/* save multiboot info */

	/* Debug: write 'A' at top-left of VGA text buffer */
	movw	$0x4F41, 0xB8000	/* white 'A' on red */

/* Temporary stack in BSS */
	movl	$(boot_stack_top), %esp

	/* Zero page tables (2 pages = 8 KiB) */
	movl	$boot_pml4, %edi
	xorl	%eax, %eax
	movl	$(2 * 4096 / 4), %ecx
	rep	stosl

	/* PML4[0] -> PDPT */
	movl	$(boot_pdpt + 0x3), boot_pml4

	/*
	 * PDPT[0..3] -> 1 GiB identity pages (first 4 GiB).
	 * 1 GiB pages (PS bit 7) are supported by all AMD64 CPUs.
	 * 4 GiB covers the framebuffer which PCI maps below 4 GiB.
	 */
	movl	$0x83, boot_pdpt + 0		/* 0x00000000 - 0x3FFFFFFF */
	movl	$0, boot_pdpt + 4
	movl	$0x40000083, boot_pdpt + 8	/* 0x40000000 - 0x7FFFFFFF */
	movl	$0, boot_pdpt + 12
	movl	$0x80000083, boot_pdpt + 16	/* 0x80000000 - 0xBFFFFFFF */
	movl	$0, boot_pdpt + 20
	movl	$0xC0000083, boot_pdpt + 24	/* 0xC0000000 - 0xFFFFFFFF */
	movl	$0, boot_pdpt + 28

	/* Debug: 'B' = page tables built */
	movw	$0x4F42, 0xB8002

	/* CR3 = PML4 */
	movl	$boot_pml4, %eax
	movl	%eax, %cr3

	/* Enable PAE */
	movl	%cr4, %eax
	orl	$(1 << 5), %eax
	movl	%eax, %cr4

	/* Enable long mode (IA32_EFER.LME) */
	movl	$0xC0000080, %ecx
	rdmsr
	orl	$(1 << 8), %eax
	wrmsr

	/* Enable paging */
	movl	%cr0, %eax
	orl	$(1 << 31), %eax
	movl	%eax, %cr0

	/* Debug: 'C' = long mode enabled, about to jump */
	movw	$0x4F43, 0xB8004

/* Jump to 64-bit mode via GDT */
	lgdt	boot_gdt_ptr
	ljmp	$0x08, $trampoline

/* --- GDT (minimal 64-bit) --- */
.section .rodata
.align 16
boot_gdt:
	.quad	0x0000000000000000	/* null */
	.quad	0x00AF9A000000FFFF	/* 64-bit code */
	.quad	0x00CF92000000FFFF	/* data */
boot_gdt_end:

boot_gdt_ptr:
	.word	boot_gdt_end - boot_gdt - 1
	.long	boot_gdt

/* --- Page tables (BSS) --- */
.section .bss
.align 4096
boot_pml4:
	.space 4096
boot_pdpt:
	.space 4096

.align 16
boot_stack:
	.space 16384
boot_stack_top:

/* --- Embedded 64-bit kernel binary --- */
.section .kernel, "a"
.global kernel_payload
kernel_payload:
	.incbin "build/x86_64/anunix.bin"

/*
 * Trampoline — placed ABOVE the kernel payload so it doesn't
 * get clobbered when we copy the kernel to 0x100000.
 */
.section .trampoline, "ax"
.code64
.global trampoline
trampoline:
/* Reload data segments */
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss

	/*
	 * Save multiboot info at 0x1000-0x1028.
	 * ESI holds multiboot info pointer (saved in 32-bit entry).
	 *
	 * We store at 0x1000:
	 *   0x1000: magic (0x414E5846 = "ANXF")
	 *   0x1004: flags from multiboot info
	 *   0x1008: framebuffer_addr (u64)
	 *   0x1010: framebuffer_pitch (u32)
	 *   0x1014: framebuffer_width (u32)
	 *   0x1018: framebuffer_height (u32)
	 *   0x101C: framebuffer_bpp (u8)
	 *   0x101D: framebuffer_type (u8)
	 *   0x1020: cmdline pointer (u64, zero-extended from u32)
	 */
	movq	%rsi, %rbx		/* multiboot info pointer */
	movl	$0x414E5846, 0x1000	/* magic "ANXF" */

	/* Copy flags field (offset 0 in multiboot info) */
	movl	(%rbx), %eax
	movl	%eax, 0x1004

	/* Check if framebuffer info present (flags bit 12) */
	testl	$(1 << 12), %eax
	jz	.Lno_fb

	/* Copy framebuffer fields */
	movq	88(%rbx), %rax		/* framebuffer_addr */
	movq	%rax, 0x1008
	movl	96(%rbx), %eax		/* framebuffer_pitch */
	movl	%eax, 0x1010
	movl	100(%rbx), %eax		/* framebuffer_width */
	movl	%eax, 0x1014
	movl	104(%rbx), %eax		/* framebuffer_height */
	movl	%eax, 0x1018
	movb	108(%rbx), %al		/* framebuffer_bpp */
	movb	%al, 0x101C
	movb	109(%rbx), %al		/* framebuffer_type */
	movb	%al, 0x101D

.Lno_fb:
	/* Save cmdline pointer (offset 16, valid when flags bit 2 set) */
	movl	(%rbx), %eax		/* flags */
	testl	$(1 << 2), %eax
	jz	.Lno_cmdline
	movl	16(%rbx), %eax		/* cmdline (u32 phys addr) */
	movq	%rax, 0x1020		/* zero-extend to u64 */
.Lno_cmdline:

	/* Debug: 'D' = in 64-bit, about to copy kernel */
	movw	$0x4F44, 0xB8006

	/* Copy kernel from kernel_payload to 0x100000 */
	movq	$kernel_payload, %rsi
	movq	$0x100000, %rdi
	movq	$_kernel_size, %rcx
	shrq	$3, %rcx		/* qword count */
	addq	$1, %rcx		/* round up */
	rep	movsq

	/* Debug: 'E' = kernel copied, jumping */
	movw	$0x4F45, 0xB8008

/* Jump to kernel entry at 0x100000 */
	movq	$0x100000, %rax
	jmp	*%rax
