/*
 * js_obj.c — JS object implementation.
 */

#include "js_obj.h"
#include <anx/string.h>

/* ── GC traversal ──────────────────────────────────────────────────── */

void js_obj_gc_mark(struct js_heap *h, struct js_obj *o)
{
	uint32_t i;
	js_heap_mark_val(h, o->proto);
	for (i = 0; i < o->n_props; i++) {
		js_heap_mark_val(h, o->props[i].key);
		js_heap_mark_val(h, o->props[i].value);
	}
}

/* ── Allocation ────────────────────────────────────────────────────── */

struct js_obj *js_obj_new(struct js_heap *h, js_val proto)
{
	struct js_obj *o = (struct js_obj *)
		js_heap_alloc(h, GC_TYPE_OBJECT, sizeof(struct js_obj));
	if (!o) return NULL;
	o->proto      = proto;
	o->n_props    = 0;
	o->array_len  = 0;
	o->is_array   = 0;
	o->is_function = 0;
	return o;
}

struct js_obj *js_arr_new(struct js_heap *h)
{
	struct js_obj *o = js_obj_new(h, JV_NULL);
	if (o) o->is_array = 1;
	return o;
}

/* ── Property lookup helpers ───────────────────────────────────────── */

static int find_prop(const struct js_obj *o, const struct js_str *key)
{
	uint32_t i;
	for (i = 0; i < o->n_props; i++) {
		const struct js_str *k = jv_to_str(o->props[i].key);
		if (k->hash == key->hash && js_str_eq(k, key))
			return (int)i;
	}
	return -1;
}

/* ── Get ───────────────────────────────────────────────────────────── */

js_val js_obj_get_own(const struct js_obj *o, const struct js_str *key)
{
	int idx = find_prop(o, key);
	return idx >= 0 ? o->props[idx].value : JV_UNDEF;
}

js_val js_obj_get(struct js_heap *h, const struct js_obj *o,
                  const struct js_str *key)
{
	int depth = 0;
	const struct js_obj *cur = o;
	while (cur && depth < 16) {
		int idx = find_prop(cur, key);
		if (idx >= 0)
			return cur->props[idx].value;
		if (jv_is_null(cur->proto) || !jv_is_obj(cur->proto))
			break;
		cur = (const struct js_obj *)jv_to_ptr(cur->proto);
		depth++;
	}
	(void)h;
	return JV_UNDEF;
}

js_val js_obj_get_cstr(struct js_heap *h, const struct js_obj *o,
                       const char *ckey)
{
	/* Build a temporary js_str on the stack — we only need hash+len for
	 * the equality check; data is compared via js_str_eq which reads
	 * js_str_data() but our stack copy has data right after the header. */
	uint32_t len = 0;
	while (ckey[len]) len++;
	/* Stack-allocate: header + data + NUL */
	uint8_t buf[sizeof(struct js_str) + 64 + 1];
	struct js_str *tmp;
	if (len <= 64) {
		tmp = (struct js_str *)buf;
		tmp->len  = len;
		tmp->hash = js_str_hash_buf(ckey, len);
		anx_memcpy((uint8_t *)tmp + sizeof(struct js_str), ckey, len + 1);
		return js_obj_get(h, o, tmp);
	}
	/* Longer keys: allocate on heap */
	tmp = js_str_new(h, ckey, len);
	if (!tmp) return JV_UNDEF;
	return js_obj_get(h, o, tmp);
}

/* ── Set ───────────────────────────────────────────────────────────── */

bool js_obj_set(struct js_obj *o, const struct js_str *key, js_val val)
{
	int idx = find_prop(o, key);
	if (idx >= 0) {
		o->props[idx].value = val;
		return true;
	}
	if (o->n_props >= JS_OBJ_MAX_PROPS) return false;
	uint32_t i = o->n_props++;
	o->props[i].key          = jv_str(key);
	o->props[i].value        = val;
	o->props[i].writable     = 1;
	o->props[i].enumerable   = 1;
	o->props[i].configurable = 1;
	return true;
}

bool js_obj_set_cstr(struct js_heap *h, struct js_obj *o,
                     const char *ckey, js_val val)
{
	uint32_t len = 0;
	while (ckey[len]) len++;

	/* Fast path: key exists — no need to allocate a new string */
	uint8_t buf[sizeof(struct js_str) + 64 + 1];
	struct js_str *tmp = NULL;
	if (len <= 64) {
		tmp = (struct js_str *)buf;
		tmp->len  = len;
		tmp->hash = js_str_hash_buf(ckey, len);
		anx_memcpy((uint8_t *)tmp + sizeof(struct js_str), ckey, len + 1);
		int idx = find_prop(o, tmp);
		if (idx >= 0) {
			o->props[idx].value = val;
			return true;
		}
	}

	/* Need to add a new slot — allocate a persistent heap string */
	struct js_str *ks = js_str_new(h, ckey, len);
	if (!ks) return false;
	return js_obj_set(o, ks, val);
}

/* ── Delete ────────────────────────────────────────────────────────── */

bool js_obj_delete(struct js_obj *o, const struct js_str *key)
{
	int idx = find_prop(o, key);
	if (idx < 0) return false;
	uint32_t last = o->n_props - 1;
	if ((uint32_t)idx != last)
		o->props[idx] = o->props[last];
	o->n_props--;
	return true;
}

/* ── Has own ───────────────────────────────────────────────────────── */

bool js_obj_has_own(const struct js_obj *o, const struct js_str *key)
{
	return find_prop(o, key) >= 0;
}
