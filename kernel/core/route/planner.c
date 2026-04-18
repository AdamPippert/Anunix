/*
 * planner.c — Route Planner implementation.
 *
 * Evaluates cell intent and constraints against registered engines
 * to produce scored route candidates. Implements feasibility-first
 * filtering (RFC-0005 Section 10) followed by multi-objective
 * scoring (RFC-0005 Section 13).
 */

#include <anx/types.h>
#include <anx/route.h>
#include <anx/engine.h>
#include <anx/string.h>

void anx_route_planner_init(void)
{
	/* Nothing to initialize yet — planner is stateless */
}

#define ANX_ROUTE_TOPOLOGY_OVERLAP_BONUS	25
#define ANX_ROUTE_TOPOLOGY_MISMATCH_PENALTY	15

/*
 * Score a single engine against a cell's requirements.
 *
 * Scoring dimensions (RFC-0005 Section 13.3):
 *   +quality    (engine quality score)
 *   +locality   (local preferred by default)
 *   -cost       (higher gpu/cpu weight = more expensive)
 *   +policy fit (private data support, network constraints)
 *   +topology   (engine affinity overlaps cell's boundary-key range)
 */
int32_t anx_route_score_engine(struct anx_cell *cell,
			       struct anx_engine *engine)
{
	int32_t score = 0;

	if (!cell || !engine)
		return -1;

	/* Base quality */
	score += (int32_t)engine->quality_score;

	/* Locality bonus */
	if (engine->is_local) {
		score += 20;
		/* Extra bonus for local_first strategy */
		if (cell->routing.strategy == ANX_ROUTE_LOCAL_FIRST)
			score += 30;
	}

	/* Cost penalty: higher weight = more expensive */
	score -= (int32_t)(engine->gpu_weight / 5);
	score -= (int32_t)(engine->cpu_weight / 10);

	/* Degraded engine penalty */
	if (engine->status == ANX_ENGINE_DEGRADED)
		score -= 25;

	/* Private data bonus if engine supports it */
	if (engine->supports_private_data)
		score += 10;

	/*
	 * Topology affinity. When the cell declares a boundary-key
	 * range of interest and the engine declares one it serves,
	 * reward intervals that overlap and penalize engines that
	 * have specialized elsewhere. Engines without a declared
	 * affinity are treated as generalists (neutral on this axis).
	 */
	if (cell->constraints.topology_bk_set &&
	    engine->has_topology_affinity) {
		uint64_t ce_lo = cell->constraints.topology_bk_lo;
		uint64_t ce_hi = cell->constraints.topology_bk_hi;
		uint64_t eg_lo = engine->topology_bk_lo;
		uint64_t eg_hi = engine->topology_bk_hi;
		uint64_t olap_lo = (ce_lo > eg_lo) ? ce_lo : eg_lo;
		uint64_t olap_hi = (ce_hi < eg_hi) ? ce_hi : eg_hi;

		if (olap_lo <= olap_hi)
			score += ANX_ROUTE_TOPOLOGY_OVERLAP_BONUS;
		else
			score -= ANX_ROUTE_TOPOLOGY_MISMATCH_PENALTY;
	}

	return score;
}

/*
 * Check feasibility of an engine for a cell (RFC-0005 Section 10).
 * Returns true if the engine passes all hard constraints.
 */
static bool engine_feasible(struct anx_cell *cell,
			    struct anx_engine *engine,
			    char *reason, size_t reason_len)
{
	/* Must be available or degraded */
	if (engine->status == ANX_ENGINE_OFFLINE ||
	    engine->status == ANX_ENGINE_MAINTENANCE) {
		anx_strlcpy(reason, "engine unavailable", reason_len);
		return false;
	}

	/* Network constraint */
	if (cell->constraints.locality == ANX_LOCAL_ONLY &&
	    !engine->is_local) {
		anx_strlcpy(reason, "locality: local only", reason_len);
		return false;
	}

	/* Network requirement */
	if (engine->requires_network &&
	    !cell->execution.allow_network) {
		anx_strlcpy(reason, "network access denied", reason_len);
		return false;
	}

	/* Remote model check */
	if (engine->engine_class == ANX_ENGINE_REMOTE_MODEL &&
	    !cell->execution.allow_remote_models) {
		anx_strlcpy(reason, "remote models denied", reason_len);
		return false;
	}

	return true;
}

int anx_route_plan(struct anx_cell *cell, struct anx_route_result *result)
{
	struct anx_engine *engines[ANX_MAX_ROUTE_CANDIDATES];
	uint32_t found = 0;
	uint32_t i;
	int32_t best_score = -1;
	uint32_t best_idx = 0;

	if (!cell || !result)
		return ANX_EINVAL;

	anx_memset(result, 0, sizeof(*result));

	/*
	 * Search across all engine classes for matching engines.
	 * In a full implementation, the cell type and intent would
	 * narrow which classes to search. For now, search all.
	 */
	for (i = 0; i < ANX_ENGINE_CLASS_COUNT && found < ANX_MAX_ROUTE_CANDIDATES; i++) {
		struct anx_engine *class_results[ANX_MAX_ROUTE_CANDIDATES];
		uint32_t class_found = 0;
		uint32_t j;
		uint32_t remaining = ANX_MAX_ROUTE_CANDIDATES - found;

		anx_engine_find((enum anx_engine_class)i, 0,
				class_results, remaining, &class_found);

		for (j = 0; j < class_found && found < ANX_MAX_ROUTE_CANDIDATES; j++)
			engines[found++] = class_results[j];
	}

	/* Score and filter candidates */
	for (i = 0; i < found; i++) {
		struct anx_route_candidate *cand;

		cand = &result->candidates[result->candidate_count];
		cand->engine_id = engines[i]->eid;

		/* Feasibility check */
		cand->feasible = engine_feasible(cell, engines[i],
						 cand->reason,
						 sizeof(cand->reason));

		if (cand->feasible) {
			cand->score = anx_route_score_engine(cell, engines[i]);
			if (cand->score > best_score) {
				best_score = cand->score;
				best_idx = result->candidate_count;
			}
		} else {
			cand->score = -1;
		}

		result->candidate_count++;
	}

	result->selected_index = best_idx;
	result->decided_at = ANX_ROUTE_STAGE_KERNEL;
	result->needs_escalation = false;

	if (result->candidate_count == 0)
		return ANX_ENOENT;

	/* Check that at least one candidate is feasible */
	{
		bool any_feasible = false;
		for (i = 0; i < result->candidate_count; i++) {
			if (result->candidates[i].feasible) {
				any_feasible = true;
				break;
			}
		}
		if (!any_feasible)
			return ANX_EPERM;
	}

	/*
	 * Escalation heuristic: if the best score is low, or the
	 * top two candidates are ambiguous (within 5 points), mark
	 * for escalation to the local routing service.
	 */
	if (best_score < ANX_ROUTE_ESCALATION_THRESHOLD)
		result->needs_escalation = true;

	if (result->candidate_count >= 2) {
		int32_t second_best = -1;

		for (i = 0; i < result->candidate_count; i++) {
			if (i != best_idx && result->candidates[i].feasible &&
			    result->candidates[i].score > second_best)
				second_best = result->candidates[i].score;
		}
		if (second_best >= 0 &&
		    best_score - second_best <= 5)
			result->needs_escalation = true;
	}

	return ANX_OK;
}
