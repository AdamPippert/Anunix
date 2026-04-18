/*
 * anx/perf.h — Performance profiling macros.
 *
 * TSC-based nanosecond-precision timing for profiling hot paths.
 * Use PERF_BEGIN/PERF_END around critical sections. Results are
 * logged via kprintf on boot and available via the 'perf' command.
 */

#ifndef ANX_PERF_H
#define ANX_PERF_H

#include <anx/types.h>

/* Read a high-resolution cycle counter */
static inline uint64_t anx_rdtsc(void)
{
#ifdef __x86_64__
	uint32_t lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
	uint64_t val;

	__asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
	return val;
#else
	return 0;
#endif
}

/* Boot-time profiling slots */
#define ANX_PERF_MAX_SLOTS	32

struct anx_perf_entry {
	const char *name;
	uint64_t tsc_start;
	uint64_t tsc_end;
	uint64_t cycles;
};

/* Start profiling a named section */
void anx_perf_begin(const char *name);

/* End profiling the current section */
void anx_perf_end(void);

/* Print all profiling results */
void anx_perf_report(void);

/* Convenience macros */
#define PERF_BEGIN(name) anx_perf_begin(name)
#define PERF_END()       anx_perf_end()

#endif /* ANX_PERF_H */
