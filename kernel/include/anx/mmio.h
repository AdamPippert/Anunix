/*
 * anx/mmio.h — Map device MMIO into the kernel's address space.
 *
 * The kernel runs on an identity map, so a physical address is usually
 * also a valid pointer. "Usually" is the problem: the identity map covers
 * a bounded range, and UEFI firmware places 64-bit PCI BARs well outside
 * it. On QEMU with OVMF an NVMe controller lands at 0xc000000000 — 768 GiB
 * — against a 4 GiB map, and the first register read page-faults.
 *
 * A driver must therefore map its BAR before touching it, rather than
 * assuming the identity map already reaches that far.
 */

#ifndef ANX_MMIO_H
#define ANX_MMIO_H

#include <anx/types.h>

/*
 * Make the physical range [phys, phys + size) readable and writable as
 * device memory, and return a pointer to it.
 *
 * The mapping is uncached: MMIO registers have side effects, and a cached
 * mapping would let the CPU satisfy a read from a stale line or reorder a
 * write past a doorbell.
 *
 * Returns NULL when the range cannot be mapped. A driver that gets NULL
 * MUST NOT fall back to using `phys` as a pointer — that is exactly the
 * assumption this call exists to replace.
 *
 * Mappings are permanent. Devices are probed once and their registers live
 * as long as the kernel does, so there is no unmap.
 */
void *anx_mmio_map(uint64_t phys, uint64_t size);

/*
 * The page-table entry backing a virtual address, for a driver that needs to
 * confirm its BAR really is mapped as device memory. Bit 3 is PWT and bit 4
 * is PCD; both set means uncached. Returns 0 when nothing maps the address.
 */
uint64_t anx_mmio_pte(const void *va);

#endif /* ANX_MMIO_H */
