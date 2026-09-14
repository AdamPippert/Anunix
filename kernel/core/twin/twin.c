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
#include <anx/tuning.h>

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
	struct anx_route_tuning_state current;
	if (!out)
		return;
	anx_route_tuning_snapshot(&current);
	*out = current.weights;
}

int anx_route_weight_policy_validate(const struct anx_route_weight_policy *p)
{
	if (!p || p->gpu_cost_divisor < 1 || p->gpu_cost_divisor > ANX_ROUTE_WEIGHT_LIMIT ||
	    p->cpu_cost_divisor < 1 || p->cpu_cost_divisor > ANX_ROUTE_WEIGHT_LIMIT ||
	    p->locality_bonus < 0 || p->locality_bonus > ANX_ROUTE_WEIGHT_LIMIT ||
	    p->local_first_bonus < 0 || p->local_first_bonus > ANX_ROUTE_WEIGHT_LIMIT ||
	    p->private_data_bonus < 0 || p->private_data_bonus > ANX_ROUTE_WEIGHT_LIMIT ||
	    p->topology_overlap_bonus < 0 || p->topology_overlap_bonus > ANX_ROUTE_WEIGHT_LIMIT ||
	    p->degraded_penalty > 0 || p->degraded_penalty < -ANX_ROUTE_WEIGHT_LIMIT ||
	    p->topology_mismatch_penalty > 0 || p->topology_mismatch_penalty < -ANX_ROUTE_WEIGHT_LIMIT)
		return ANX_EINVAL;
	return ANX_OK;
}

static int validate_simulation(const struct anx_resource_twin *twin,
			       const struct anx_cell *cell)
{
	uint32_t i;

	if (twin->engine_count > ANX_TWIN_MAX_ENGINES ||
	    (uint32_t)cell->constraints.locality > ANX_REMOTE_REQUIRED ||
	    (uint32_t)cell->routing.strategy > ANX_ROUTE_POLICY_LOCKED ||
	    (cell->constraints.topology_bk_set &&
	     cell->constraints.topology_bk_lo > cell->constraints.topology_bk_hi))
		return ANX_EINVAL;
	for (i = 0; i < twin->engine_count; i++) {
		const struct anx_twin_engine_snapshot *s = &twin->engines[i];
		if ((uint32_t)s->engine_class >= ANX_ENGINE_CLASS_COUNT ||
		    (uint32_t)s->status >= ANX_ENGINE_STATUS_COUNT ||
		    s->readiness != anx_readiness_from_status(s->status) ||
		    s->cpu_weight > 100 || s->gpu_weight > 100 || s->quality_score > 100 ||
		    (s->has_topology_affinity && s->topology_bk_lo > s->topology_bk_hi))
			return ANX_EINVAL;
	}
	return ANX_OK;
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

	int capacity = anx_lease_snapshot_capacity(&twin->capacity);
	if (capacity != ANX_OK && capacity != ANX_ENODEV) { anx_free(twin); return capacity; }
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
	/* Preflight bounds imply scores in [-2200, 4100] and margins <= 6300. */
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
	if (anx_route_weight_policy_validate(policy) != ANX_OK ||
	    validate_simulation(twin, cell) != ANX_OK)
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

int anx_twin_simulate_restoration(struct anx_resource_twin *twin, struct anx_cell *cell,
		const struct anx_route_weight_policy *policy, const struct anx_twin_restore_request *request,
		struct anx_twin_restore_result *out)
{
	if (!twin || !cell || !policy || !request || !out) return ANX_EINVAL;
	if (anx_route_weight_policy_validate(policy) != ANX_OK || validate_simulation(twin, cell) != ANX_OK ||
	    (uint32_t)request->tier >= ANX_MEM_TIER_COUNT || (uint32_t)request->accelerator >= ANX_ACCEL_COUNT ||
	    request->accelerator_pct > 100 || (request->accelerator == ANX_ACCEL_NONE && request->accelerator_pct) ||
	    !request->retained_bytes || !request->restore_peak_bytes || request->resident_bytes > request->retained_bytes ||
	    request->restore_peak_bytes < request->resident_bytes) return ANX_EINVAL;
	if (!twin->capacity.schema) return ANX_ENOTSUP;
	if (twin->capacity.schema != 1) return ANX_EINVAL;
	for (uint32_t i = 0; i < ANX_MEM_TIER_COUNT; i++)
		if (twin->capacity.free_memory[i] > twin->capacity.total_memory[i]) return ANX_EINVAL;
	for (uint32_t i = 0; i < ANX_ACCEL_COUNT; i++)
		if (twin->capacity.total_accelerator[i] > 100 ||
		    twin->capacity.free_accelerator[i] > twin->capacity.total_accelerator[i]) return ANX_EINVAL;
	struct anx_twin_restore_result result;
	anx_memset(&result, 0, sizeof(result));
	result.additional_memory_bytes = request->restore_peak_bytes - request->resident_bytes;
	if (result.additional_memory_bytes > twin->capacity.free_memory[request->tier] ||
	    request->accelerator_pct > twin->capacity.free_accelerator[request->accelerator]) return ANX_ENOMEM;
	int ret = anx_twin_simulate(twin, cell, policy, &result.route);
	if (ret == ANX_OK) *out = result;
	return ret;
}
