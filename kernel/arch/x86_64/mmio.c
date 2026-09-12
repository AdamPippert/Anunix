/*
 * mmio.c — x86_64 MMIO mapping.
 *
 * Adds 1 GiB identity pages to the active page tables so a driver can
 * reach a BAR the boot-time identity map does not cover.
 *
 * Both boot paths leave a bounded map behind. The QEMU trampoline
 * (qemu_boot.S) fills PDPT[0..3]: 4 GiB. The anxboot EFI stub does the
 * same and discards the firmware's larger map on the way past. UEFI then
 * assigns 64-bit BARs above that — 0xc000000000 on QEMU with OVMF — and
 * the first register access faults. boot.S's claim that "UEFI identity
 * mapping covers all physical memory" was true of the firmware's tables,
 * not of the ones the stub replaces them with.
 *
 * 1 GiB pages keep this cheap: a BAR needs one PDPT entry, and at most one
 * new PDPT page when the BAR lands in a 512 GiB region nothing has touched.
 */

#include <anx/mmio.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define PTE_PRESENT	(1ULL << 0)
#define PTE_RW		(1ULL << 1)
#define PTE_PWT		(1ULL << 3)	/* write-through */
#define PTE_PCD		(1ULL << 4)	/* cache disable */
#define PTE_PS		(1ULL << 7)	/* 1 GiB page at PDPT level */

#define GIB		(1ULL << 30)
#define ADDR_MASK	0x000FFFFFFFFFF000ULL

static uint64_t read_cr3(void)
{
	uint64_t v;

	__asm__ volatile("mov %%cr3, %0" : "=r"(v));
	return v;
}

static void flush_tlb(void)
{
	/*
	 * Reload CR3. Adding a present entry where none was does not
	 * strictly require a flush on x86-64, but the architecture permits
	 * caching not-present translations, and a stale negative entry here
	 * would fault on the very access this call exists to enable.
	 */
	uint64_t cr3 = read_cr3();

	__asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* Allocate a zeroed page table. Returns 0 on failure. */
static uint64_t alloc_table(void)
{
	uintptr_t page = anx_page_alloc(0);

	if (!page)
		return 0;
	anx_memset((void *)page, 0, 4096);
	return (uint64_t)page;
}

/* Map one 1 GiB-aligned region. Returns false on failure. */
static bool map_gib(uint64_t base)
{
	uint64_t *pml4 = (uint64_t *)(read_cr3() & ADDR_MASK);
	uint64_t *pdpt;
	uint32_t i4 = (uint32_t)((base >> 39) & 0x1FF);
	uint32_t i3 = (uint32_t)((base >> 30) & 0x1FF);

	if (!(pml4[i4] & PTE_PRESENT)) {
		uint64_t table = alloc_table();

		if (!table)
			return false;
		pml4[i4] = table | PTE_PRESENT | PTE_RW;
	}

	pdpt = (uint64_t *)(pml4[i4] & ADDR_MASK);

	if (pdpt[i3] & PTE_PRESENT) {
		/*
		 * Already mapped. Leave it alone: a 1 GiB page low in memory
		 * covers RAM as well as any MMIO in the same gigabyte, and
		 * making that range uncached would slow every access to the
		 * RAM sharing it.
		 */
		return true;
	}

	pdpt[i3] = (base & ~(GIB - 1)) | PTE_PRESENT | PTE_RW | PTE_PS |
		   PTE_PCD | PTE_PWT;
	return true;
}

void *anx_mmio_map(uint64_t phys, uint64_t size)
{
	uint64_t first, last, addr;

	if (size == 0)
		return NULL;

	/* Guard against a wrapping range before deriving anything from it. */
	if (phys > 0xFFFFFFFFFFFFFFFFULL - (size - 1))
		return NULL;

	first = phys & ~(GIB - 1);
	last  = (phys + size - 1) & ~(GIB - 1);

	for (addr = first; ; addr += GIB) {
		if (!map_gib(addr)) {
			kprintf("mmio: cannot map %llx (+%llu)\n",
				(unsigned long long)phys,
				(unsigned long long)size);
			return NULL;
		}
		if (addr == last)
			break;
	}

	flush_tlb();
	return (void *)(uintptr_t)phys;
}
