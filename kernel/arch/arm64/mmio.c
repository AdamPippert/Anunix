/*
 * mmio.c — arm64 MMIO mapping.
 *
 * The arm64 port boots with a flat mapping established before the kernel
 * runs, and does not yet build page tables of its own, so there is nothing
 * to add an entry to. Returning the identity address preserves today's
 * behaviour exactly.
 *
 * When arm64 grows its own tables this must map the range the way the
 * x86_64 version does — device memory, not normal memory — or every MMIO
 * access becomes cacheable and reorderable.
 */

#include <anx/mmio.h>

void *anx_mmio_map(uint64_t phys, uint64_t size)
{
	(void)size;
	return (void *)(uintptr_t)phys;
}

/*
 * No page tables of our own yet, so there is no entry to report. Returning 0
 * says "nothing maps this", which is honest: a caller checking cache bits
 * must not be told the mapping is device memory when nothing has arranged
 * for it to be.
 */
uint64_t anx_mmio_pte(const void *va)
{
	(void)va;
	return 0;
}

/* No PAT on arm64; memory attributes live in MAIR_EL1, not set up yet. */
bool anx_pat_enable_wc(void)
{
	return false;
}

void *anx_mmio_map_wc(uint64_t phys, uint64_t size)
{
	(void)size;
	return (void *)(uintptr_t)phys;
}
