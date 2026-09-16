/* Typed live policy trials preserve feasibility and restore rejected changes. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/tuning.h>
#include <anx/route.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct tuning_context { struct anx_route_tuning_action action; uint64_t trial; };

static int tuning_handler(struct anx_external_call *call, void *context)
{
	struct tuning_context *state = context;
	uint64_t rejected = 123;
	(void)call;
	if (anx_route_tuning_begin(&state->action, &rejected) != ANX_EPERM || rejected != 123 ||
	    anx_route_tuning_finish(state->trial, ANX_ROUTE_TRIAL_ACCEPT) != ANX_EPERM)
		return -2110;
	return ANX_OK;
}

static int choose(struct anx_cell *cell, struct anx_engine **engines, int expected, bool negative)
{
	struct anx_route_session session = {0};
	struct anx_route_result result = {0};
	session.engine_count = 2;
	session.eligible_engines[0] = engines[0]->eid;
	session.eligible_engines[1] = engines[1]->eid;
	int ret = anx_route_plan_session(cell, &session, &result);
	if (expected < 0)
		return ret == ANX_EPERM ? ANX_OK : -2109;
	if (ret != ANX_OK || result.selected_index != (uint32_t)expected ||
	    (negative && result.candidates[result.selected_index].score >= 0))
		return -2108;
	return ANX_OK;
}

static int general_rank(struct anx_cell *cell)
{
	struct anx_route_result result;
	if (anx_route_plan(cell, &result) != ANX_OK)
		return -2111;
	struct anx_route_candidate *selected = &result.candidates[result.selected_index];
	if (!selected->feasible)
		return -2112;
	for (uint32_t i = 0; i < result.candidate_count; i++)
		if (result.candidates[i].feasible && result.candidates[i].score > selected->score)
			return -2113;
	return ANX_OK;
}

int anx_research_day021(void)
{
	struct anx_route_tuning_state original, current;
	struct anx_route_tuning_action action = {0};
	struct anx_engine *engines[2] = {0};
	struct anx_cell *cell = NULL, *caller = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct tuning_context context;
	uint64_t trial = 0, rejected = 123;
	int rc = anx_route_tuning_snapshot(&original);
	if (rc != ANX_OK || original.trial_active)
		return ANX_EBUSY;
	action.schema = 1;
	action.expected_generation = original.generation;
	action.weights = original.weights;
	action.weights.cpu_cost_divisor = 0;
	rc = -2101;
	if (anx_route_tuning_begin(&action, &rejected) != ANX_EINVAL || rejected != 123 ||
	    anx_route_tuning_snapshot(&current) != ANX_OK || anx_memcmp(&current, &original, sizeof(current)))
		goto out;
	action.weights = original.weights;
	action.schema = 2;
	if (anx_route_tuning_begin(&action, &rejected) != ANX_EINVAL || rejected != 123)
		goto out;
	action.schema = 1;
	rc = anx_engine_register("research-day-021-local", ANX_ENGINE_LOCAL_MODEL, 0, &engines[0]);
	if (rc != ANX_OK)
		goto out;
	rc = anx_engine_register("research-day-021-remote", ANX_ENGINE_REMOTE_MODEL, 0, &engines[1]);
	if (rc != ANX_OK)
		goto out;
	for (int i = 0; i < 2; i++) {
		engines[i]->status = ANX_ENGINE_AVAILABLE;
		engines[i]->quality_score = i ? 90 : 10;
		engines[i]->cpu_weight = engines[i]->gpu_weight = 0;
		engines[i]->is_local = i == 0;
		engines[i]->requires_network = i == 1;
		engines[i]->supports_private_data = false;
	}
	anx_strlcpy(intent.name, "research-day-021", sizeof(intent.name));
	rc = anx_cell_create(ANX_CELL_TASK_RETRIEVAL, &intent, &cell);
	if (rc != ANX_OK)
		goto out;
	cell->constraints.locality = ANX_REMOTE_ALLOWED;
	cell->routing.strategy = ANX_ROUTE_DIRECT;
	cell->execution.allow_network = cell->execution.allow_remote_models = true;
	rc = choose(cell, engines, 1, false);
	if (rc != ANX_OK)
		goto out;
	action.weights.locality_bonus = 1000;
	rc = -2102;
	if (anx_route_tuning_begin(&action, &trial) != ANX_OK ||
	    anx_route_tuning_begin(&action, &rejected) != ANX_EBUSY || rejected != 123 ||
	    anx_route_tuning_finish(trial + 1, ANX_ROUTE_TRIAL_REJECT) != ANX_EBUSY ||
	    anx_route_tuning_finish(trial, (enum anx_route_trial_result)2) != ANX_EINVAL)
		goto out;
	rc = choose(cell, engines, 0, false);
	if (rc != ANX_OK)
		goto out;
	cell->constraints.locality = ANX_REMOTE_REQUIRED;
	rc = choose(cell, engines, 1, false);
	if (rc != ANX_OK)
		goto out;
	cell->execution.allow_remote_models = false;
	rc = choose(cell, engines, -1, false);
	if (rc != ANX_OK)
		goto out;
	cell->execution.allow_remote_models = true;
	cell->constraints.locality = ANX_REMOTE_ALLOWED;
	context.action = action;
	context.trial = trial;
	rc = anx_external_register_handler("anxresearch021", tuning_handler, &context);
	if (rc != ANX_OK)
		goto out;
	call = anx_zalloc(sizeof(*call));
	rc = ANX_ENOMEM;
	if (!call)
		goto out;
	anx_strlcpy(call->endpoint, "anxresearch021://tune", sizeof(call->endpoint));
	rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (rc != ANX_OK)
		goto out;
	caller->execution.allow_side_effects = true;
	caller->ext_call = call;
	rc = anx_cell_run(caller);
	if (rc != ANX_OK)
		goto out;
	rc = -2103;
	if (anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT) != ANX_OK ||
	    anx_route_tuning_snapshot(&current) != ANX_OK || current.trial_active ||
	    anx_memcmp(&current.weights, &original.weights, sizeof(current.weights)) ||
	    current.generation != original.generation + 2 ||
	    anx_route_tuning_begin(&action, &rejected) != ANX_EBUSY || rejected != 123)
		goto out;
	trial = 0;
	rc = choose(cell, engines, 1, false);
	if (rc != ANX_OK)
		goto out;
	action.expected_generation = current.generation;
	rc = -2104;
	if (anx_route_tuning_begin(&action, &trial) != ANX_OK ||
	    anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_ACCEPT) != ANX_OK ||
	    anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT) != ANX_ENOENT ||
	    anx_route_tuning_snapshot(&current) != ANX_OK || current.trial_active ||
	    anx_memcmp(&current.weights, &action.weights, sizeof(current.weights)))
		goto out;
	trial = 0;
	rc = choose(cell, engines, 0, false);
	if (rc != ANX_OK)
		goto out;
	action.expected_generation = current.generation;
	action.weights = original.weights;
	action.weights.locality_bonus = action.weights.local_first_bonus = action.weights.private_data_bonus = 0;
	action.weights.degraded_penalty = -1000;
	action.weights.cpu_cost_divisor = action.weights.gpu_cost_divisor = 1;
	for (int i = 0; i < 2; i++) {
		engines[i]->status = ANX_ENGINE_DEGRADED;
		engines[i]->cpu_weight = engines[i]->gpu_weight = 100;
	}
	rc = anx_route_tuning_begin(&action, &trial);
	if (rc != ANX_OK)
		goto out;
	rc = choose(cell, engines, 1, true);
	if (rc != ANX_OK)
		goto out;
	rc = general_rank(cell);
	if (rc != ANX_OK)
		goto out;
	engines[0]->quality_score = 90;
	engines[1]->quality_score = 10;
	rc = general_rank(cell);
out:
	if (trial)
		anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	/* Restore the original policy after the accepted trial as well. */
	if (anx_route_tuning_snapshot(&current) == ANX_OK && !current.trial_active &&
	    anx_memcmp(&current.weights, &original.weights, sizeof(current.weights))) {
		action.schema = 1;
		action.expected_generation = current.generation;
		action.weights = original.weights;
		if (anx_route_tuning_begin(&action, &trial) != ANX_OK ||
		    anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_ACCEPT) != ANX_OK)
			rc = -2114;
	}
	if (caller)
		anx_cell_destroy(caller);
	if (cell)
		anx_cell_destroy(cell);
	if (call)
		anx_free(call);
	anx_external_unregister_handler("anxresearch021");
	for (int i = 0; i < 2; i++)
		if (engines[i])
			anx_engine_unregister(engines[i]);
	return rc;
}
#endif
