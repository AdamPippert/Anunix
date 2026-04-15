/*
 * mock_arch.c — Mock architecture functions for host-native testing.
 *
 * Stubs the arch.h interface so kernel code can run on the host
 * without actual hardware initialization.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/page.h>
#include <anx/fb.h>
#include <anx/hwprobe.h>

static uint64_t mock_time = 1000000000ULL;	/* 1 second in ns */

/* 4 MiB static heap for test builds */
#define MOCK_HEAP_SIZE	(4 * 1024 * 1024)
static uint8_t mock_heap[MOCK_HEAP_SIZE]
	__attribute__((aligned(4096)));

void arch_early_init(void)
{
}

void arch_init(void)
{
	uintptr_t start = (uintptr_t)mock_heap;
	uintptr_t end = start + MOCK_HEAP_SIZE;
	anx_page_init(start, end);
}

void arch_halt(void)
{
	/* Declared noreturn — spin forever in test builds */
	for (;;)
		;
}

bool arch_irq_disable(void)
{
	return false;
}

void arch_irq_enable(void)
{
}

void arch_irq_restore(bool flags)
{
	(void)flags;
}

anx_time_t arch_time_now(void)
{
	/* Monotonically increasing mock time */
	mock_time += 1000000;	/* +1ms per call */
	return mock_time;
}

/*
 * For host-native test builds, output to stdout via raw syscall
 * so we don't need <stdio.h> in freestanding-compatible headers.
 */
extern long write(int fd, const void *buf, unsigned long count);

void arch_console_putc(char c)
{
	write(1, &c, 1);
}

void arch_console_puts(const char *s)
{
	while (*s) {
		write(1, s, 1);
		s++;
	}
}

int arch_console_getc(void)
{
	/* In test builds, return EOF-like value — no interactive input */
	return -1;
}

bool arch_console_has_input(void)
{
	return false;
}

void arch_exception_init(void)
{
}

uint64_t arch_timer_ticks(void)
{
	return 0;
}

void arch_probe_hw(struct anx_hw_inventory *inv)
{
	/* Mock: 4 CPUs, 16 GiB RAM, 1 GPU */
	inv->cpu_count = 4;
	inv->ram_bytes = 16ULL * 1024 * 1024 * 1024;
	inv->accel_count = 1;
	inv->accels[0].type = ANX_ACCEL_GPU;
	inv->accels[0].mem_bytes = 8ULL * 1024 * 1024 * 1024;
	inv->accels[0].compute_units = 32;
}

void arch_fb_detect(struct anx_fb_info *info)
{
	/* No framebuffer in test builds — tests set it up directly */
	info->available = false;
}

void arch_mb(void)
{
}

void arch_rmb(void)
{
}

void arch_wmb(void)
{
}
