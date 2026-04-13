/*
 * alloc.c — Kernel memory allocators.
 *
 * General-purpose: power-of-two size classes with per-class free lists,
 * backed by the page allocator. Each allocation is prefixed with a
 * header recording its size class.
 *
 * Pool: fixed-size object pool using a free list embedded in free slots.
 *
 * Arena: bump-pointer allocator with bulk reset.
 */

#include <anx/types.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/panic.h>

/* --- General-purpose allocator --- */

#define ALLOC_MAGIC	0xA11C0000
#define MIN_CLASS	5	/* 32 bytes */
#define MAX_CLASS	12	/* 4096 bytes */
#define NUM_CLASSES	(MAX_CLASS - MIN_CLASS + 1)

struct alloc_header {
	uint32_t magic;
	uint32_t class;		/* size class index */
};

/* Free list per size class */
struct free_node {
	struct free_node *next;
};

static struct free_node *freelists[NUM_CLASSES];

static uint32_t size_to_class(size_t size)
{
	/* Add header size */
	size += sizeof(struct alloc_header);

	uint32_t cls = MIN_CLASS;
	while (cls < MAX_CLASS && ((size_t)1 << cls) < size)
		cls++;
	return cls;
}

static void slab_refill(uint32_t cls)
{
	size_t obj_size = (size_t)1 << cls;
	uintptr_t page = anx_page_alloc(0); /* one page */

	if (!page)
		return;

	/* Carve the page into objects of this size class */
	size_t count = ANX_PAGE_SIZE / obj_size;

	for (size_t i = 0; i < count; i++) {
		struct free_node *node = (struct free_node *)(page + i * obj_size);
		node->next = freelists[cls - MIN_CLASS];
		freelists[cls - MIN_CLASS] = node;
	}
}

void *anx_alloc(size_t size)
{
	if (size == 0)
		return NULL;

	/* Large allocations go directly to page allocator */
	if (size + sizeof(struct alloc_header) > ANX_PAGE_SIZE) {
		uint32_t pages_needed = (size + sizeof(struct alloc_header) +
					 ANX_PAGE_SIZE - 1) >> ANX_PAGE_SHIFT;
		uint32_t order = 0;
		while ((1U << order) < pages_needed)
			order++;

		uintptr_t addr = anx_page_alloc(order);
		if (!addr)
			return NULL;

		struct alloc_header *hdr = (struct alloc_header *)addr;
		hdr->magic = ALLOC_MAGIC;
		hdr->class = MAX_CLASS + order + 1; /* mark as large */
		return hdr + 1;
	}

	uint32_t cls = size_to_class(size);
	uint32_t idx = cls - MIN_CLASS;

	if (!freelists[idx])
		slab_refill(cls);

	if (!freelists[idx])
		return NULL; /* out of memory */

	struct free_node *node = freelists[idx];
	freelists[idx] = node->next;

	struct alloc_header *hdr = (struct alloc_header *)node;
	hdr->magic = ALLOC_MAGIC;
	hdr->class = cls;
	return hdr + 1;
}

void *anx_zalloc(size_t size)
{
	void *ptr = anx_alloc(size);

	if (ptr)
		anx_memset(ptr, 0, size);
	return ptr;
}

void anx_free(void *ptr)
{
	if (!ptr)
		return;

	struct alloc_header *hdr = (struct alloc_header *)ptr - 1;
	ANX_ASSERT((hdr->magic & 0xFFFF0000) == ALLOC_MAGIC);

	uint32_t cls = hdr->class;

	/* Large allocation */
	if (cls > MAX_CLASS) {
		uint32_t order = cls - MAX_CLASS - 1;
		anx_page_free((uintptr_t)hdr, order);
		return;
	}

	/* Return to free list */
	uint32_t idx = cls - MIN_CLASS;
	struct free_node *node = (struct free_node *)hdr;
	node->next = freelists[idx];
	freelists[idx] = node;
}

/* --- Pool allocator --- */

struct anx_pool {
	void *base;		/* backing memory */
	struct free_node *free_list;
	size_t obj_size;
	uint32_t capacity;
	uint32_t alloc_pages;	/* pages allocated */
};

struct anx_pool *anx_pool_create(size_t obj_size, uint32_t count)
{
	/* Minimum object size is pointer-sized for the free list */
	if (obj_size < sizeof(struct free_node))
		obj_size = sizeof(struct free_node);

	size_t total = obj_size * count;
	uint32_t pages = (total + ANX_PAGE_SIZE - 1) >> ANX_PAGE_SHIFT;
	uint32_t order = 0;
	while ((1U << order) < pages)
		order++;

	uintptr_t mem = anx_page_alloc(order);
	if (!mem)
		return NULL;

	/* Allocate the pool struct itself */
	struct anx_pool *pool = anx_alloc(sizeof(struct anx_pool));
	if (!pool) {
		anx_page_free(mem, order);
		return NULL;
	}

	pool->base = (void *)mem;
	pool->obj_size = obj_size;
	pool->capacity = count;
	pool->alloc_pages = 1U << order;
	pool->free_list = NULL;

	/* Build free list */
	for (uint32_t i = 0; i < count; i++) {
		struct free_node *node = (struct free_node *)(mem + i * obj_size);
		node->next = pool->free_list;
		pool->free_list = node;
	}

	return pool;
}

void *anx_pool_alloc(struct anx_pool *pool)
{
	if (!pool || !pool->free_list)
		return NULL;

	struct free_node *node = pool->free_list;
	pool->free_list = node->next;
	return node;
}

void anx_pool_free(struct anx_pool *pool, void *ptr)
{
	if (!pool || !ptr)
		return;

	struct free_node *node = ptr;
	node->next = pool->free_list;
	pool->free_list = node;
}

void anx_pool_destroy(struct anx_pool *pool)
{
	if (!pool)
		return;

	/* Calculate order from alloc_pages */
	uint32_t order = 0;
	uint32_t p = pool->alloc_pages;
	while (p > 1) {
		order++;
		p >>= 1;
	}

	anx_page_free((uintptr_t)pool->base, order);
	anx_free(pool);
}

/* --- Arena allocator --- */

struct anx_arena {
	void *base;
	size_t size;
	size_t used;
	uint32_t alloc_pages;
};

struct anx_arena *anx_arena_create(size_t size)
{
	uint32_t pages = (size + ANX_PAGE_SIZE - 1) >> ANX_PAGE_SHIFT;
	uint32_t order = 0;
	while ((1U << order) < pages)
		order++;

	uintptr_t mem = anx_page_alloc(order);
	if (!mem)
		return NULL;

	struct anx_arena *arena = anx_alloc(sizeof(struct anx_arena));
	if (!arena) {
		anx_page_free(mem, order);
		return NULL;
	}

	arena->base = (void *)mem;
	arena->size = (size_t)(1U << order) * ANX_PAGE_SIZE;
	arena->used = 0;
	arena->alloc_pages = 1U << order;

	return arena;
}

void *anx_arena_alloc(struct anx_arena *arena, size_t size)
{
	if (!arena)
		return NULL;

	/* Align to 8 bytes */
	size = (size + 7) & ~(size_t)7;

	if (arena->used + size > arena->size)
		return NULL;

	void *ptr = (uint8_t *)arena->base + arena->used;
	arena->used += size;
	return ptr;
}

void anx_arena_reset(struct anx_arena *arena)
{
	if (arena)
		arena->used = 0;
}

void anx_arena_destroy(struct anx_arena *arena)
{
	if (!arena)
		return;

	uint32_t order = 0;
	uint32_t p = arena->alloc_pages;
	while (p > 1) {
		order++;
		p >>= 1;
	}

	anx_page_free((uintptr_t)arena->base, order);
	anx_free(arena);
}
