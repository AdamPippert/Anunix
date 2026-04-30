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
/*
 * FLAGS bits:
 *   bit 0 (0x1) — align modules on 4 KiB pages
 *   bit 1 (0x2) — provide memory map
 *
 * Bit 2 (framebuffer request) is intentionally omitted: some QEMU builds
 * reject it and abort. The kernel detects the Bochs VBE framebuffer directly
 * via I/O port probing (fb_detect_bochs) which works with -vga std.
 */
.set FLAGS,	0x00000003
.set CHECKSUM,	-(MAGIC + FLAGS)

/* --- Multiboot header (must be in first 8 KiB) --- */
.section .multiboot, "a"
.align 4
	.long MAGIC
	.long FLAGS
	.long CHECKSUM
	/* Address fields (unused — ELF headers used instead; required before video fields) */
	.long 0			/* header_addr */
	.long 0			/* load_addr */
	.long 0			/* load_end_addr */
	.long 0			/* bss_end_addr */
	.long 0			/* entry_addr */
	/* Video mode fields (FLAGS bit 2) — 0 = any linear-framebuffer mode */
	.long 0			/* mode_type: 0 = linear graphics */
	.long 0			/* width:     0 = no preference */
	.long 0			/* height:    0 = no preference */
	.long 0			/* depth:     0 = no preference */

/* --- 32-bit entry point --- */
.section .text
.global _start
.code32

/*
 * Debug macro: draw a colored horizontal bar on the GOP framebuffer.
 * Uses the framebuffer address from multiboot info (ESI).
 * \color = XRGB8888 pixel value, \row = which 10-pixel row band.
 * Only works if multiboot flags bit 12 is set.
 */
.macro FB_DEBUG color, row
	pushl	%eax
	pushl	%ecx
	pushl	%edi
	movl	(%esi), %eax		/* flags */
	testl	$(1 << 12), %eax
	jz	99f
	movl	88(%esi), %edi		/* framebuffer_addr (low 32 bits) */
	testl	%edi, %edi
	jz	99f
	/* offset = row * 10 * pitch */
	movl	96(%esi), %eax		/* pitch (bytes per scanline) */
	imull	$(\row * 10), %eax
	addl	%eax, %edi
	/* fill 10 rows x 200 pixels */
	movl	$\color, %eax
	movl	$(200 * 10), %ecx	/* 200 pixels wide, 10 rows */
1:	movl	%eax, (%edi)
	addl	$4, %edi
	decl	%ecx
	jnz	1b
99:
	popl	%edi
	popl	%ecx
	popl	%eax
.endm

/*
 * Minimal UART init + single-byte debug output.
 * Called in 32-bit protected mode before any stack or paging.
 * Uses only AL and DX; clobbers nothing else.
 */
.macro UART_INIT_AND_SEND byte
	/* Init COM1 at 115200 8N1 */
	movw	$0x3F9, %dx; xorb %al, %al; outb %al, %dx	/* IER=0 */
	movw	$0x3FB, %dx; movb $0x80, %al; outb %al, %dx	/* DLAB=1 */
	movw	$0x3F8, %dx; movb $0x01, %al; outb %al, %dx	/* DLL=1 */
	movw	$0x3F9, %dx; xorb %al, %al; outb %al, %dx	/* DLH=0 */
	movw	$0x3FB, %dx; movb $0x03, %al; outb %al, %dx	/* 8N1 */
	/* Wait for transmit-hold-register empty (LSR bit 5) */
1:	movw	$0x3FD, %dx; inb %dx, %al; testb $0x20, %al; jz 1b
	movw	$0x3F8, %dx; movb $(\byte), %al; outb %al, %dx
.endm

.macro UART_SEND byte
	/* Wait for THR empty */
1:	movw	$0x3FD, %dx; inb %dx, %al; testb $0x20, %al; jz 1b
	movw	$0x3F8, %dx; movb $(\byte), %al; outb %al, %dx
.endm

_start:
	cli
	movl	%ebx, %esi		/* save multiboot info */

	/* Stack must be live before FB_DEBUG (uses push/pop) */
	movl	$(boot_stack_top), %esp

	UART_INIT_AND_SEND '1'		/* checkpoint: 32-bit entry reached */

	/* Debug: red bar = wrapper entry */
	FB_DEBUG 0x00FF0000, 0

	UART_SEND '2'			/* checkpoint: FB_DEBUG survived */

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

	UART_SEND '3'			/* checkpoint: page tables built */

	/* Debug: green bar = page tables built */
	FB_DEBUG 0x0000FF00, 1

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

	UART_SEND '4'			/* checkpoint: paging enabled */

	/* Debug: blue bar = long mode enabled */
	FB_DEBUG 0x000000FF, 2

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

	/* UART checkpoint: 64-bit trampoline entry */
	movw	$0x3FD, %dx
.Lwait5: inb %dx, %al; testb $0x20, %al; jz .Lwait5
	movw	$0x3F8, %dx; movb $'5', %al; outb %al, %dx

	/*
	 * Read multiboot info fields into registers FIRST, before we
	 * write our boot info block at 0x1000 (which might alias the
	 * multiboot info structure if QEMU placed it at 0x1000).
	 */
	movl	(%rbx), %r8d		/* r8d = multiboot flags */
	xorq	%r9, %r9		/* r9 = framebuffer_addr (0 = none) */
	xorl	%r10d, %r10d		/* r10d = pitch */
	xorl	%r11d, %r11d		/* r11d = width */
	xorl	%r14d, %r14d		/* r14d = height (0 = unknown) */

	testl	$(1 << 12), %r8d	/* framebuffer field present? */
	jz	.Lno_fb
	movq	88(%rbx), %r9		/* framebuffer_addr */
	movl	96(%rbx), %r10d		/* pitch */
	movl	100(%rbx), %r11d	/* width */
	movl	104(%rbx), %r14d	/* height */
	movzbl	108(%rbx), %eax		/* bpp */
	movl	%eax, %r12d		/* save bpp */
	movzbl	109(%rbx), %eax		/* type */
	movl	%eax, %r13d		/* save type */
	jmp	.Lhave_fb
.Lno_fb:
	xorl	%r12d, %r12d
	xorl	%r13d, %r13d
.Lhave_fb:

	/* Read cmdline pointer before overwriting 0x1000 */
	xorq	%rcx, %rcx
	testl	$(1 << 2), %r8d
	jz	.Lno_cmdline_r
	movl	16(%rbx), %ecx		/* cmdline (u32, zero-extend to r/rcx) */
.Lno_cmdline_r:

	/* Now safe to write boot info block at 0x1000 */
	movl	$0x414E5846, 0x1000	/* magic "ANXF" */
	movl	%r8d, 0x1004		/* flags */
	movq	%r9, 0x1008		/* framebuffer_addr */
	movl	%r10d, 0x1010		/* pitch */
	movl	%r11d, 0x1014		/* width */
	movl	%r14d, 0x1018		/* height */
	movb	%r12b, 0x101C		/* bpp */
	movb	%r13b, 0x101D		/* type */
	movq	%rcx, 0x1020		/* cmdline pointer */

	/* UART checkpoint: about to copy kernel */
	movw	$0x3FD, %dx
.Lwait6: inb %dx, %al; testb $0x20, %al; jz .Lwait6
	movw	$0x3F8, %dx; movb $'6', %al; outb %al, %dx

	/* Copy kernel from kernel_payload to 0x100000 */
	movq	$kernel_payload, %rsi
	movq	$0x100000, %rdi
	movq	$_kernel_size, %rcx
	shrq	$3, %rcx		/* qword count */
	addq	$1, %rcx		/* round up */
	rep	movsq

	/* UART checkpoint: kernel copied, about to jump */
	movw	$0x3FD, %dx
.Lwait7: inb %dx, %al; testb $0x20, %al; jz .Lwait7
	movw	$0x3F8, %dx; movb $'7', %al; outb %al, %dx

/* Jump to kernel entry at 0x100000 */
	movq	$0x100000, %rax
	jmp	*%rax
