/*
 * anx/delay.h — Busy-wait delays with a guaranteed minimum.
 *
 * The TSC is not calibrated, and driver probe runs before interrupts are
 * enabled, so timer ticks cannot measure a wait. A delay here assumes the
 * fastest plausible TSC rate and never returns early. On a slower clock it
 * lasts proportionally longer. Use it where a specification sets a minimum
 * wait, such as USB port reset or hub power-good time.
 */

#ifndef ANX_DELAY_H
#define ANX_DELAY_H

#include <anx/types.h>
#include <anx/perf.h>

/* Fastest TSC rate a delay allows for, in Hz. */
#define ANX_TSC_HZ_MAX		6000000000ULL

/* TSC cycles that cover at least us microseconds. */
static inline uint64_t anx_tsc_budget_us(uint64_t us)
{
	return us * (ANX_TSC_HZ_MAX / 1000000ULL);
}

static inline void anx_delay_us(uint64_t us)
{
	uint64_t start = anx_rdtsc();
	uint64_t budget = anx_tsc_budget_us(us);

	/* No cycle counter on this architecture: a wait would never end. */
	if (start == 0 && anx_rdtsc() == 0)
		return;

	while (anx_rdtsc() - start < budget) {
#if defined(__x86_64__)
		__asm__ volatile("pause");
#endif
	}
}

static inline void anx_delay_ms(uint64_t ms)
{
	anx_delay_us(ms * 1000ULL);
}

#endif /* ANX_DELAY_H */
