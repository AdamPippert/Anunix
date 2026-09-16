/* The live planner uses an issued profile only inside its recorded envelope. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/route_profile.h>
#include <anx/route.h>
#include <anx/external_call.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct profile_context { struct anx_cell *cell; struct anx_route_weight_policy weights; struct anx_route_profile_case input; };
static int profile_handler(struct anx_external_call *call, void *arg)
{
	struct profile_context *c = arg;
	struct anx_route_result route;
	anx_oid_t denied = ANX_UUID_NIL;
	(void)call;
	if (anx_route_profile_compile(&c->weights, &c->input, 1, &denied) != ANX_EPERM || !anx_uuid_is_nil(&denied) ||
	    anx_route_profile_release(&c->cell->routing.profile_oid) != ANX_EPERM ||
	    anx_route_plan(c->cell, &route) != ANX_OK || !route.profile_applied) return -4409;
	return ANX_OK;
}
static bool score_matches(const struct anx_route_result *route, struct anx_cell *cell, struct anx_engine *engine,
		const struct anx_route_weight_policy *weights)
{
	for (uint32_t i = 0; i < route->candidate_count; i++)
		if (!anx_uuid_compare(&route->candidates[i].engine_id, &engine->eid))
			return route->candidates[i].feasible && route->candidates[i].score == anx_route_score_with_policy(cell, engine, weights);
	return false;
}
int anx_research_day044(void)
{
	struct anx_route_tuning_state original, current;
	struct anx_route_weight_policy candidate;
	struct anx_route_profile_case input = { .strategy = ANX_ROUTE_LOCAL_FIRST, .locality = ANX_LOCAL_ONLY };
	struct anx_cell *cell = NULL;
	struct anx_engine *engine = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call *call = NULL;
	struct anx_state_object *object = NULL, *forged = NULL;
	struct anx_route_result route;
	struct anx_so_create_params p = {0};
	anx_oid_t profiles[2] = {0};
	uint64_t trial = 0;
	int ret = anx_route_tuning_snapshot(&original);
	if (ret != ANX_OK || original.trial_active) return ANX_EBUSY;
	candidate = original.weights; candidate.locality_bonus++;
	anx_strlcpy(intent.name, "research-day-044", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
	if (ret == ANX_OK) ret = anx_engine_register("research-day-044-target", ANX_ENGINE_DETERMINISTIC_TOOL, 0, &engine);
	if (ret != ANX_OK) goto out;
	engine->is_local = true; engine->status = ANX_ENGINE_AVAILABLE; engine->quality_score = 100;
	cell->routing.strategy = input.strategy; cell->constraints.locality = input.locality;
	ret = anx_route_profile_compile(&candidate, &input, 1, &profiles[0]);
	if (ret != ANX_OK) goto out;
	cell->routing.profile_oid = profiles[0];
	cell->routing.strategy = ANX_ROUTE_DIRECT;
	ret = -4401;
	if (anx_route_plan(cell, &route) != ANX_OK || route.profile_applied || route.profile_status != ANX_EBUSY ||
	    !score_matches(&route, cell, engine, &original.weights)) goto out;
	cell->routing.strategy = input.strategy;
	ret = -4402;
	if (anx_route_plan(cell, &route) != ANX_OK || !route.profile_applied || route.profile_status != ANX_OK ||
	    !score_matches(&route, cell, engine, &candidate)) goto out;
	anx_route_tuning_snapshot(&current);
	if (anx_memcmp(&current, &original, sizeof(current))) goto out;
	/* A matching-looking sealed object is not an issued profile. */
	object = anx_objstore_lookup(&profiles[0]);
	ret = -4403;
	if (!object || object->state != ANX_OBJ_SEALED) goto out;
	p.object_type = ANX_OBJ_STRUCTURED_DATA; p.schema_uri = ANX_ROUTE_PROFILE_SCHEMA; p.schema_version = "1";
	p.payload = object->payload; p.payload_size = object->payload_size;
	ret = anx_so_create(&p, &forged);
	if (ret == ANX_OK) ret = anx_so_seal(&forged->oid);
	if (ret != ANX_OK) goto out;
	cell->routing.profile_oid = forged->oid;
	ret = -4403;
	if (anx_route_plan(cell, &route) != ANX_OK || route.profile_applied || route.profile_status != ANX_EPERM ||
	    !score_matches(&route, cell, engine, &original.weights)) goto out;
	cell->routing.profile_oid = profiles[0];
	((uint8_t *)object->payload)[0] ^= 1;
	int corrupted = anx_route_plan(cell, &route);
	((uint8_t *)object->payload)[0] ^= 1;
	if (corrupted != ANX_OK || route.profile_applied || route.profile_status != ANX_EPERM) goto out;
	object->access_policy.rule_count = 1;
	object->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	object->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	ret = -4404;
	if (anx_route_plan(cell, &route) != ANX_OK || route.profile_applied || route.profile_status != ANX_EPERM) goto out;
	object->access_policy.rule_count = 0;
	engine->quality_score--;
	ret = -4405;
	if (anx_route_plan(cell, &route) != ANX_OK || route.profile_applied || route.profile_status != ANX_EBUSY ||
	    !score_matches(&route, cell, engine, &original.weights)) goto out;
	engine->quality_score++;
	if (anx_route_plan(cell, &route) != ANX_OK || !route.profile_applied) goto out;
	struct anx_route_tuning_action action = { .schema = 1, .expected_generation = original.generation, .weights = original.weights };
	ret = anx_route_tuning_begin(&action, &trial);
	if (ret != ANX_OK) goto out;
	ret = -4406;
	if (anx_route_plan(cell, &route) != ANX_OK || route.profile_applied || route.profile_status != ANX_EBUSY) goto out;
	ret = anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT); trial = 0;
	if (ret != ANX_OK) goto out;
	ret = -4406;
	if (anx_route_plan(cell, &route) != ANX_OK || route.profile_applied || route.profile_status != ANX_EBUSY) goto out;
	ret = anx_route_profile_compile(&candidate, &input, 1, &profiles[1]);
	if (ret != ANX_OK) goto out;
	cell->routing.profile_oid = profiles[1];
	struct profile_context context = {cell, candidate, input};
	ret = anx_external_register_handler("anxresearch044", profile_handler, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch044://profile", sizeof(call->endpoint));
	cell->ext_call = call; cell->execution.allow_side_effects = true;
	ret = anx_cell_run(cell);
	if (ret != ANX_OK) goto out;
	ret = anx_route_profile_release(&profiles[1]);
	if (ret != ANX_OK) goto out;
	ret = -4407;
	if (anx_route_plan(cell, &route) != ANX_OK || route.profile_applied || route.profile_status != ANX_EPERM ||
	    !score_matches(&route, cell, engine, &original.weights)) goto out;
	anx_route_tuning_snapshot(&current);
	if (current.trial_active || anx_memcmp(&current.weights, &original.weights, sizeof(current.weights))) goto out;
	ret = ANX_OK;
out:
	if (trial) anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	for (uint32_t i = 0; i < 2; i++) if (!anx_uuid_is_nil(&profiles[i])) {
		anx_route_profile_release(&profiles[i]); anx_so_delete(&profiles[i], false);
	}
	if (object) anx_objstore_release(object);
	if (forged) { anx_so_delete(&forged->oid, false); anx_objstore_release(forged); }
	if (cell) anx_cell_destroy(cell);
	if (engine) anx_engine_unregister(engine);
	if (call) anx_free(call);
	anx_external_unregister_handler("anxresearch044");
	return ret;
}
#endif
