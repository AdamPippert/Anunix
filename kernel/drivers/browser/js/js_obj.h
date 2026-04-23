/*
 * js_obj.h — JS object: flat property table with prototype chain.
 *
 * Each property slot stores a key (js_val string) and a value (js_val).
 * Up to JS_OBJ_MAX_PROPS properties per object.  Overflow is silently
 * dropped — good enough for a DOM engine.
 *
 * Prototype chain: proto == JV_NULL means no prototype.
 */

#ifndef ANX_JS_OBJ_H
#define ANX_JS_OBJ_H

#include <anx/types.h>
#include "js_heap.h"
#include "js_val.h"
#include "js_str.h"

#define JS_OBJ_MAX_PROPS  32

struct js_prop {
	js_val key;    /* always a JV_TAG_STR */
	js_val value;
	uint8_t writable;
	uint8_t enumerable;
	uint8_t configurable;
	uint8_t _pad;
};

struct js_obj {
	js_val proto;                          /* JV_NULL or object js_val */
	uint32_t n_props;
	struct js_prop props[JS_OBJ_MAX_PROPS];

	/* For Array objects: length tracked separately from props. */
	uint32_t array_len;
	uint8_t  is_array;
	uint8_t  is_function;
	uint8_t  _pad[2];
};

/* GC traversal — marks all reachable values in obj. */
void js_obj_gc_mark(struct js_heap *h, struct js_obj *o);

/* Allocate a new empty object with given prototype.
 * proto = JV_NULL for Object.prototype, pass JV_NULL explicitly. */
struct js_obj *js_obj_new(struct js_heap *h, js_val proto);

/* Allocate a new empty Array object. */
struct js_obj *js_arr_new(struct js_heap *h);

/* Get own property.  Returns JV_UNDEF if not found. */
js_val js_obj_get_own(const struct js_obj *o, const struct js_str *key);

/* Get property, walking prototype chain up to 16 levels.
 * heap needed to allocate synthetic array length string, etc. */
js_val js_obj_get(struct js_heap *h, const struct js_obj *o,
                  const struct js_str *key);

/* Get property by C string key (convenience). */
js_val js_obj_get_cstr(struct js_heap *h, const struct js_obj *o,
                       const char *key);

/* Set own property (writable/enumerable/configurable = true).
 * Returns false if property table is full. */
bool js_obj_set(struct js_obj *o, const struct js_str *key, js_val val);

/* Set property by C string — key must be internable without heap alloc
 * when key already exists.  Creates a new js_str on heap for new keys. */
bool js_obj_set_cstr(struct js_heap *h, struct js_obj *o,
                     const char *key, js_val val);

/* Delete own property. Returns true if found and removed. */
bool js_obj_delete(struct js_obj *o, const struct js_str *key);

/* Check own property existence. */
bool js_obj_has_own(const struct js_obj *o, const struct js_str *key);

#endif /* ANX_JS_OBJ_H */
