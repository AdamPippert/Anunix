/*
 * js_heap.c — GC heap: bump allocator + mark-and-sweep.
 */

#include "js_heap.h"
#include "js_val.h"
#include <anx/string.h>
#include <anx/kprintf.h>

/* ── Allocation ─────────────────────────────────────────────────── */

void *js_heap_alloc(struct js_heap *h, uint8_t type, uint32_t sz)
{
	uint32_t total = sizeof(struct gc_hdr) + sz;
	/* Align to 8 bytes */
	total = (total + 7u) & ~7u;

	if (h->used + total > GC_ARENA_SIZE) {
		/* Try a GC pass first */
		js_heap_gc(h);
		if (h->used + total > GC_ARENA_SIZE) {
			kprintf("js_heap: OOM (need %u, used %u / %u)\n",
				total, h->used, (uint32_t)GC_ARENA_SIZE);
			return NULL;
		}
	}

	struct gc_hdr *hdr = (struct gc_hdr *)(h->arena + h->used);
	hdr->type  = type;
	hdr->flags = 0;
	hdr->size  = (uint16_t)(total > 0xFFFF ? 0xFFFF : total);
	hdr->next  = 0;

	/* Link previous object's next pointer */
	if (h->used > 0) {
		/* Walk chain to find last object */
		struct gc_hdr *cur = (struct gc_hdr *)h->arena;
		while (cur->next != 0)
			cur = (struct gc_hdr *)(h->arena + cur->next);
		if ((uint8_t *)cur != h->arena + h->used)
			cur->next = h->used;
	}

	h->used += total;
	h->n_objects++;
	anx_memset((uint8_t *)hdr + sizeof(struct gc_hdr), 0, sz);
	return (uint8_t *)hdr + sizeof(struct gc_hdr);
}

/* ── Root management ────────────────────────────────────────────── */

void js_heap_root_add(struct js_heap *h, js_val *p)
{
	if (h->n_roots < GC_MAX_ROOTS)
		h->roots[h->n_roots++] = p;
}

void js_heap_root_remove(struct js_heap *h, js_val *p)
{
	uint32_t i;
	for (i = 0; i < h->n_roots; i++) {
		if (h->roots[i] == p) {
			h->roots[i] = h->roots[--h->n_roots];
			return;
		}
	}
}

/* ── GC ─────────────────────────────────────────────────────────── */

static void mark_val(struct js_heap *h, js_val v);

/* Forward declarations for object/string traversal */
struct js_obj;
struct js_str;
void js_obj_gc_mark(struct js_heap *h, struct js_obj *o);

void js_heap_mark_val(struct js_heap *h, js_val v)
{
	if (jv_is_obj(v) || jv_is_str(v)) {
		void *ptr = jv_to_ptr(v);
		if (!ptr) return;
		struct gc_hdr *hdr = js_hdr(ptr);
		if (hdr->flags & GC_FLAG_MARK) return;  /* already marked */
		hdr->flags |= GC_FLAG_MARK;
		if (jv_is_obj(v))
			js_obj_gc_mark(h, (struct js_obj *)ptr);
	}
}

static void mark_val(struct js_heap *h, js_val v)
{
	js_heap_mark_val(h, v);
}

void js_heap_gc(struct js_heap *h)
{
	uint32_t i;

	/* Clear all marks */
	uint32_t off = 0;
	while (off < h->used) {
		struct gc_hdr *hdr = (struct gc_hdr *)(h->arena + off);
		if (hdr->type != GC_TYPE_FREE)
			hdr->flags &= (uint8_t)~GC_FLAG_MARK;
		off += hdr->size ? hdr->size : 8;
	}

	/* Mark from roots */
	for (i = 0; i < h->n_roots; i++) {
		if (h->roots[i])
			mark_val(h, *h->roots[i]);
	}

	/* Sweep: compact live objects */
	uint32_t new_used = 0;
	uint8_t  new_arena[GC_ARENA_SIZE];
	uint32_t new_n = 0;

	off = 0;
	while (off < h->used) {
		struct gc_hdr *hdr = (struct gc_hdr *)(h->arena + off);
		uint32_t sz = hdr->size ? hdr->size : 8;
		if (hdr->type != GC_TYPE_FREE &&
		    ((hdr->flags & (GC_FLAG_MARK | GC_FLAG_PERM)))) {
			anx_memcpy(new_arena + new_used, hdr, sz);
			new_used += sz;
			new_n++;
		}
		off += sz;
	}

	anx_memcpy(h->arena, new_arena, new_used);
	h->used      = new_used;
	h->n_objects = new_n;

	/* Rebuild next-pointer chain */
	off = 0;
	while (off < h->used) {
		struct gc_hdr *hdr = (struct gc_hdr *)(h->arena + off);
		uint32_t sz = hdr->size ? hdr->size : 8;
		uint32_t next_off = off + sz;
		hdr->next = (next_off < h->used) ? next_off : 0;
		off = next_off;
	}

	kprintf("js_heap: GC freed %u objects, %u bytes used\n",
		h->n_objects, h->used);
}
