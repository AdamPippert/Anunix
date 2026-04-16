/*
 * page.c — Bitmap-based physical page allocator.
 *
 * Simple and correct. Each bit in the bitmap represents one physical
 * page. 1 = free, 0 = allocated. Order-N allocations find N contiguous
 * free pages by scanning the bitmap.
 */

#include <anx/types.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/panic.h>

#define MAX_PAGES	(1 << 16)	/* 64K pages = 256 MiB max */
#define BITMAP_WORDS	(MAX_PAGES / 64)

static uint64_t bitmap[BITMAP_WORDS];
static uintptr_t heap_base;
static uint64_t total_pages;
static uint64_t free_count;

static void bit_set(uint64_t page)
{
	bitmap[page / 64] |= (1ULL << (page % 64));
}

static void bit_clear(uint64_t page)
{
	bitmap[page / 64] &= ~(1ULL << (page % 64));
}

static bool bit_test(uint64_t page)
{
	return (bitmap[page / 64] >> (page % 64)) & 1;
}

void anx_page_init(uintptr_t start, uintptr_t end)
{
	/* Align start up and end down to page boundaries */
	start = (start + ANX_PAGE_SIZE - 1) & ~((uintptr_t)ANX_PAGE_SIZE - 1);
	end = end & ~((uintptr_t)ANX_PAGE_SIZE - 1);

	ANX_ASSERT(end > start);

	heap_base = start;
	total_pages = (end - start) >> ANX_PAGE_SHIFT;
	if (total_pages > MAX_PAGES)
		total_pages = MAX_PAGES;

	/* Mark all pages free */
	anx_memset(bitmap, 0, sizeof(bitmap));
	for (uint64_t i = 0; i < total_pages; i++)
		bit_set(i);

	free_count = total_pages;
}

uintptr_t anx_page_alloc(uint32_t order)
{
	uint64_t count = 1ULL << order;

	if (count > free_count)
		return 0;

	/*
	 * For large allocations (>= 16 pages), search from the END
	 * of the heap to avoid fragmenting the low region used by
	 * small slab allocations. For small allocations, search
	 * from the start.
	 */
	if (count >= 16) {
		/* Search backwards from end */
		for (uint64_t i = total_pages - count; i < total_pages; i--) {
			bool found = true;

			for (uint64_t j = 0; j < count; j++) {
				if (!bit_test(i + j)) {
					found = false;
					break;
				}
			}

			if (found) {
				for (uint64_t j = 0; j < count; j++)
					bit_clear(i + j);
				free_count -= count;
				return heap_base + (i << ANX_PAGE_SHIFT);
			}

			if (i == 0)
				break;
		}
	} else {
		/* Small: scan forward from start */
		for (uint64_t i = 0; i + count <= total_pages; i++) {
			bool found = true;

			for (uint64_t j = 0; j < count; j++) {
				if (!bit_test(i + j)) {
					found = false;
					i += j;
					break;
				}
			}

			if (found) {
				for (uint64_t j = 0; j < count; j++)
					bit_clear(i + j);
				free_count -= count;
				return heap_base + (i << ANX_PAGE_SHIFT);
			}
		}
	}

	return 0; /* out of memory */
}

void anx_page_free(uintptr_t addr, uint32_t order)
{
	ANX_ASSERT(addr >= heap_base);
	ANX_ASSERT((addr & (ANX_PAGE_SIZE - 1)) == 0);

	uint64_t page = (addr - heap_base) >> ANX_PAGE_SHIFT;
	uint64_t count = 1ULL << order;

	ANX_ASSERT(page + count <= total_pages);

	for (uint64_t j = 0; j < count; j++) {
		ANX_ASSERT(!bit_test(page + j)); /* double-free check */
		bit_set(page + j);
	}

	free_count += count;
}

void anx_page_stats(uint64_t *total, uint64_t *free_pages)
{
	if (total)
		*total = total_pages;
	if (free_pages)
		*free_pages = free_count;
}
