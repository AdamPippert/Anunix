/*
 * page_span_main.c — The page allocator over a span with holes.
 *
 * A separate binary: it re-initialises the global page allocator, which
 * the main test runner's heap depends on. The span is simulated address
 * arithmetic only; no page is ever written, so the addresses need not be
 * backed by memory.
 */

#include <anx/types.h>
#include <anx/page.h>

int printf(const char *fmt, ...);
void exit(int code);

void anx_panic(const char *file, int line, const char *msg)
{
	printf("  PANIC %s:%d: %s\n", file, line, msg);
	exit(2);
}

static int failures;

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			printf("  FAIL: %s (line %d)\n", (msg), __LINE__); \
			failures++;					\
		}							\
	} while (0)

#define MIB	(1ull << 20)
#define BASE	(0x1800000ull)			/* 24 MiB */
#define TOP	(0x80000000ull)			/* 2 GiB */

static bool inside(uintptr_t a, uint64_t len, uint64_t lo, uint64_t hi)
{
	return a + len > lo && a < hi;
}

int main(void)
{
	uint64_t total, free_pages, before;
	uintptr_t a, b, c;
	int i;

	/* RAM: 24..512 MiB and 1..2 GiB; a hole between; exec window out */
	anx_page_init_span(BASE, TOP);
	anx_page_add_free(0, 512 * MIB);	/* clipped to the span */
	anx_page_add_free(1024 * MIB, TOP);
	anx_page_add_free(40 * MIB, 41 * MIB);	/* already free: no-op */
	anx_page_reserve(32 * MIB, 128 * MIB);

	anx_page_stats(&total, &free_pages);
	CHECK(total == ((512 - 24 - 96 + 1024) * MIB) >> 12, "usable pages");
	CHECK(free_pages == total, "all usable pages free");

	/* Small allocations come from the bottom, below the exec window. */
	a = anx_page_alloc(0);
	CHECK(a == BASE, "first page at the span base");
	for (i = 0; i < 2048; i++) {
		b = anx_page_alloc(0);
		CHECK(b && !inside(b, 4096, 32 * MIB, 128 * MIB) &&
		      !inside(b, 4096, 512 * MIB, 1024 * MIB), "small page");
	}
	CHECK(b >= 128 * MIB, "small pages skip the exec window");

	/* Large allocations come from the top and never cross the hole. */
	c = anx_page_alloc(13);
	CHECK(c == TOP - 32 * MIB, "32 MiB block at the top");
	for (i = 0; i < 40; i++) {
		b = anx_page_alloc(13);
		CHECK(b && !inside(b, 32 * MIB, 512 * MIB, 1024 * MIB) &&
		      !inside(b, 32 * MIB, 32 * MIB, 128 * MIB),
		      "large block avoids holes");
	}

	/* A block larger than any RAM run fails cleanly. */
	CHECK(anx_page_alloc(19) == 0, "2 GiB block impossible");

	anx_page_stats(NULL, &before);
	anx_page_free(c, 13);
	anx_page_stats(NULL, &free_pages);
	CHECK(free_pages == before + 8192, "free returns pages");
	CHECK(anx_page_alloc(13) == c, "freed block reused");

	/* The classic single-region initialiser still works. */
	anx_page_init(BASE, BASE + 16 * MIB);
	anx_page_stats(&total, &free_pages);
	CHECK(total == 4096 && free_pages == 4096, "linker heap");
	CHECK(anx_page_alloc(12) == BASE, "whole 16 MiB block");
	CHECK(anx_page_alloc(0) == 0, "then empty");

	printf("  page_span: %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
