/*
 * twin.c — Resource Twin implementation.
 *
 * Snapshot + what-if simulation over the engine registry and scheduler
 * queue depths. See kernel/include/anx/twin.h for scope notes.
 */

#include <anx/types.h>
#include <anx/twin.h>
#include <anx/alloc.h>
#include <anx/arch.h>
#include <anx/string.h>

void anx_twin_init(void)
{
	/* Nothing to initialize — the Twin is stateless between snapshots. */
}

enum anx_readiness anx_readiness_from_status(enum anx_engine_status status)
{
	switch (status) {
	case ANX_ENGINE_OFFLINE:
	case ANX_ENGINE_MAINTENANCE:
		return ANX_READY_NONE;
	case ANX_ENGINE_REGISTERED:
	case ANX_ENGINE_LOADING:
	case ANX_ENGINE_DRAINING:
	case ANX_ENGINE_UNLOADING:
		return ANX_READY_USABLE;
	case ANX_ENGINE_READY:
	case ANX_ENGINE_DEGRADED:
		return ANX_READY_READY;
	case ANX_ENGINE_AVAILABLE:
		return ANX_READY_HEALTHY;
	default:
		return ANX_READY_NONE;
	}
}

void anx_route_weight_policy_incumbent(struct anx_route_weight_policy *out)
{
	if (!out)
		return;

	/* Mirrors the constants anx_route_score_engine currently hardcodes
	 * (kernel/core/route/planner.c). Keep these in sync if that
	 * function's constants change. */
	out->locality_bonus = 20;
	out->local_first_bonus = 30;
	out->gpu_cost_divisor = 5;
	out->cpu_cost_divisor = 10;
	out->degraded_penalty = -25;
	out->private_data_bonus = 10;
	out->topology_overlap_bonus = 25;
	out->topology_mismatch_penalty = -15;
}

int anx_twin_snapshot(struct anx_resource_twin **out)
{
	struct anx_resource_twin *twin;
	uint32_t class_idx;
	uint32_t queue_idx;

	if (!out)
		return ANX_EINVAL;

	twin = anx_zalloc(sizeof(*twin));
	if (!twin)
		return ANX_ENOMEM;

	for (class_idx = 0;
	     class_idx < ANX_ENGINE_CLASS_COUNT &&
	     twin->engine_count < ANX_TWIN_MAX_ENGINES;
	     class_idx++) {
		struct anx_engine *found[ANX_TWIN_MAX_ENGINES];
		uint32_t found_count = 0;
		uint32_t remaining = ANX_TWIN_MAX_ENGINES - twin->engine_count;
		uint32_t i;

		anx_engine_find((enum anx_engine_class)class_idx, 0,
				found, remaining, &found_count);

		for (i = 0; i < found_count &&
		     twin->engine_count < ANX_TWIN_MAX_ENGINES; i++) {
			struct anx_twin_engine_snapshot *snap;
			struct anx_engine *eng = found[i];

			snap = &twin->engines[twin->engine_count];
			snap->eid = eng->eid;
			snap->engine_class = eng->engine_class;
			snap->status = eng->status;
			snap->readiness = anx_readiness_from_status(eng->status);
			snap->supports_private_data = eng->supports_private_data;
			snap->requires_network = eng->requires_network;
			snap->cpu_weight = eng->cpu_weight;
			snap->gpu_weight = eng->gpu_weight;
			snap->quality_score = eng->quality_score;
			snap->is_local = eng->is_local;
			snap->has_topology_affinity = eng->has_topology_affinity;
			snap->topology_bk_lo = eng->topology_bk_lo;
			snap->topology_bk_hi = eng->topology_bk_hi;

			twin->engine_count++;
		}
	}

	for (queue_idx = 0; queue_idx < ANX_QUEUE_CLASS_COUNT; queue_idx++)
		twin->queue_depth[queue_idx] =
			anx_sched_queue_depth((enum anx_queue_class)queue_idx);

	twin->taken_at = arch_time_now();

	*out = twin;
	return ANX_OK;
}

void anx_twin_destroy(struct anx_resource_twin *twin)
{
	anx_free(twin);
}

/*
 * Feasibility, mirrored from route/planner.c's engine_feasible() but
 * evaluated against a frozen snapshot entry rather than a live engine.
 * Kept in sync intentionally rather than shared — the live function
 * takes struct anx_engine *, not a snapshot value.
 */
static bool snapshot_feasible(struct anx_cell *cell,
			      const struct anx_twin_engine_snapshot *snap)
{
	if (snap->status == ANX_ENGINE_OFFLINE ||
	    snap->status == ANX_ENGINE_MAINTENANCE)
		return false;

	if (cell->constraints.locality == ANX_LOCAL_ONLY && !snap->is_local)
		return false;

	if (snap->requires_network && !cell->execution.allow_network)
		return false;

	if (snap->engine_class == ANX_ENGINE_REMOTE_MODEL &&
	    !cell->execution.allow_remote_models)
		return false;

	return true;
}

static int32_t score_snapshot(struct anx_cell *cell,
			      const struct anx_twin_engine_snapshot *snap,
			      const struct anx_route_weight_policy *policy)
{
	int32_t score = 0;

	score += (int32_t)snap->quality_score;

	if (snap->is_local) {
		score += policy->locality_bonus;
		if (cell->routing.strategy == ANX_ROUTE_LOCAL_FIRST)
			score += policy->local_first_bonus;
	}

	score -= (int32_t)snap->gpu_weight / policy->gpu_cost_divisor;
	score -= (int32_t)snap->cpu_weight / policy->cpu_cost_divisor;

	if (snap->status == ANX_ENGINE_DEGRADED)
		score += policy->degraded_penalty;

	if (snap->supports_private_data)
		score += policy->private_data_bonus;

	if (cell->constraints.topology_bk_set && snap->has_topology_affinity) {
		uint64_t ce_lo = cell->constraints.topology_bk_lo;
		uint64_t ce_hi = cell->constraints.topology_bk_hi;
		uint64_t eg_lo = snap->topology_bk_lo;
		uint64_t eg_hi = snap->topology_bk_hi;
		uint64_t olap_lo = (ce_lo > eg_lo) ? ce_lo : eg_lo;
		uint64_t olap_hi = (ce_hi < eg_hi) ? ce_hi : eg_hi;

		if (olap_lo <= olap_hi)
			score += policy->topology_overlap_bonus;
		else
			score += policy->topology_mismatch_penalty;
	}

	return score;
}

int anx_twin_simulate(struct anx_resource_twin *twin,
		      struct anx_cell *cell,
		      const struct anx_route_weight_policy *policy,
		      struct anx_twin_simulate_result *result_out)
{
	uint32_t i;
	int32_t best_score = 0;
	int32_t second_score = 0;
	uint32_t best_idx = 0;
	uint32_t feasible_count = 0;
	bool have_best = false;
	bool have_second = false;

	if (!twin || !cell || !policy || !result_out)
		return ANX_EINVAL;
	if (policy->gpu_cost_divisor == 0 || policy->cpu_cost_divisor == 0)
		return ANX_EINVAL;

	anx_memset(result_out, 0, sizeof(*result_out));

	for (i = 0; i < twin->engine_count; i++) {
		const struct anx_twin_engine_snapshot *snap = &twin->engines[i];
		int32_t score;

		if (!snapshot_feasible(cell, snap))
			continue;

		score = score_snapshot(cell, snap, policy);
		feasible_count++;

		if (!have_best || score > best_score) {
			if (have_best) {
				second_score = best_score;
				have_second = true;
			}
			best_score = score;
			best_idx = i;
			have_best = true;
		} else if (!have_second || score > second_score) {
			second_score = score;
			have_second = true;
		}
	}

	result_out->candidate_count = feasible_count;

	if (!have_best)
		return ANX_OK;

	result_out->winner_index = best_idx;
	result_out->winner_score = best_score;

	if (have_second) {
		result_out->has_margin = true;
		result_out->margin = best_score - second_score;
	}

	return ANX_OK;
}
