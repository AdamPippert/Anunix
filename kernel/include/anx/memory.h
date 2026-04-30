/*
 * memory.h — PAL (Policy and Action Learning) cross-session memory accumulator.
 *
 * RFC-0020 Phase 5 (accumulator) + Phase 12 (live query API).
 *
 * anx_pal_memory_update() is called by loop_memory.c after each committed or
 * aborted session; it performs an EMA update of the per-world per-action stats.
 *
 * anx_pal_action_prior() is called by the IBAL scorer at the start of each
 * iteration to bias action proposal selection toward historically lower-cost
 * actions.  Returns 0.5 (neutral) until PAL_COLD_GATE sessions have been
 * processed for the world.
 *
 * anx_pal_stats_get() is used by the shell and diagnostics subsystem to
 * inspect the accumulated per-action statistics.
 */

#ifndef ANX_MEMORY_H
#define ANX_MEMORY_H

#include <anx/loop.h>

/* ------------------------------------------------------------------ */
/* Update                                                              */
/* ------------------------------------------------------------------ */

/*
 * Update the PAL cross-session memory accumulator for a given world URI
 * with the consolidation payload produced after a committed or aborted
 * loop session.
 */
void anx_pal_memory_update(const char *world_uri,
                            const struct anx_loop_memory_payload *payload);

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

/*
 * Return the PAL-learned prior energy for action_id in world_uri.
 * Range [0.0, 1.0]; lower = action historically optimal.
 * Returns 0.5 (neutral) when world unknown or cold-start gate not met.
 */
float anx_pal_action_prior(const char *world_uri, uint32_t action_id);

/* Per-action PAL statistics for shell / diagnostics output. */
struct anx_pal_action_info {
	float    avg_energy;
	float    win_rate;
	float    min_energy;
	uint32_t sample_count;	/* total iterations contributing to this action */
	uint32_t session_count;	/* sessions whose data covered this action */
};

/*
 * Fill stats_out[0..min(max_actions, ANX_MEMORY_ACT_COUNT)-1] for world_uri.
 * Sets *count_out to the number of actions written.
 * Returns ANX_OK, ANX_ENOENT if world not known, ANX_EINVAL on bad args.
 */
int anx_pal_stats_get(const char *world_uri,
		      struct anx_pal_action_info *stats_out,
		      uint32_t max_actions, uint32_t *count_out);

/* Number of sessions accumulated so far for world_uri (0 if unknown). */
uint32_t anx_pal_session_count(const char *world_uri);

/* ------------------------------------------------------------------ */
/* PAL warm-start priming (loop_pal_prime.c)                          */
/* ------------------------------------------------------------------ */

/* Call after anx_drivers_probe() — uses detected hardware to pre-seed priors. */
void anx_pal_prime_hardware(void);

/* Call for each workflow URI in a kickstart [workflows] section. */
void anx_pal_prime_kickstart(const char *workflow_uri);

/* ANX_PAL_PRIME_HW_* flags for anx_pal_prime_install(). */
#define ANX_PAL_PRIME_HW_NPU      (1u << 0)
#define ANX_PAL_PRIME_HW_GPU      (1u << 1)
#define ANX_PAL_PRIME_HW_WIFI     (1u << 2)
#define ANX_PAL_PRIME_HW_ETHERNET (1u << 3)
#define ANX_PAL_PRIME_HW_STORAGE  (1u << 4)

/* Call at end of install with a bitmask of detected hardware. */
void anx_pal_prime_install(uint32_t hardware_flags);

/* ------------------------------------------------------------------ */
/* PAL persistence (loop_pal.c)                                       */
/* ------------------------------------------------------------------ */

/* Save the PAL accumulator state to the disk object store. */
void anx_pal_persist_save(void);

/* Load a previously saved PAL state from disk.  Also marks disk as
 * available so subsequent sessions trigger auto-save.  Call after
 * anx_disk_store_init() succeeds. */
void anx_pal_persist_load(void);

#endif /* ANX_MEMORY_H */
