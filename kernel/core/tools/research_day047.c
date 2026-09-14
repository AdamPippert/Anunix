/* Reject infeasible compiler budgets without publishing a policy. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/route_profile.h>
#include <anx/route.h>
#include <anx/state_object.h>
#include <anx/uuid.h>

int anx_research_day047(void)
{
	struct anx_route_weight_policy weights;
	struct anx_route_profile_case cases[2] = {
		{ .strategy = ANX_ROUTE_LOCAL_FIRST, .locality = ANX_LOCAL_ONLY },
		{ .strategy = ANX_ROUTE_COST_FIRST, .locality = ANX_LOCAL_ONLY },
	};
	struct anx_route_profile_budget budget = { .artifact_payload_bytes = sizeof(struct anx_route_profile) - 1,
		.compiler_scratch_bytes = ~(uint64_t)0, .simulation_calls = 4 };
	struct anx_engine *engine = NULL;
	anx_oid_t oid = ANX_UUID_NIL;
	int ret = anx_engine_register("research-day-047", ANX_ENGINE_LOCAL_MODEL, ANX_CAP_SUMMARIZATION, &engine);
	if (ret != ANX_OK) return ret;
	engine->is_local = true; engine->quality_score = 99; engine->status = ANX_ENGINE_AVAILABLE;
	anx_route_weight_policy_incumbent(&weights);
	ret = anx_route_profile_compile_bounded(&weights, cases, 2, &budget, &oid);
	if (ret != ANX_ENOMEM || !anx_uuid_is_nil(&oid)) ret = -4701;
	else ret = ANX_OK;
	if (!anx_uuid_is_nil(&oid)) { anx_route_profile_release(&oid); anx_so_delete(&oid, false); }
	anx_engine_unregister(engine);
	return ret;
}
#endif
