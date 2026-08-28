/*
 * usermode.S — x86_64 ring-3 entry/exit and the syscall trap stub.
 *
 * arch_enter_usermode() drops to ring 3 and does not return via a
 * normal ret — the exit syscall (isr_stub_syscall -> anx_syscall_trap
 * -> arch_return_to_kernel) resumes the saved kernel context, making
 * arch_enter_usermode() appear to "return" the exit code.
 */

.section .text

/*
 * isr_stub_syscall — entered via `int $0x80` from ring 3.
 * Convention: rax = syscall nr, rdi = a0, rsi = a1, rdx = a2.
 * anx_syscall_trap(nr, a0, a1, a2) uses the normal SysV C ABI, so
 * shuffle registers into rdi,rsi,rdx,rcx (in reverse order, so we
 * never clobber a source register before it's copied out).
 */
.global isr_stub_syscall
isr_stub_syscall:
	subq	$8, %rsp		/* the int-gate frame leaves rsp%16==8; realign for `call` */
	movq	%rdx, %rcx
	movq	%rsi, %rdx
	movq	%rdi, %rsi
	movq	%rax, %rdi
	call	anx_syscall_trap
	addq	$8, %rsp
	iretq

/*
 * uint64_t arch_enter_usermode(uint64_t entry /*rdi*/, uint64_t user_rsp /*rsi*/)
 */
.global arch_enter_usermode
arch_enter_usermode:
	/* Save callee-saved kernel context; kernel_ctx_rsp is the value of
	 * rsp at function entry, i.e. it points at our return address, so
	 * arch_return_to_kernel() can resume us with a plain `ret`. */
	movq	%rbx, kernel_ctx_rbx(%rip)
	movq	%rbp, kernel_ctx_rbp(%rip)
	movq	%r12, kernel_ctx_r12(%rip)
	movq	%r13, kernel_ctx_r13(%rip)
	movq	%r14, kernel_ctx_r14(%rip)
	movq	%r15, kernel_ctx_r15(%rip)
	movq	%rsp, kernel_ctx_rsp(%rip)

	movq	%rdi, %rax		/* entry */
	movq	%rsi, %rbx		/* user_rsp */

	/* Reload data segment registers for ring 3 (selector 0x23 | RPL 3) */
	movw	$0x23, %cx
	movw	%cx, %ds
	movw	%cx, %es
	movw	%cx, %fs
	movw	%cx, %gs

	/* iretq frame — RIP is popped first, so it's pushed last. */
	pushq	$0x23			/* user SS */
	pushq	%rbx			/* user RSP */
	pushq	$0x202			/* RFLAGS: IF=1 */
	pushq	$0x1B			/* user CS (RPL 3) */
	pushq	%rax			/* user RIP (entry) */
	iretq

/*
 * void arch_return_to_kernel(int code) — noreturn.
 * Restores the context saved by arch_enter_usermode() and returns
 * `code` (in eax) to whoever called it, as if it had returned normally.
 */
.global arch_return_to_kernel
arch_return_to_kernel:
	movl	%edi, %eax
	movq	kernel_ctx_rbx(%rip), %rbx
	movq	kernel_ctx_rbp(%rip), %rbp
	movq	kernel_ctx_r12(%rip), %r12
	movq	kernel_ctx_r13(%rip), %r13
	movq	kernel_ctx_r14(%rip), %r14
	movq	kernel_ctx_r15(%rip), %r15
	movq	kernel_ctx_rsp(%rip), %rsp
	ret

.section .bss
.align 8
kernel_ctx_rbx: .quad 0
kernel_ctx_rbp: .quad 0
kernel_ctx_r12: .quad 0
kernel_ctx_r13: .quad 0
kernel_ctx_r14: .quad 0
kernel_ctx_r15: .quad 0
kernel_ctx_rsp: .quad 0
