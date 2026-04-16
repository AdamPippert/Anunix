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

/* Read the TSC (Time Stamp Counter) — sub-nanosecond on modern CPUs */
static inline uint64_t anx_rdtsc(void)
{
	uint32_t lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
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
