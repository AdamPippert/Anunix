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

uint64_t anx_strtoull(const char *s, char **endp, int base)
{
	uint64_t val = 0;
	const char *p = s;

	while (*p == ' ' || *p == '\t')
		p++;
	if (base == 0 || base == 16) {
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
			base = 16;
			p += 2;
		} else if (base == 0) {
			base = (p[0] == '0') ? 8 : 10;
		}
	}
	for (; *p; p++) {
		int d;
		if (*p >= '0' && *p <= '9')       d = *p - '0';
		else if (*p >= 'a' && *p <= 'f')   d = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F')   d = *p - 'A' + 10;
		else break;
		if (d >= base) break;
		val = val * (uint64_t)base + (uint64_t)d;
	}
	if (endp)
		*endp = (char *)p;
	return val;
}

uint32_t anx_strtoul(const char *s, char **endp, int base)
{
	return (uint32_t)anx_strtoull(s, endp, base);
}

/* Write decimal representation of val into buf[size]. Returns chars written. */
static uint32_t fmt_uint64(char *buf, uint32_t size, uint64_t val)
{
	char tmp[20];
	uint32_t n = 0, i;

	if (size == 0)
		return 0;
	if (val == 0) {
		tmp[n++] = '0';
	} else {
		while (val > 0 && n < sizeof(tmp)) {
			tmp[n++] = (char)('0' + val % 10);
			val /= 10;
		}
	}
	for (i = 0; i < n && i < size - 1; i++)
		buf[i] = tmp[n - 1 - i];
	buf[i] = '\0';
	return i;
}

int anx_snprintf(char *buf, uint32_t size, const char *fmt, ...)
{
	__builtin_va_list ap;
	uint32_t pos = 0;
	const char *p = fmt;

	if (!buf || size == 0)
		return 0;

	__builtin_va_start(ap, fmt);
	while (*p && pos + 1 < size) {
		if (*p != '%') {
			buf[pos++] = *p++;
			continue;
		}
		p++;
		if (*p == 'u') {
			uint32_t v = __builtin_va_arg(ap, unsigned int);
			pos += fmt_uint64(buf + pos, size - pos, (uint64_t)v);
			p++;
		} else if (p[0] == 'l' && p[1] == 'l' && p[2] == 'u') {
			uint64_t v = __builtin_va_arg(ap, unsigned long long);
			pos += fmt_uint64(buf + pos, size - pos, v);
			p += 3;
		} else if (*p == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s && pos + 1 < size)
				buf[pos++] = *s++;
			p++;
		} else {
			buf[pos++] = '%';
			if (*p && pos + 1 < size)
				buf[pos++] = *p++;
		}
	}
	__builtin_va_end(ap);
	buf[pos] = '\0';
	return (int)pos;
}
