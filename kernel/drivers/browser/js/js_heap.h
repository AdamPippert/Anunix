/*
 * js_heap.h — GC heap: mark-and-sweep over a fixed arena.
 *
 * All heap-allocated objects (strings, JS objects, byte-code functions)
 * are allocated from a single flat arena.  GC is triggered when the
 * arena is close to full.  Roots are explicit (call-stack values +
 * registered permanent roots).
 *
 * Object header (8 bytes prepended to every heap allocation):
 *   type (1 byte): GC_TYPE_*
 *   flags (1 byte): GC_MARK | GC_PERM (permanent — never collected)
 *   size (2 bytes): total allocation size including header
 *   next (4 bytes): offset of next object header in arena (0 = end)
 */

#ifndef ANX_JS_HEAP_H
#define ANX_JS_HEAP_H

#include <anx/types.h>
#include "js_val.h"

#define GC_ARENA_SIZE    (512 * 1024)   /* 512 KB per JS context */
#define GC_MAX_ROOTS     512

#define GC_TYPE_FREE     0
#define GC_TYPE_STRING   1
#define GC_TYPE_OBJECT   2
#define GC_TYPE_FUNCTION 3
#define GC_TYPE_ARRAY    4
#define GC_TYPE_CLOSURE  5

#define GC_FLAG_MARK  0x01
#define GC_FLAG_PERM  0x02   /* permanent: not collected */

struct gc_hdr {
	uint8_t  type;
	uint8_t  flags;
	uint16_t size;   /* total bytes including this header */
	uint32_t next;   /* byte offset of next header from arena base; 0=end */
};

struct js_heap {
	uint8_t  arena[GC_ARENA_SIZE];
	uint32_t used;         /* bytes allocated so far */
	uint32_t n_objects;

	/* Roots registered for GC traversal */
	js_val  *roots[GC_MAX_ROOTS];
	uint32_t n_roots;
};

/* Allocate sz bytes (not including header).  Returns pointer to payload.
 * The header is filled in with the given type; flags and next are zeroed. */
void *js_heap_alloc(struct js_heap *h, uint8_t type, uint32_t sz);

/* Register/unregister a GC root pointer. */
void js_heap_root_add(struct js_heap *h, js_val *p);
void js_heap_root_remove(struct js_heap *h, js_val *p);

/* Run a mark-and-sweep collection.
 * mark_fn(v): called for every js_val reachable from roots; marks objects. */
void js_heap_gc(struct js_heap *h);

/* Mark one js_val and all objects reachable from it (for external call). */
void js_heap_mark_val(struct js_heap *h, js_val v);

/* Get the gc_hdr for a heap pointer. */
static inline struct gc_hdr *js_hdr(void *ptr)
{
	return (struct gc_hdr *)((uint8_t *)ptr - sizeof(struct gc_hdr));
}

#endif /* ANX_JS_HEAP_H */
