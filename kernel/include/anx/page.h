/*
 * anx/page.h — Physical page allocator.
 */

#ifndef ANX_PAGE_H
#define ANX_PAGE_H

#include <anx/types.h>

#define ANX_PAGE_SIZE	4096
#define ANX_PAGE_SHIFT	12

/* Initialize the page allocator with one free memory region */
void anx_page_init(uintptr_t start, uintptr_t end);

/* Start an empty span [start, end); pages become usable only when added. */
void anx_page_init_span(uintptr_t start, uintptr_t end);

/* Mark RAM inside the span free. */
void anx_page_add_free(uintptr_t start, uintptr_t end);

/* Take never-allocated pages inside the span out of use. */
void anx_page_reserve(uintptr_t start, uintptr_t end);

/* Allocate 2^order contiguous pages. Returns physical address or 0 on failure */
uintptr_t anx_page_alloc(uint32_t order);

/* Free 2^order contiguous pages starting at addr */
void anx_page_free(uintptr_t addr, uint32_t order);

/* Query allocator statistics: usable pages and free pages */
void anx_page_stats(uint64_t *total, uint64_t *free_pages);

#endif /* ANX_PAGE_H */
