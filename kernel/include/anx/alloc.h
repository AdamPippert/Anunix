/*
 * anx/alloc.h — Kernel memory allocators.
 *
 * General-purpose allocator built on top of the page allocator,
 * plus pool and arena allocators for specialized use cases.
 */

#ifndef ANX_ALLOC_H
#define ANX_ALLOC_H

#include <anx/types.h>

/* General-purpose allocation */
void *anx_alloc(size_t size);
void *anx_zalloc(size_t size);
void  anx_free(void *ptr);

/* Pool allocator — fixed-size objects, O(1) alloc/free */
struct anx_pool;

struct anx_pool *anx_pool_create(size_t obj_size, uint32_t count);
void *anx_pool_alloc(struct anx_pool *pool);
void  anx_pool_free(struct anx_pool *pool, void *ptr);
void  anx_pool_destroy(struct anx_pool *pool);

/* Arena allocator — bump pointer, bulk free only */
struct anx_arena;

struct anx_arena *anx_arena_create(size_t size);
void *anx_arena_alloc(struct anx_arena *arena, size_t size);
void  anx_arena_reset(struct anx_arena *arena);
void  anx_arena_destroy(struct anx_arena *arena);

#endif /* ANX_ALLOC_H */
