/* Reject infeasible compiler budgets without publishing a policy. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/route_profile.h>
#include <anx/route.h>
#include <anx/state_object.h>
#include <anx/uuid.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>

struct compiler_context {
	struct anx_route_weight_policy weights;
	struct anx_route_profile_case *cases;
	struct anx_route_profile_budget budget;
};
static int active_compile(struct anx_external_call *call, void *arg)
{
	struct compiler_context *c = arg;
	anx_oid_t denied = ANX_UUID_NIL;
	(void)call;
	return anx_route_profile_compile_bounded(&c->weights, c->cases, 2, &c->budget, &denied) == ANX_EPERM &&
		anx_uuid_is_nil(&denied) ? ANX_OK : -4709;
}

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
	struct anx_route_profile_budget required, invalid;
	struct anx_route_tuning_state before, after;
	struct anx_state_object *object = NULL;
	struct anx_cell *caller = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call *call = NULL;
	struct compiler_context context;
	anx_oid_t oid = ANX_UUID_NIL;
	int ret = anx_engine_register("research-day-047", ANX_ENGINE_LOCAL_MODEL, ANX_CAP_SUMMARIZATION, &engine);
	if (ret != ANX_OK) return ret;
	engine->is_local = true; engine->quality_score = 99; engine->status = ANX_ENGINE_AVAILABLE;
	anx_route_weight_policy_incumbent(&weights);
	anx_route_tuning_snapshot(&before);
	ret = anx_route_profile_compile_bounded(&weights, cases, 2, &budget, &oid);
	if (ret != ANX_ENOMEM || !anx_uuid_is_nil(&oid)) { ret = -4701; goto out; }
	ret = anx_route_profile_requirements(2, &required);
	if (ret != ANX_OK) goto out;
	ret = -4702;
	if (required.artifact_payload_bytes != sizeof(struct anx_route_profile) || required.simulation_calls != 4 ||
	    required.compiler_scratch_bytes <= required.artifact_payload_bytes) goto out;
	budget = required; budget.compiler_scratch_bytes--;
	if (anx_route_profile_compile_bounded(&weights, cases, 2, &budget, &oid) != ANX_ENOMEM || !anx_uuid_is_nil(&oid)) goto out;
	budget = required; budget.simulation_calls--;
	ret = -4703;
	if (anx_route_profile_compile_bounded(&weights, cases, 2, &budget, &oid) != ANX_EFULL || !anx_uuid_is_nil(&oid)) goto out;
	/* Cheap feasibility rejects before policy validation or any simulation. */
	budget = required; budget.artifact_payload_bytes = 0;
	weights.gpu_cost_divisor = 0;
	ret = -4704;
	if (anx_route_profile_compile_bounded(&weights, cases, 2, &budget, &oid) != ANX_ENOMEM || !anx_uuid_is_nil(&oid)) goto out;
	budget = required;
	if (anx_route_profile_compile_bounded(&weights, cases, 2, &budget, &oid) != ANX_EINVAL || !anx_uuid_is_nil(&oid)) goto out;
	anx_route_weight_policy_incumbent(&weights);
	invalid = required;
	ret = -4705;
	if (anx_route_profile_requirements(0, &invalid) != ANX_EINVAL ||
	    anx_route_profile_requirements(~(uint32_t)0, &invalid) != ANX_EINVAL ||
	    invalid.artifact_payload_bytes != required.artifact_payload_bytes ||
	    invalid.compiler_scratch_bytes != required.compiler_scratch_bytes || invalid.simulation_calls != required.simulation_calls ||
	    anx_route_profile_requirements(2, NULL) != ANX_EINVAL ||
	    anx_route_profile_compile_bounded(&weights, cases, 2, NULL, &oid) != ANX_EINVAL ||
	    anx_route_profile_compile_bounded(&weights, cases, 9, &budget, &oid) != ANX_EINVAL || !anx_uuid_is_nil(&oid)) goto out;
	ret = anx_route_profile_compile_bounded(&weights, cases, 2, &required, &oid);
	if (ret != ANX_OK) goto out;
	object = anx_objstore_lookup(&oid);
	ret = -4706;
	if (!object || object->state != ANX_OBJ_SEALED || object->payload_size != required.artifact_payload_bytes) goto out;
	anx_objstore_release(object); object = NULL;
	anx_route_tuning_snapshot(&after);
	ret = -4707;
	if (anx_memcmp(&before, &after, sizeof(before))) goto out;
	/* The accepted artifact still follows ordinary runtime selection. */
	caller = anx_zalloc(sizeof(*caller));
	if (!caller) { ret = ANX_ENOMEM; goto out; }
	caller->routing.strategy = cases[0].strategy; caller->constraints.locality = cases[0].locality;
	struct anx_route_weight_policy chosen = {0};
	ret = anx_route_profile_choose(&oid, caller, &after, &chosen);
	anx_free(caller); caller = NULL;
	if (ret != ANX_OK) goto out;
	ret = -4708;
	if (anx_memcmp(&weights, &chosen, sizeof(weights))) goto out;
	context = (struct compiler_context){ weights, cases, required };
	anx_strlcpy(intent.name, "research-day-047-caller", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch047", active_compile, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	if (!call) { ret = ANX_ENOMEM; goto out; }
	anx_strlcpy(call->endpoint, "anxresearch047://compile", sizeof(call->endpoint));
	caller->ext_call = call; caller->execution.allow_side_effects = true;
	ret = anx_cell_run(caller);
out:
	if (object) anx_objstore_release(object);
	anx_external_unregister_handler("anxresearch047");
	if (caller) anx_cell_destroy(caller);
	anx_free(call);
	if (!anx_uuid_is_nil(&oid)) { anx_route_profile_release(&oid); anx_so_delete(&oid, false); }
	anx_engine_unregister(engine);
	return ret;
}
#endif
