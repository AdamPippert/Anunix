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

/* Output capture — when active, kprintf also writes to a buffer */
static char *capture_buf;
static uint32_t capture_size;
static uint32_t capture_pos;

void anx_kprintf_capture_start(char *buf, uint32_t buf_size)
{
	capture_buf = buf;
	capture_size = buf_size;
	capture_pos = 0;
	if (buf && buf_size > 0)
		buf[0] = '\0';
}

uint32_t anx_kprintf_capture_stop(void)
{
	uint32_t n = capture_pos;

	/* Null-terminate if there's space */
	if (capture_buf && capture_pos < capture_size)
		capture_buf[capture_pos] = '\0';
	capture_buf = NULL;
	capture_size = 0;
	capture_pos = 0;
	return n;
}

static void putc(char c)
{
	arch_console_putc(c);
	if (anx_fbcon_active())
		anx_fbcon_putc(c);

	/* Tee to capture buffer if active */
	if (capture_buf && capture_pos + 1 < capture_size) {
		capture_buf[capture_pos++] = c;
		capture_buf[capture_pos] = '\0';
	}
}

/* Public single-char output for shell echo */
void kputc(char c)
{
	putc(c);
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

		/* Optional width specifier, e.g., "%02x" for 2-digit hex */
		int width = 0;
		char pad = ' ';
		if (*fmt == '0') {
			pad = '0';
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10 + (*fmt - '0');
			fmt++;
		}

		/* Length modifiers: l → long, ll → long long */
		int is_long = 0;
		while (*fmt == 'l') { is_long++; fmt++; }

		switch (*fmt) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			puts(s);
			break;
		}
		case 'd': {
			int64_t val = (is_long >= 2) ? (int64_t)va_arg(ap, long long) :
				      (is_long == 1) ? (int64_t)va_arg(ap, long) :
						       (int64_t)va_arg(ap, int);
			put_int(val);
			break;
		}
		case 'u': {
			uint64_t val = (is_long >= 2) ? (uint64_t)va_arg(ap, unsigned long long) :
				       (is_long == 1) ? (uint64_t)va_arg(ap, unsigned long) :
						        (uint64_t)va_arg(ap, unsigned int);
			put_uint(val, 10, width, pad);
			break;
		}
		case 'x': {
			uint64_t val = (is_long >= 2) ? (uint64_t)va_arg(ap, unsigned long long) :
				       (is_long == 1) ? (uint64_t)va_arg(ap, unsigned long) :
						        (uint64_t)va_arg(ap, unsigned int);
			put_uint(val, 16, width, pad);
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
