/*
 * handoff.S — anxboot kernel handoff trampoline.
 *
 * Called once UEFI Boot Services have been exited. Reads our pre-
 * built identity-RWX 0..4 GiB PML4 from the fixed scratch slot at
 * physical 0x1FF8 (set by anxboot.c after building the tables); if
 * non-zero, switches CR3 there so the kernel doesn't inherit UEFI's
 * W^X mapping (which traps the kernel at its first instruction
 * fetch on Framework Desktop and other strict UEFI implementations).
 *
 * Then sets up multiboot2 register state and jumps:
 *
 *   RAX = magic (0x36d76289)
 *   RBX = multiboot2 info pointer
 *   RIP = kernel entry point
 *
 * Microsoft x64 calling convention (UEFI ABI):
 *   rcx = entry, rdx = magic, r8 = info_ptr
 */

.section .text,"xr"
.global anx_jump_to_kernel

anx_jump_to_kernel:
	cli
	movq	%rcx, %r10	/* save entry */
	movl	%edx, %eax	/* RAX = magic */
	movq	%r8,  %rbx	/* RBX = info pointer */

	/* Load PML4 phys from absolute slot 0x1FF8; switch CR3 if non-zero. */
	movabsq	$0x1FF8, %r11
	movq	(%r11), %r11
	testq	%r11, %r11
	je	1f
	movq	%r11, %cr3
1:
	jmpq	*%r10
