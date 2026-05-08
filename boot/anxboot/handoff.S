/*
 * handoff.S — anxboot kernel handoff trampoline.
 *
 * Called once UEFI Boot Services have been exited. Sets up the
 * register state Anunix's multiboot2 entry expects and jumps:
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
	jmpq	*%r10
