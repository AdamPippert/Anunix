/*
 * kprintf.c — Minimal kernel printf.
 *
 * Supports: %s, %d, %u, %x, %p, %c, %%.
 * Output goes through arch_console_putc().
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/kprintf.h>
#include <anx/fbcon.h>

typedef __builtin_va_list va_list;
#define va_start(ap, last)	__builtin_va_start(ap, last)
#define va_arg(ap, type)	__builtin_va_arg(ap, type)
#define va_end(ap)		__builtin_va_end(ap)

static void putc(char c)
{
	arch_console_putc(c);
	if (anx_fbcon_active())
		anx_fbcon_putc(c);
}

static void puts(const char *s)
{
	while (*s)
		putc(*s++);
}

static void put_uint(uint64_t val, int base, int width, char pad)
{
	char buf[20];
	int i = 0;

	if (val == 0) {
		buf[i++] = '0';
	} else {
		while (val > 0) {
			int digit = val % base;
			buf[i++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
			val /= base;
		}
	}

	/* pad */
	while (i < width)
		buf[i++] = pad;

	/* reverse output */
	while (i > 0)
		putc(buf[--i]);
}

static void put_int(int64_t val)
{
	if (val < 0) {
		putc('-');
		put_uint(-val, 10, 0, '0');
	} else {
		put_uint(val, 10, 0, '0');
	}
}

int kprintf(const char *fmt, ...)
{
	va_list ap;
	int count = 0;

	va_start(ap, fmt);

	while (*fmt) {
		if (*fmt != '%') {
			putc(*fmt++);
			count++;
			continue;
		}

		fmt++; /* skip '%' */

		switch (*fmt) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			puts(s);
			break;
		}
		case 'd': {
			int64_t val = va_arg(ap, int);
			put_int(val);
			break;
		}
		case 'u': {
			uint64_t val = va_arg(ap, unsigned int);
			put_uint(val, 10, 0, '0');
			break;
		}
		case 'x': {
			uint64_t val = va_arg(ap, unsigned int);
			put_uint(val, 16, 0, '0');
			break;
		}
		case 'p': {
			uintptr_t val = (uintptr_t)va_arg(ap, void *);
			puts("0x");
			put_uint(val, 16, 16, '0');
			break;
		}
		case 'c': {
			char c = (char)va_arg(ap, int);
			putc(c);
			break;
		}
		case '%':
			putc('%');
			break;
		default:
			putc('%');
			putc(*fmt);
			break;
		}

		fmt++;
		count++;
	}

	va_end(ap);
	return count;
}
