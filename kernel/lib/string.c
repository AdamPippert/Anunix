/*
 * string.c — Kernel string and memory functions.
 *
 * Simple byte-at-a-time implementations. Correctness first;
 * optimize with word-wide or SIMD operations later if profiling
 * shows these are hot paths.
 */

#include <anx/string.h>

void *anx_memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *anx_memset(void *dst, int val, size_t n)
{
	uint8_t *d = dst;
	uint8_t v = (uint8_t)val;

	while (n--)
		*d++ = v;
	return dst;
}

int anx_memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *pa = a;
	const uint8_t *pb = b;

	while (n--) {
		if (*pa != *pb)
			return *pa < *pb ? -1 : 1;
		pa++;
		pb++;
	}
	return 0;
}

void *anx_memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else if (d > s) {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

/*
 * Bare-name aliases for compiler-generated calls (struct assignment, etc.).
 * Only needed in freestanding kernel builds — host-native test builds
 * get these from libc.
 */
#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__ == 0
void *memcpy(void *dst, const void *src, size_t n)
{
	return anx_memcpy(dst, src, n);
}

void *memset(void *dst, int val, size_t n)
{
	return anx_memset(dst, val, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
	return anx_memmove(dst, src, n);
}
#endif

size_t anx_strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return p - s;
}

int anx_strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int anx_strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) {
		a++;
		b++;
		n--;
	}
	if (n == 0)
		return 0;
	return (unsigned char)*a - (unsigned char)*b;
}

size_t anx_strlcpy(char *dst, const char *src, size_t dstsize)
{
	size_t srclen = anx_strlen(src);

	if (dstsize > 0) {
		size_t copylen = srclen < dstsize - 1 ? srclen : dstsize - 1;
		anx_memcpy(dst, src, copylen);
		dst[copylen] = '\0';
	}
	return srclen;
}
