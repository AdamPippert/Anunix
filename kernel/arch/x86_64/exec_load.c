/*
 * exec_load.c — arch_exec_load_segment() for x86_64.
 *
 * The ring-3 entry/exit and syscall trap assembly live in usermode.S;
 * this is the one piece of segment loading that's genuinely
 * architecture-specific (it assumes vaddr is a physical==virtual
 * identity-mapped address, only true inside the real kernel).
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/string.h>

void arch_exec_load_segment(uint64_t vaddr, const void *src,
			     uint64_t filesz, uint64_t memsz)
{
	anx_memcpy((void *)(uintptr_t)vaddr, src, filesz);
	if (memsz > filesz)
		anx_memset((void *)(uintptr_t)(vaddr + filesz), 0, memsz - filesz);
}
