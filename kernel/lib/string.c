/*
 * string.c — Kernel string and memory functions.
 *
 * Word-wide (64-bit) bulk paths for memcpy/memset/memmove.
 * Framebuffer scrolls copy ~8 MB per newline; byte-at-a-time is
 * the scroll-lag bottleneck — 64-bit words give an 8× improvement
 * with no SIMD required (all general registers on x86_64 / arm64).
 */

#include <anx/string.h>

void *anx_memcpy(void *dst, const void *src, size_t n)
{
	uint8_t       *d8 = (uint8_t *)dst;
	const uint8_t *s8 = (const uint8_t *)src;

	if ((((size_t)dst | (size_t)src) & 7) == 0) {
		uint64_t       *d64 = (uint64_t *)dst;
		const uint64_t *s64 = (const uint64_t *)src;
		size_t          words = n / 8;

		while (words--)
			*d64++ = *s64++;

		d8 = (uint8_t *)d64;
		s8 = (const uint8_t *)s64;
		n  = n % 8;
	}

	while (n--)
		*d8++ = *s8++;

	return dst;
}

void *anx_memset(void *dst, int val, size_t n)
{
	uint8_t  v8  = (uint8_t)val;
	uint8_t *d8  = (uint8_t *)dst;

	if (((size_t)dst & 7) == 0) {
		uint64_t  v64 = (uint64_t)v8 * 0x0101010101010101ULL;
		uint64_t *d64 = (uint64_t *)dst;
		size_t    words = n / 8;

		while (words--)
			*d64++ = v64;

		d8 = (uint8_t *)d64;
		n  = n % 8;
	}

	while (n--)
		*d8++ = v8;

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
	uint8_t       *d = dst;
	const uint8_t *s = src;

	if (d == s || n == 0)
		return dst;

	if (d < s) {
		/* Forward copy: word-wide where possible */
		uint64_t       *d64 = dst;
		const uint64_t *s64 = src;
		size_t          words = n / 8;
		size_t          tail  = n % 8;

		while (words--)
			*d64++ = *s64++;

		d = (uint8_t *)d64;
		s = (const uint8_t *)s64;
		while (tail--)
			*d++ = *s++;
	} else {
		/* Backward copy — must be byte-at-a-time to stay correct */
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

char *anx_strncat(char *dst, const char *src, size_t n)
{
	char *p = dst;

	while (*p)
		p++;
	while (n-- && *src)
		*p++ = *src++;
	*p = '\0';
	return dst;
}

const char *anx_strstr(const char *haystack, const char *needle)
{
	size_t nlen = anx_strlen(needle);

	if (nlen == 0)
		return haystack;
	while (*haystack) {
		if (anx_memcmp(haystack, needle, nlen) == 0)
			return haystack;
		haystack++;
	}
	return NULL;
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

/* Append one formatted field to buf, padded to width. */
static uint32_t snp_field(char *buf, uint32_t pos, uint32_t size,
			  const char *digits, uint32_t n, bool neg,
			  uint32_t width, char pad, bool left)
{
	uint32_t len = n + (neg ? 1 : 0), i;

	if (neg && pad == '0' && pos + 1 < size)
		buf[pos++] = '-';
	for (i = len; !left && i < width && pos + 1 < size; i++)
		buf[pos++] = pad;
	if (neg && pad != '0' && pos + 1 < size)
		buf[pos++] = '-';
	for (i = 0; i < n && pos + 1 < size; i++)
		buf[pos++] = digits[i];
	for (i = len; left && i < width && pos + 1 < size; i++)
		buf[pos++] = ' ';
	return pos;
}

/*
 * A subset of snprintf: flags '-' and '0', a field width, a precision
 * for strings (".N" or ".*"), the length modifiers l and ll, and the
 * conversions d u x s c %. Output is always
 * NUL-terminated; the return value is the length written.
 */
int anx_snprintf(char *buf, uint32_t size, const char *fmt, ...)
{
	__builtin_va_list ap;
	uint32_t pos = 0;
	const char *p = fmt;

	if (!buf || size == 0)
		return 0;

	__builtin_va_start(ap, fmt);
	while (*p && pos + 1 < size) {
		char digits[24];
		uint32_t n = 0, width = 0, lng = 0;
		int32_t prec = -1;
		bool left = false, neg = false;
		char pad = ' ';
		uint64_t v = 0;

		if (*p != '%') {
			buf[pos++] = *p++;
			continue;
		}
		p++;
		if (*p == '-') {
			left = true;
			p++;
		}
		if (*p == '0' && !left) {
			pad = '0';
			p++;
		}
		while (*p >= '0' && *p <= '9')
			width = width * 10 + (uint32_t)(*p++ - '0');
		if (*p == '.') {
			p++;
			prec = 0;
			if (*p == '*') {
				prec = __builtin_va_arg(ap, int);
				p++;
			}
			while (*p >= '0' && *p <= '9')
				prec = prec * 10 + (int32_t)(*p++ - '0');
		}
		while (*p == 'l') {
			lng++;
			p++;
		}

		switch (*p) {
		case 'd': {
			int64_t sv = lng >= 2 ? (int64_t)__builtin_va_arg(ap, long long) :
				     lng == 1 ? (int64_t)__builtin_va_arg(ap, long) :
				     (int64_t)__builtin_va_arg(ap, int);

			neg = sv < 0;
			v = neg ? (uint64_t)0 - (uint64_t)sv : (uint64_t)sv;
			goto decimal;
		}
		case 'u':
			v = lng >= 2 ? (uint64_t)__builtin_va_arg(ap, unsigned long long) :
			    lng == 1 ? (uint64_t)__builtin_va_arg(ap, unsigned long) :
			    (uint64_t)__builtin_va_arg(ap, unsigned int);
decimal:
			do {
				digits[sizeof(digits) - 1 - n++] =
					(char)('0' + v % 10);
				v /= 10;
			} while (v);
			pos = snp_field(buf, pos, size,
					digits + sizeof(digits) - n, n, neg,
					width, pad, left);
			break;
		case 'x':
			v = lng >= 2 ? (uint64_t)__builtin_va_arg(ap, unsigned long long) :
			    lng == 1 ? (uint64_t)__builtin_va_arg(ap, unsigned long) :
			    (uint64_t)__builtin_va_arg(ap, unsigned int);
			do {
				digits[sizeof(digits) - 1 - n++] =
					"0123456789abcdef"[v & 0xf];
				v >>= 4;
			} while (v);
			pos = snp_field(buf, pos, size,
					digits + sizeof(digits) - n, n, false,
					width, pad, left);
			break;
		case 's': {
			const char *s = __builtin_va_arg(ap, const char *);

			if (!s)
				s = "(null)";
			while (s[n] && (prec < 0 || n < (uint32_t)prec))
				n++;
			pos = snp_field(buf, pos, size, s, n, false,
					width, ' ', left);
			break;
		}
		case 'c':
			digits[0] = (char)__builtin_va_arg(ap, int);
			pos = snp_field(buf, pos, size, digits, 1, false,
					width, ' ', left);
			break;
		case '%':
			buf[pos++] = '%';
			break;
		default:
			buf[pos++] = '%';
			if (*p && pos + 1 < size)
				buf[pos++] = *p;
			break;
		}
		if (*p)
			p++;
	}
	__builtin_va_end(ap);
	buf[pos] = '\0';
	return (int)pos;
}
