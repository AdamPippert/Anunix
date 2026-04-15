/*
 * anx/hwprobe.h — Hardware capability probing.
 *
 * Discovers CPU, memory, and accelerator capabilities at boot.
 * Each architecture implements arch_probe_hw() to populate the
 * inventory from device tree, ACPI, or static configuration.
 */

#ifndef ANX_HWPROBE_H
#define ANX_HWPROBE_H

#include <anx/types.h>
#include <anx/engine_lease.h>

/* --- Hardware accelerator descriptor --- */

#define ANX_MAX_ACCELERATORS	4

struct anx_hw_accel {
	enum anx_accel_type type;
	char name[64];
	uint64_t mem_bytes;		/* dedicated memory */
	uint32_t compute_units;		/* CUs / cores / ANE cores */
};

/* --- Hardware inventory (populated once at boot) --- */

struct anx_hw_inventory {
	uint32_t cpu_count;
	uint64_t ram_bytes;

	struct anx_hw_accel accels[ANX_MAX_ACCELERATORS];
	uint32_t accel_count;
};

/* Forward declaration */
struct anx_engine;

/* --- Hardware Probe API --- */

/* Probe hardware and populate global inventory (call once at boot) */
void anx_hwprobe_init(void);

/* Get the current hardware inventory */
const struct anx_hw_inventory *anx_hwprobe_get(void);

/* Benchmark a loaded model engine (stub — returns ANX_ENOSYS) */
int anx_hwprobe_bench_engine(struct anx_engine *engine);

#endif /* ANX_HWPROBE_H */
