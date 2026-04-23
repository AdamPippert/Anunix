/*
 * jepa_obs.c — JEPA system observation collector.
 *
 * anx_jepa_observe() samples Anunix kernel subsystems and fills an
 * anx_jepa_obs snapshot.  The active world profile's collect_obs hook
 * is called for the raw collection; for os-default this file provides
 * the implementation directly.
 *
 * All reads are best-effort: if a subsystem counter is momentarily
 * inconsistent the snapshot is still valid — JEPA is a soft signal, not
 * a hard guarantee.
 */

#include <anx/jepa.h>
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/state_object.h>
#include <anx/sched.h>
#include <anx/memplane.h>
#include <anx/cell.h>
#include <anx/route.h>
#include <anx/string.h>
#include <anx/alloc.h>

/* ------------------------------------------------------------------ */
/* Internal: collect os-default observation                            */
/* ------------------------------------------------------------------ */

/*
 * Counts active cells across all lifecycle states except COMPLETED,
 * FAILED, CANCELLED, and COMPENSATED.
 */
struct cell_count_arg {
	uint32_t count;
};

static int count_active_cell(struct anx_cell *cell, void *arg)
{
	struct cell_count_arg *a = (struct cell_count_arg *)arg;

	switch (cell->status) {
	case ANX_CELL_COMPLETED:
	case ANX_CELL_FAILED:
	case ANX_CELL_CANCELLED:
	case ANX_CELL_COMPENSATED:
		break;
	default:
		a->count++;
		break;
	}
	return 0;
}

/*
 * Per-tier memory stats: sum decay_score and count entries.
 * Called once per entry by anx_objstore_iterate-equivalent via
 * a separate memplane pass.  We directly use anx_sched_queue_depth()
 * and a simple counter walk instead of a full iterate to keep this
 * fast (no allocations, no locks held across iteration).
 */

int anx_jepa_obs_collect_os_default(void *obs_buf, uint32_t obs_buf_size)
{
	struct anx_jepa_obs *obs = (struct anx_jepa_obs *)obs_buf;
	struct cell_count_arg ca;
	uint32_t i;

	if (!obs_buf || obs_buf_size < sizeof(struct anx_jepa_obs))
		return ANX_EINVAL;

	anx_memset(obs, 0, sizeof(*obs));

	/* Scheduler queue depths */
	for (i = 0; i < ANX_JEPA_OBS_SCHED_CLASSES; i++)
		obs->sched_queue_depths[i] =
			anx_sched_queue_depth((enum anx_queue_class)i);

	/* Active cell count */
	ca.count = 0;
	anx_cell_store_iterate(count_active_cell, &ca);
	obs->active_cell_count = ca.count;

	/*
	 * Memory tier stats.
	 * anx_memplane does not expose a per-tier aggregate API yet, so we
	 * leave these at 0 until memplane gains tier-level counters.
	 * The obs struct is still useful: the scheduler and cell counts
	 * alone carry meaningful signal for routing and scheduling decisions.
	 */
	for (i = 0; i < ANX_JEPA_OBS_MEM_TIERS; i++) {
		obs->mem_decay_score_avg[i] = 0;
		obs->mem_entry_counts[i]    = 0;
	}

	/* Routing stats: not yet exposed by the planner; zero for now. */
	obs->route_fallback_count = 0;
	obs->route_avg_score      = 0.0f;

	/* Tensor utilization: no real-time util counters yet; zero for now. */
	obs->tensor_cpu_util = 0.0f;
	obs->tensor_npu_util = 0.0f;

	/* Capability validation: not yet exposed; zero for now. */
	obs->cap_validation_avg = 0.0f;
	obs->cap_failures       = 0;

	/* Error and security counters: not yet centralised; zero for now. */
	obs->error_count          = 0;
	obs->security_event_count = 0;

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int anx_jepa_observe(struct anx_jepa_obs *obs_out)
{
	const struct anx_jepa_world_profile *world;
	int rc;

	if (!obs_out)
		return ANX_EINVAL;

	if (!anx_jepa_available())
		return ANX_ENOENT;

	world = anx_jepa_world_get_active();
	if (!world || !world->collect_obs)
		return ANX_ENOENT;

	rc = world->collect_obs((void *)obs_out, sizeof(struct anx_jepa_obs));
	return rc;
}

int anx_jepa_observe_store(const struct anx_jepa_obs *obs,
			   anx_oid_t *oid_out)
{
	struct anx_so_create_params params;
	struct anx_state_object *so;
	int rc;

	if (!obs || !oid_out)
		return ANX_EINVAL;

	anx_memset(&params, 0, sizeof(params));
	params.object_type  = ANX_OBJ_JEPA_OBS;
	params.schema_uri   = "anx:schema/jepa-obs/v1";
	params.payload      = obs;
	params.payload_size = sizeof(struct anx_jepa_obs);

	rc = anx_so_create(&params, &so);
	if (rc != ANX_OK)
		return rc;

	*oid_out = so->oid;
	anx_objstore_release(so);
	return ANX_OK;
}
