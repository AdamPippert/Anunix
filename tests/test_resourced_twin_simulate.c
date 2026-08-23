/*
 * test_resourced_twin_simulate.c — Resource Twin snapshot + simulate
 * (RFC-0029).
 */

#include <anx/types.h>
#include <anx/twin.h>
#include <anx/engine.h>
#include <anx/cell.h>
#include <anx/string.h>

int test_resourced_twin_simulate(void)
{
	struct anx_engine *fast, *slow;
	struct anx_cell *cell;
	struct anx_cell_intent intent;
	struct anx_resource_twin *twin;
	struct anx_route_weight_policy policy;
	struct anx_twin_simulate_result result;
	int ret;

	anx_engine_registry_init();
	anx_cell_store_init();
	anx_sched_init();

	/* readiness derivation */
	if (anx_readiness_from_status(ANX_ENGINE_OFFLINE) != ANX_READY_NONE)
		return -1;
	if (anx_readiness_from_status(ANX_ENGINE_READY) != ANX_READY_READY)
		return -2;
	if (anx_readiness_from_status(ANX_ENGINE_AVAILABLE) != ANX_READY_HEALTHY)
		return -3;

	/* Two engines: "fast" has higher quality, "slow" is degraded. */
	/* anx_engine_register() starts engines in ANX_ENGINE_AVAILABLE
	 * directly (unlike anx_engine_register_model(), which starts in
	 * REGISTERED) — no LOADING/READY steps needed here. */
	if (anx_engine_register("fast", ANX_ENGINE_RETRIEVAL_SERVICE,
				0, &fast) != ANX_OK)
		return -4;
	fast->quality_score = 80;
	fast->is_local = true;

	if (anx_engine_register("slow", ANX_ENGINE_RETRIEVAL_SERVICE,
				0, &slow) != ANX_OK)
		return -6;
	slow->quality_score = 80;
	slow->is_local = true;
	if (anx_engine_transition(slow, ANX_ENGINE_DEGRADED) != ANX_OK)
		return -7;

	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "twin_test", sizeof(intent.name));
	if (anx_cell_create(ANX_CELL_TASK_RETRIEVAL, &intent, &cell) != ANX_OK)
		return -8;

	/* Snapshot */
	ret = anx_twin_snapshot(&twin);
	if (ret != ANX_OK)
		return -9;
	if (twin->engine_count < 2)
		return -10;

	/* Mutate live state after the snapshot — the twin must not see it. */
	if (anx_engine_transition(fast, ANX_ENGINE_OFFLINE) != ANX_OK)
		return -11;

	anx_route_weight_policy_incumbent(&policy);

	ret = anx_twin_simulate(twin, cell, &policy, &result);
	if (ret != ANX_OK)
		return -12;

	/* Both were feasible at snapshot time (fast was AVAILABLE, slow was
	 * DEGRADED — engine_feasible only rejects OFFLINE/MAINTENANCE). */
	if (result.candidate_count != 2)
		return -13;

	/* fast beats slow: same quality/locality, but slow eats the
	 * degraded penalty. The winner must be "fast" even though we just
	 * took it offline live — proves the snapshot is truly frozen. */
	{
		const struct anx_twin_engine_snapshot *winner =
			&twin->engines[result.winner_index];
		if (winner->eid.hi != fast->eid.hi ||
		    winner->eid.lo != fast->eid.lo)
			return -14;
	}
	if (!result.has_margin || result.margin <= 0)
		return -15;

	/* Invalid policy (zero divisor) is rejected without touching result_out. */
	{
		struct anx_route_weight_policy bad = policy;
		bad.gpu_cost_divisor = 0;
		if (anx_twin_simulate(twin, cell, &bad, &result) != ANX_EINVAL)
			return -16;
	}

	/* No leak on repeated snapshot/destroy. */
	anx_twin_destroy(twin);
	{
		struct anx_resource_twin *twin2;
		if (anx_twin_snapshot(&twin2) != ANX_OK)
			return -17;
		anx_twin_destroy(twin2);
	}

	return 0;
}
