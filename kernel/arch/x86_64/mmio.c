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
#define PTE_PS		(1ULL << 7)	/* large page (1 GiB at PDPT, 2 MiB at PD) */

#define GIB		(1ULL << 30)
#define MIB2		(2ULL << 20)
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

/*
 * Split a 1 GiB page into 512 x 2 MiB pages covering the same range with the
 * same attributes. Returns the page directory, or 0 on failure.
 *
 * This is what makes it possible to uncache a BAR that shares a gigabyte
 * with RAM. The old code saw a present 1 GiB entry, concluded that
 * uncaching it would slow every RAM access in the same gigabyte -- which is
 * true -- and left the mapping alone. That was the wrong conclusion: it
 * meant anx_mmio_map() silently returned a write-back mapping for every BAR
 * the boot map already covered, which on a machine with BARs under 4 GiB is
 * all of them. Drivers then read stale cache lines and wrote registers that
 * never reached the device.
 */
static uint64_t split_1gib(uint64_t entry)
{
	uint64_t base  = entry & ADDR_MASK & ~(GIB - 1);
	uint64_t flags = entry & (PTE_PRESENT | PTE_RW | PTE_PCD | PTE_PWT);
	uint64_t pd    = alloc_table();
	uint64_t *e;
	uint32_t i;

	if (!pd)
		return 0;

	e = (uint64_t *)pd;
	for (i = 0; i < 512; i++)
		e[i] = (base + (uint64_t)i * MIB2) | flags | PTE_PS;

	return pd;
}

/*
 * Make [base, base + size) uncached at 2 MiB granularity, splitting a 1 GiB
 * page if one covers it. RAM in the same gigabyte outside the requested
 * range keeps its original attributes.
 */
static bool map_cache_attr(uint64_t base, uint64_t size, uint64_t attr)
{
	uint64_t *pml4 = (uint64_t *)(read_cr3() & ADDR_MASK);
	uint64_t *pdpt, *pd;
	uint64_t first = base & ~(MIB2 - 1);
	uint64_t last  = (base + size - 1) & ~(MIB2 - 1);
	uint64_t addr;

	for (addr = first; addr <= last; addr += MIB2) {
		uint32_t i4 = (uint32_t)((addr >> 39) & 0x1FF);
		uint32_t i3 = (uint32_t)((addr >> 30) & 0x1FF);
		uint32_t i2 = (uint32_t)((addr >> 21) & 0x1FF);

		if (!(pml4[i4] & PTE_PRESENT)) {
			uint64_t t = alloc_table();

			if (!t)
				return false;
			pml4[i4] = t | PTE_PRESENT | PTE_RW;
		}
		pdpt = (uint64_t *)(pml4[i4] & ADDR_MASK);

		if (!(pdpt[i3] & PTE_PRESENT)) {
			uint64_t t = alloc_table();

			if (!t)
				return false;
			pdpt[i3] = t | PTE_PRESENT | PTE_RW;
		} else if (pdpt[i3] & PTE_PS) {
			uint64_t t = split_1gib(pdpt[i3]);

			if (!t)
				return false;
			pdpt[i3] = t | PTE_PRESENT | PTE_RW;
		}
		pd = (uint64_t *)(pdpt[i3] & ADDR_MASK);

		pd[i2] = (addr & ~(MIB2 - 1)) | PTE_PRESENT | PTE_RW | PTE_PS |
			 attr;
	}
	return true;
}

/*
 * Report the page-table entry backing a virtual address, so a driver can
 * prove what the CPU thinks its BAR is rather than trusting that a call to
 * anx_mmio_map() did what it says. That trust was misplaced once already.
 *
 * Returns the entry, or 0 when nothing maps the address.
 */
uint64_t anx_mmio_pte(const void *va)
{
	uint64_t addr  = (uint64_t)(uintptr_t)va;
	uint64_t *pml4 = (uint64_t *)(read_cr3() & ADDR_MASK);
	uint64_t *pdpt, *pd;
	uint32_t i4 = (uint32_t)((addr >> 39) & 0x1FF);
	uint32_t i3 = (uint32_t)((addr >> 30) & 0x1FF);
	uint32_t i2 = (uint32_t)((addr >> 21) & 0x1FF);

	if (!(pml4[i4] & PTE_PRESENT))
		return 0;
	pdpt = (uint64_t *)(pml4[i4] & ADDR_MASK);
	if (!(pdpt[i3] & PTE_PRESENT))
		return 0;
	if (pdpt[i3] & PTE_PS)
		return pdpt[i3];
	pd = (uint64_t *)(pdpt[i3] & ADDR_MASK);
	if (!(pd[i2] & PTE_PRESENT))
		return 0;
	return pd[i2];
}

void *anx_mmio_map(uint64_t phys, uint64_t size)
{
	if (size == 0)
		return NULL;

	/* Guard against a wrapping range before deriving anything from it. */
	if (phys > 0xFFFFFFFFFFFFFFFFULL - (size - 1))
		return NULL;

	if (!map_cache_attr(phys, size, PTE_PCD | PTE_PWT))
		return NULL;

	flush_tlb();
	return (void *)(uintptr_t)phys;
}

/* ------------------------------------------------------------------ */
/* Write-combining for framebuffers                                     */
/* ------------------------------------------------------------------ */

/*
 * IA32_PAT maps the three cache bits in a page-table entry (PAT, PCD, PWT)
 * to one of eight memory types. The power-on table has no write-combining
 * entry at all, and Anunix never changed it, so the framebuffer took
 * whatever the firmware's MTRRs said -- uncached for a PCIe BAR -- and
 * every pixel was its own bus write. Boot text visibly scanned down the
 * screen because of it.
 *
 * Linux reprograms slot 1 (PWT set, PCD and PAT clear) to write-combining
 * on every x86 CPU it supports (arch/x86/mm/pat/memtype.c, v6.12). This
 * does the same, and changes nothing else: the firmware's other seven
 * slots are kept as found, so no existing mapping changes meaning.
 * Uncached device memory uses PCD|PWT, slot 3, which is untouched.
 */
#define MSR_IA32_PAT		0x277
#define PAT_TYPE_WC		0x01	/* MTRR_TYPE_WRCOMB, uapi/asm/mtrr.h */
#define CR0_NW			(1ULL << 29)
#define CR0_CD			(1ULL << 30)

static bool g_pat_wc;

static uint64_t rdmsr(uint32_t msr)
{
	uint32_t lo, hi;

	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
	return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t val)
{
	__asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val),
			 "d"((uint32_t)(val >> 32)) : "memory");
}

static uint64_t read_cr0(void)
{
	uint64_t v;

	__asm__ volatile("mov %%cr0, %0" : "=r"(v));
	return v;
}

static void write_cr0(uint64_t v)
{
	__asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

bool anx_pat_enable_wc(void)
{
	uint64_t old, want, cr0;

	if (g_pat_wc)
		return true;

	old = rdmsr(MSR_IA32_PAT);
	if (old == 0) {
		kprintf("pat: firmware left PAT disabled; no write-combining\n");
		return false;
	}

	want = (old & ~(0xffULL << 8)) | ((uint64_t)PAT_TYPE_WC << 8);
	if (want != old) {
		/*
		 * The SDM asks for caches to be disabled around a change to
		 * memory types, with the TLB flushed on both sides.
		 */
		cr0 = read_cr0();
		write_cr0((cr0 | CR0_CD) & ~CR0_NW);
		flush_tlb();
		wrmsr(MSR_IA32_PAT, want);
		flush_tlb();
		write_cr0(cr0);
	}

	g_pat_wc = rdmsr(MSR_IA32_PAT) == want;
	kprintf("pat: 0x%016llx -> 0x%016llx, slot 1 %s\n",
		(unsigned long long)old, (unsigned long long)want,
		g_pat_wc ? "is write-combining" : "did not take");
	return g_pat_wc;
}

void *anx_mmio_map_wc(uint64_t phys, uint64_t size)
{
	if (size == 0 || phys > 0xFFFFFFFFFFFFFFFFULL - (size - 1))
		return NULL;
	if (!g_pat_wc)
		return (void *)(uintptr_t)phys;	/* keep the existing map */
	if (!map_cache_attr(phys, size, PTE_PWT))
		return NULL;
	flush_tlb();
	return (void *)(uintptr_t)phys;
}
