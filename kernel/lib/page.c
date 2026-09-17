/*
 * page.c — Bitmap-based physical page allocator.
 *
 * Each bit represents one physical page of a span starting at heap_base:
 * 1 = free, 0 = allocated or not RAM. A span may contain holes -- firmware
 * reservations, MMIO, the user exec window -- which simply never become
 * free. Order-N allocations find N contiguous free pages by scanning.
 */

#include <anx/types.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/panic.h>

#define MAX_PAGES	(1ULL << 20)	/* 4 GiB of span */
#define BITMAP_WORDS	(MAX_PAGES / 64)

static uint64_t bitmap[BITMAP_WORDS];
static uintptr_t heap_base;
static uint64_t total_pages;	/* span length in pages */
static uint64_t usable_pages;	/* pages ever marked free */
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

void anx_page_init_span(uintptr_t start, uintptr_t end)
{
	start = (start + ANX_PAGE_SIZE - 1) & ~((uintptr_t)ANX_PAGE_SIZE - 1);
	end = end & ~((uintptr_t)ANX_PAGE_SIZE - 1);

	ANX_ASSERT(end > start);

	heap_base = start;
	total_pages = (end - start) >> ANX_PAGE_SHIFT;
	if (total_pages > MAX_PAGES)
		total_pages = MAX_PAGES;

	anx_memset(bitmap, 0, sizeof(bitmap));
	usable_pages = 0;
	free_count = 0;
}

/* Clip [start, end) to the span and return it in pages; false if empty. */
static bool clip(uintptr_t start, uintptr_t end, uint64_t *first,
		 uint64_t *last)
{
	uintptr_t span_end = heap_base + (total_pages << ANX_PAGE_SHIFT);

	start = (start + ANX_PAGE_SIZE - 1) & ~((uintptr_t)ANX_PAGE_SIZE - 1);
	end = end & ~((uintptr_t)ANX_PAGE_SIZE - 1);
	if (start < heap_base)
		start = heap_base;
	if (end > span_end)
		end = span_end;
	if (end <= start)
		return false;
	*first = (start - heap_base) >> ANX_PAGE_SHIFT;
	*last = (end - heap_base) >> ANX_PAGE_SHIFT;
	return true;
}

void anx_page_add_free(uintptr_t start, uintptr_t end)
{
	uint64_t first, last, p;

	if (!clip(start, end, &first, &last))
		return;
	for (p = first; p < last; p++) {
		if (bit_test(p))
			continue;
		bit_set(p);
		usable_pages++;
		free_count++;
	}
}

void anx_page_reserve(uintptr_t start, uintptr_t end)
{
	uint64_t first, last, p;

	if (!clip(start, end, &first, &last))
		return;
	for (p = first; p < last; p++) {
		if (!bit_test(p))
			continue;
		bit_clear(p);
		usable_pages--;
		free_count--;
	}
}

void anx_page_init(uintptr_t start, uintptr_t end)
{
	anx_page_init_span(start, end);
	anx_page_add_free(start, end);
}

static uintptr_t take(uint64_t first, uint64_t count)
{
	uint64_t j;

	for (j = 0; j < count; j++)
		bit_clear(first + j);
	free_count -= count;
	return heap_base + (first << ANX_PAGE_SHIFT);
}

uintptr_t anx_page_alloc(uint32_t order)
{
	uint64_t count = 1ULL << order;
	uint64_t i, j;

	if (count > free_count || count > total_pages)
		return 0;

	/*
	 * For large allocations (>= 16 pages), search from the END
	 * of the heap to avoid fragmenting the low region used by
	 * small slab allocations. For small allocations, search
	 * from the start. Whole words of allocated pages -- holes in
	 * the span -- are skipped 64 at a time.
	 */
	if (count >= 16) {
		i = total_pages - count;
		for (;;) {
			if (bitmap[i / 64] == 0) {
				uint64_t w = i / 64;

				if (w == 0)
					break;
				i = w * 64 - 1;
				if (i + count > total_pages)
					i = total_pages - count;
				continue;
			}
			for (j = 0; j < count; j++)
				if (!bit_test(i + j))
					break;
			if (j == count)
				return take(i, count);
			if (i == 0)
				break;
			i--;
		}
	} else {
		for (i = 0; i + count <= total_pages;) {
			if (bitmap[i / 64] == 0) {
				i = (i / 64 + 1) * 64;
				continue;
			}
			for (j = 0; j < count; j++)
				if (!bit_test(i + j))
					break;
			if (j == count)
				return take(i, count);
			i += j + 1;
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
		*total = usable_pages;
	if (free_pages)
		*free_pages = free_count;
}
