/*
 * js_str.c — heap-allocated JS string.
 */

#include "js_str.h"
#include <anx/string.h>

struct js_str *js_str_new(struct js_heap *h, const char *src, uint32_t len)
{
	uint32_t payload = sizeof(struct js_str) + len + 1;
	struct js_str *s = (struct js_str *)js_heap_alloc(h, GC_TYPE_STRING, payload);
	if (!s) return NULL;
	s->len  = len;
	s->hash = js_str_hash_buf(src, len);
	char *dst = (char *)((uint8_t *)s + sizeof(struct js_str));
	if (len) anx_memcpy(dst, src, len);
	dst[len] = '\0';
	return s;
}

struct js_str *js_str_from_cstr(struct js_heap *h, const char *src)
{
	uint32_t len = 0;
	while (src[len]) len++;
	return js_str_new(h, src, len);
}

struct js_str *js_str_concat(struct js_heap *h,
                             const struct js_str *a,
                             const struct js_str *b)
{
	uint32_t len = a->len + b->len;
	uint32_t payload = sizeof(struct js_str) + len + 1;
	struct js_str *s = (struct js_str *)js_heap_alloc(h, GC_TYPE_STRING, payload);
	if (!s) return NULL;
	s->len  = len;
	char *dst = (char *)((uint8_t *)s + sizeof(struct js_str));
	if (a->len) anx_memcpy(dst, js_str_data(a), a->len);
	if (b->len) anx_memcpy(dst + a->len, js_str_data(b), b->len);
	dst[len] = '\0';
	s->hash = js_str_hash_buf(dst, len);
	return s;
}

struct js_str *js_str_slice(struct js_heap *h,
                            const struct js_str *src,
                            uint32_t start, uint32_t len)
{
	if (start > src->len) start = src->len;
	if (start + len > src->len) len = src->len - start;
	return js_str_new(h, js_str_data(src) + start, len);
}
