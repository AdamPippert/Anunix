/*
 * perf.c — Performance profiling infrastructure.
 *
 * Records TSC-based timing for boot subsystems and hot paths.
 * Results can be displayed via the 'perf' shell command.
 */

#include <anx/types.h>
#include <anx/perf.h>
#include <anx/kprintf.h>

static struct anx_perf_entry entries[ANX_PERF_MAX_SLOTS];
static uint32_t entry_count;
static uint32_t current_slot;
static bool in_section;

void anx_perf_begin(const char *name)
{
	if (entry_count >= ANX_PERF_MAX_SLOTS)
		return;

	current_slot = entry_count;
	entries[current_slot].name = name;
	entries[current_slot].tsc_start = anx_rdtsc();
	entries[current_slot].tsc_end = 0;
	entries[current_slot].cycles = 0;
	in_section = true;
}

void anx_perf_end(void)
{
	uint64_t end;

	if (!in_section)
		return;

	end = anx_rdtsc();
	entries[current_slot].tsc_end = end;
	entries[current_slot].cycles = end - entries[current_slot].tsc_start;
	entry_count++;
	in_section = false;
}

void anx_perf_report(void)
{
	uint32_t i;
	uint64_t total_cycles = 0;

	kprintf("\n=== Performance Profile ===\n\n");

	for (i = 0; i < entry_count; i++) {
		uint64_t cycles = entries[i].cycles;
		uint32_t kcycles = (uint32_t)(cycles / 1000);
		uint32_t us = kcycles; /* ~1 GHz approx */

		kprintf("  %s: %u Kcycles (~%u us)\n",
			entries[i].name, kcycles, us);
		total_cycles += cycles;
	}

	kprintf("\n  TOTAL: %u Kcycles (~%u us)\n",
		(uint32_t)(total_cycles / 1000),
		(uint32_t)(total_cycles / 1000));
	kprintf("\n");
}
