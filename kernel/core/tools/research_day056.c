/* Frozen planning separates retained bytes from restoration working capacity. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/twin.h>
#include <anx/string.h>
#include <anx/route.h>
#include <anx/uuid.h>
int anx_research_day056(void)
{
	struct anx_cell *cell = NULL;
	struct anx_engine *engine = NULL;
	struct anx_engine_lease *resident = NULL, *rival = NULL, *admitted = NULL;
	anx_eid_t ids[3];
	struct anx_resource_twin *fresh = NULL;
	uint64_t available, memory_before;
	uint32_t percent_before;
	anx_lease_avail_mem(ANX_MEM_L1, &memory_before);
	anx_lease_avail_accel(ANX_ACCEL_GPU, &percent_before);
	struct anx_cell_intent intent = {0};
	struct anx_resource_twin *twin = NULL;
	struct anx_route_weight_policy policy;
	struct anx_twin_restore_request request = { .tier = ANX_MEM_L1,
		.retained_bytes = 64ULL * 1024 * 1024 * 1024, .resident_bytes = 0,
		.restore_peak_bytes = 512ULL * 1024 * 1024, .accelerator = ANX_ACCEL_NONE };
	struct anx_twin_restore_result result;
	anx_strlcpy(intent.name, "research-day-056", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &cell);
	if (ret == ANX_OK) ret = anx_twin_snapshot(&twin);
	if (ret != ANX_OK) goto out;
	anx_route_weight_policy_incumbent(&policy);
	ret = -5601;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &request, &result) != ANX_OK ||
	    result.additional_memory_bytes != request.restore_peak_bytes) goto out;
	for (uint32_t i = 0; i < 3; i++) anx_uuid_generate(&ids[i]);
	ret = anx_engine_register("research-day-056-target", ANX_ENGINE_DETERMINISTIC_TOOL, 0, &engine);
	if (ret != ANX_OK) goto out;
	engine->is_local = true; engine->status = ANX_ENGINE_AVAILABLE; engine->quality_score = 100;
	engine->cpu_weight = engine->gpu_weight = 0; engine->supports_private_data = true;
	engine->has_topology_affinity = true; engine->topology_bk_lo = engine->topology_bk_hi = 42;
	cell->constraints.locality = ANX_LOCAL_ONLY; cell->routing.strategy = ANX_ROUTE_LOCAL_FIRST;
	cell->constraints.topology_bk_set = true; cell->constraints.topology_bk_lo = cell->constraints.topology_bk_hi = 42;
	request.resident_bytes = 64ULL * 1024 * 1024;
	request.accelerator = ANX_ACCEL_GPU; request.accelerator_pct = 20;
	ret = anx_lease_grant(&ids[0], ANX_MEM_L1, request.resident_bytes, ANX_ACCEL_NONE, 0, &resident);
	if (ret != ANX_OK) goto out;
	anx_twin_destroy(twin); twin = NULL;
	ret = anx_twin_snapshot(&twin);
	if (ret == ANX_OK) ret = anx_twin_simulate_restoration(twin, cell, &policy, &request, &result);
	if (ret != ANX_OK) goto out;
	uint64_t required = request.restore_peak_bytes - request.resident_bytes;
	struct anx_route_result live;
	ret = -5602;
	if (result.additional_memory_bytes != required || !result.route.candidate_count ||
	    anx_uuid_compare(&twin->engines[result.route.winner_index].eid, &engine->eid) ||
	    result.route.winner_score != anx_route_score_with_policy(cell, engine, &policy) ||
	    anx_route_plan(cell, &live) != ANX_OK || !live.candidate_count ||
	    anx_uuid_compare(&live.candidates[live.selected_index].engine_id, &engine->eid) ||
	    live.candidates[live.selected_index].score != result.route.winner_score ||
	    twin->capacity.free_memory[ANX_MEM_L1] != memory_before - request.resident_bytes) goto out;
	struct anx_twin_restore_result frozen = result;
	engine->quality_score = 0;
	ret = -5603;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &request, &result) != ANX_OK ||
	    anx_memcmp(&result, &frozen, sizeof(result)) ||
	    anx_route_score_with_policy(cell, engine, &policy) != frozen.route.winner_score - 100) goto out;
	engine->quality_score = 100;
	/* Leave one byte less than restoration needs after the resident reservation. */
	available = twin->capacity.free_memory[ANX_MEM_L1];
	ret = anx_lease_grant(&ids[1], ANX_MEM_L1, available - required + 1, ANX_ACCEL_NONE, 0, &rival);
	if (ret == ANX_OK) ret = anx_twin_snapshot(&fresh);
	if (ret != ANX_OK) goto out;
	struct anx_twin_restore_result sentinel;
	anx_memset(&sentinel, 0x55, sizeof(sentinel));
	struct anx_twin_restore_result untouched = sentinel;
	ret = -5604;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &request, &result) != ANX_OK ||
	    anx_memcmp(&result, &frozen, sizeof(result)) ||
	    anx_twin_simulate_restoration(fresh, cell, &policy, &request, &sentinel) != ANX_ENOMEM ||
	    anx_lease_grant(&ids[2], ANX_MEM_L1, required, ANX_ACCEL_GPU, 20, &admitted) != ANX_ENOMEM || admitted ||
	    anx_memcmp(&sentinel, &untouched, sizeof(sentinel))) goto out;
	ret = anx_lease_release(rival); rival = NULL;
	anx_twin_destroy(fresh); fresh = NULL;
	if (ret == ANX_OK) ret = anx_lease_grant(&ids[1], ANX_MEM_L1, 0, ANX_ACCEL_GPU, percent_before - 19, &rival);
	if (ret == ANX_OK) ret = anx_twin_snapshot(&fresh);
	if (ret != ANX_OK) goto out;
	ret = -5605;
	if (anx_twin_simulate_restoration(fresh, cell, &policy, &request, &sentinel) != ANX_ENOMEM ||
	    anx_lease_grant(&ids[2], ANX_MEM_L1, required, ANX_ACCEL_GPU, 20, &admitted) != ANX_ENOMEM || admitted) goto out;
	ret = anx_lease_release(rival); rival = NULL;
	if (ret == ANX_OK) ret = anx_lease_grant(&ids[2], ANX_MEM_L1, required, ANX_ACCEL_GPU, 20, &admitted);
	if (ret != ANX_OK) goto out;
	ret = anx_lease_release(admitted); admitted = NULL;
	if (ret != ANX_OK) goto out;
	/* Absent or malformed evidence abstains without changing the caller's result. */
	struct anx_lease_capacity capacity = twin->capacity;
	twin->capacity.schema = 0;
	ret = -5606;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &request, &sentinel) != ANX_ENOTSUP) goto out;
	twin->capacity = capacity; twin->capacity.free_memory[ANX_MEM_L1] = capacity.total_memory[ANX_MEM_L1] + 1;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &request, &sentinel) != ANX_EINVAL) goto out;
	twin->capacity = capacity;
	struct anx_twin_restore_request bad = request;
	bad.restore_peak_bytes = bad.resident_bytes - 1;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &bad, &sentinel) != ANX_EINVAL) goto out;
	bad = request; bad.retained_bytes = bad.resident_bytes - 1;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &bad, &sentinel) != ANX_EINVAL) goto out;
	bad = request; bad.tier = (enum anx_mem_tier)-1;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &bad, &sentinel) != ANX_EINVAL) goto out;
	bad = request; bad.accelerator = ANX_ACCEL_NONE;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &bad, &sentinel) != ANX_EINVAL) goto out;
	bad = request; bad.restore_peak_bytes = ~(uint64_t)0;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &bad, &sentinel) != ANX_ENOMEM ||
	    anx_memcmp(&sentinel, &untouched, sizeof(sentinel))) goto out;
	bad = request; bad.restore_peak_bytes = bad.resident_bytes + capacity.free_memory[ANX_MEM_L1];
	ret = anx_twin_simulate_restoration(twin, cell, &policy, &bad, &result);
	if (ret != ANX_OK) goto out;
	ret = -5607;
	if (result.additional_memory_bytes != capacity.free_memory[ANX_MEM_L1]) goto out;
	struct anx_route_weight_policy after;
	anx_route_weight_policy_incumbent(&after);
	if (anx_memcmp(&after, &policy, sizeof(policy))) goto out;
	ret = ANX_OK;
out:
	if (admitted) anx_lease_release(admitted);
	if (rival) anx_lease_release(rival);
	if (resident) anx_lease_release(resident);
	anx_twin_destroy(fresh);
	if (engine) anx_engine_unregister(engine);
	anx_lease_avail_mem(ANX_MEM_L1, &available);
	uint32_t percent;
	anx_lease_avail_accel(ANX_ACCEL_GPU, &percent);
	if (ret == ANX_OK && (available != memory_before || percent != percent_before)) ret = -5608;
	anx_twin_destroy(twin);
	if (cell) anx_cell_destroy(cell);
	return ret;
}
#endif
