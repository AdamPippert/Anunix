/* Candidate policy validation must preserve a prior simulation result. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/twin.h>
#include <anx/alloc.h>
#include <anx/string.h>

int anx_research_day013(void)
{
	struct anx_resource_twin *twin = anx_zalloc(sizeof(*twin));
	struct anx_resource_twin *bad = anx_zalloc(sizeof(*bad));
	struct anx_cell *cell = anx_zalloc(sizeof(*cell));
	struct anx_route_weight_policy policy, candidate, before;
	struct anx_twin_simulate_result result, saved;
	uint32_t i;
	int rc = ANX_ENOMEM;

	if (!twin || !bad || !cell)
		goto out;
	twin->engine_count = 2;
	for (i = 0; i < 2; i++) {
		twin->engines[i].engine_class = ANX_ENGINE_RETRIEVAL_SERVICE;
		twin->engines[i].status = ANX_ENGINE_AVAILABLE;
		twin->engines[i].readiness = ANX_READY_HEALTHY;
		twin->engines[i].quality_score = 80 + 10 * i;
	}
	twin->engines[0].is_local = true;
	cell->constraints.locality = ANX_REMOTE_ALLOWED;
	anx_route_weight_policy_incumbent(&policy);
	rc = -1300;
	if (anx_twin_simulate(twin, cell, &policy, &result) != ANX_OK ||
	    result.winner_index != 0 || result.margin != 10)
		goto out;
	saved = result;
	for (i = 0; i < 20; i++) {
		candidate = policy;
		*bad = *twin;
		cell->constraints.topology_bk_set = false;
		cell->constraints.locality = ANX_REMOTE_ALLOWED;
		cell->routing.strategy = ANX_ROUTE_DIRECT;
		switch (i) {
		case 0: candidate.gpu_cost_divisor = -1; break;
		case 1: candidate.cpu_cost_divisor = 0; break;
		case 2: candidate.locality_bonus = 2147483647; break;
		case 3: candidate.degraded_penalty = -2147483647 - 1; break;
		case 4: candidate.cpu_cost_divisor = 1001; break;
		case 5: candidate.local_first_bonus = -1; break;
		case 6: candidate.private_data_bonus = 1001; break;
		case 7: candidate.topology_overlap_bonus = -1; break;
		case 8: candidate.topology_mismatch_penalty = 1; break;
		case 9: bad->engine_count = ANX_TWIN_MAX_ENGINES + 1; break;
		case 10: bad->engines[0].quality_score = 101; break;
		case 11: bad->engines[0].cpu_weight = 101; break;
		case 12: bad->engines[0].gpu_weight = 0xffffffffU; break;
		case 13: bad->engines[0].status = ANX_ENGINE_STATUS_COUNT; break;
		case 14: bad->engines[0].engine_class = ANX_ENGINE_CLASS_COUNT; break;
		case 15: bad->engines[0].readiness = ANX_READY_NONE; break;
		case 16:
			bad->engines[0].has_topology_affinity = true;
			bad->engines[0].topology_bk_lo = 1;
			break;
		case 17: cell->constraints.locality = (enum anx_locality)99; break;
		case 18: cell->routing.strategy = (enum anx_routing_strategy)99; break;
		case 19:
			cell->constraints.topology_bk_set = true;
			cell->constraints.topology_bk_lo = 1;
			break;
		}
		before = candidate;
		rc = -1301 - (int)i;
		if (anx_twin_simulate(bad, cell, &candidate, &result) != ANX_EINVAL ||
		    anx_memcmp(&result, &saved, sizeof(result)) != 0 ||
		    anx_memcmp(&candidate, &before, sizeof(candidate)) != 0)
			goto out;
	}
	cell->constraints.topology_bk_set = false;
	candidate = policy;
	candidate.locality_bonus = 0;
	rc = -1321;
	if (anx_twin_simulate(twin, cell, &candidate, &result) != ANX_OK ||
	    result.winner_index != 1 || result.margin != 10)
		goto out;
	rc = -1322;
	if (anx_twin_simulate(twin, cell, &policy, &result) != ANX_OK ||
	    anx_memcmp(&result, &saved, sizeof(result)) != 0)
		goto out;
	rc = ANX_OK;
out:
	anx_free(cell);
	anx_free(bad);
	anx_free(twin);
	return rc;
}
#endif
