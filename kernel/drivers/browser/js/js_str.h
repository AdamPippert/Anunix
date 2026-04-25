/*
 * js_str.h — heap-allocated JS string with FNV-1a hash.
 *
 * Layout in GC arena (type = GC_TYPE_STRING):
 *   struct js_str  (header: len + hash)
 *   uint8_t data[len+1]  (UTF-8, NUL-terminated)
 *
 * Strings are immutable once created.  The hash is computed on creation
 * and used for property key lookup in js_obj.
 */

#ifndef ANX_JS_STR_H
#define ANX_JS_STR_H

#include <anx/types.h>
#include "js_heap.h"
#include "js_val.h"

struct js_str {
	uint32_t len;    /* byte length, not including NUL */
	uint32_t hash;   /* FNV-1a over data bytes */
	/* uint8_t data[] follows immediately */
};

/* Pointer to the character data of a js_str. */
static inline const char *js_str_data(const struct js_str *s)
{
	return (const char *)((const uint8_t *)s + sizeof(struct js_str));
}

/* Compute FNV-1a hash for a byte buffer. */
static inline uint32_t js_str_hash_buf(const char *p, uint32_t len)
{
	uint32_t h = 2166136261u;
	uint32_t i;
	for (i = 0; i < len; i++)
		h = (h ^ (uint8_t)p[i]) * 16777619u;
	return h;
}

/*
 * Allocate a new JS string by copying src[0..len-1].
 * Returns NULL on OOM.  The returned pointer is the js_str payload;
 * wrap in jv_str() to get a js_val.
 */
struct js_str *js_str_new(struct js_heap *h, const char *src, uint32_t len);

/* Allocate from a NUL-terminated C string. */
struct js_str *js_str_from_cstr(struct js_heap *h, const char *s);

/* Concatenate two strings.  Returns NULL on OOM. */
struct js_str *js_str_concat(struct js_heap *h,
                             const struct js_str *a,
                             const struct js_str *b);

/* Substring [start, start+len).  Clamps to string bounds. */
struct js_str *js_str_slice(struct js_heap *h,
                            const struct js_str *s,
                            uint32_t start, uint32_t len);

/* Equality check. */
static inline bool js_str_eq(const struct js_str *a, const struct js_str *b)
{
	if (a->len != b->len || a->hash != b->hash) return false;
	/* anx_memcmp not always available; inline byte compare */
	const uint8_t *p = (const uint8_t *)js_str_data(a);
	const uint8_t *q = (const uint8_t *)js_str_data(b);
	uint32_t i;
	for (i = 0; i < a->len; i++)
		if (p[i] != q[i]) return false;
	return true;
}

/* Compare a js_str against a C string literal. */
static inline bool js_str_eq_cstr(const struct js_str *s, const char *c)
{
	const char *d = js_str_data(s);
	uint32_t i;
	for (i = 0; i < s->len; i++)
		if (d[i] != c[i] || c[i] == '\0') return false;
	return c[s->len] == '\0';
}

/* Convert js_val → js_str * (no type check). */
static inline struct js_str *jv_to_str(js_val v)
{
	return (struct js_str *)jv_to_ptr(v);
}

#endif /* ANX_JS_STR_H */
