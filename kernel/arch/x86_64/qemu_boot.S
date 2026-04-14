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
.set FLAGS,	0x00000003	/* align modules + provide meminfo */
.set CHECKSUM,	-(MAGIC + FLAGS)

/* --- Multiboot header (must be in first 8 KiB) --- */
.section .multiboot, "a"
.align 4
	.long MAGIC
	.long FLAGS
	.long CHECKSUM

/* --- 32-bit entry point --- */
.section .text
.global _start
.code32

_start:
	cli
	movl	%ebx, %esi		/* save multiboot info */

/* Temporary stack in BSS */
	movl	$(boot_stack_top), %esp

	/* Zero page tables (3 pages = 12 KiB) */
	movl	$boot_pml4, %edi
	xorl	%eax, %eax
	movl	$(3 * 4096 / 4), %ecx
	rep	stosl

	/* PML4[0] -> PDPT */
	movl	$(boot_pdpt + 0x3), boot_pml4

	/* PDPT[0] -> PD */
	movl	$(boot_pd + 0x3), boot_pdpt

	/* PD[0..511] -> 2 MiB identity pages (first 1 GiB) */
	movl	$boot_pd, %edi
	movl	$0x83, %eax		/* present + writable + 2MiB */
	movl	$512, %ecx
1:	movl	%eax, (%edi)
	movl	$0, 4(%edi)
	addl	$0x200000, %eax
	addl	$8, %edi
	decl	%ecx
	jnz	1b

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
boot_pd:
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

	/* Copy kernel from kernel_payload to 0x100000 */
	movq	$kernel_payload, %rsi
	movq	$0x100000, %rdi
	movq	$_kernel_size, %rcx
	shrq	$3, %rcx		/* qword count */
	addq	$1, %rcx		/* round up */
	rep	movsq

/* Jump to kernel entry at 0x100000 */
	movq	$0x100000, %rax
	jmp	*%rax
