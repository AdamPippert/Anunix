/*
 * usermode.c — Ring-3 execution primitives for arm64: not implemented.
 *
 * The real ring-3 path exists only on x86_64, in usermode.S and
 * exec_load.c. arm64 needs an EL1-to-EL0 entry, an SVC vector that
 * routes to anx_syscall_trap(), and a user address space distinct from
 * the identity map. None of that is written yet.
 *
 * These definitions exist so the arm64 kernel links and boots. Every
 * other subsystem — including the object store, the RAID layer, and the
 * shell — works on arm64. Only `exec` of a ring-3 ELF does not.
 *
 * The stubs refuse loudly rather than half-working. A silent no-op here
 * would report a successful exec of a program that never ran.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/kprintf.h>
#include <anx/panic.h>

/*
 * Loading a segment means writing to the ELF's own virtual addresses.
 * On x86_64 those are identity-mapped and the write is a memcpy. arm64
 * has no user address space to write into, and the program can never
 * run, so the copy is skipped rather than scribbled somewhere.
 */
void arch_exec_load_segment(uint64_t vaddr, const void *src,
			     uint64_t filesz, uint64_t memsz)
{
	(void)vaddr;
	(void)src;
	(void)filesz;
	(void)memsz;
}

/*
 * Returns an exit status rather than an error code, because the caller
 * has no error channel here. The message is the real signal.
 */
uint64_t arch_enter_usermode(uint64_t entry, uint64_t user_rsp)
{
	(void)entry;
	(void)user_rsp;
	kprintf("exec: ring-3 execution is not implemented on arm64\n");
	return (uint64_t)ANX_ENOSYS;
}

/*
 * Reachable only from the exit syscall of a running ring-3 program.
 * arch_enter_usermode() never enters ring 3 on arm64, so no such
 * program exists and arriving here means the kernel lost track of its
 * own state.
 */
void arch_return_to_kernel(int code)
{
	(void)code;
	ANX_PANIC("arch_return_to_kernel on arm64: no ring-3 program in flight");
}
