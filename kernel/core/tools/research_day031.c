/* A caller-authored pass record must not authorize an optimization trial. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/tuning.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/optimization_harness.h>
#include <anx/external_call.h>
#include <anx/alloc.h>

struct harness_context {
	struct anx_route_tuning_action action;
	anx_oid_t receipt;
};

static int harness_handler(struct anx_external_call *call, void *arg)
{
	struct harness_context *context = arg;
	anx_oid_t result = ANX_UUID_NIL;
	(void)call;
	if (anx_route_evaluate(&context->action, &result) != ANX_EPERM || !anx_uuid_is_nil(&result) ||
	    anx_route_evaluation_release(&context->receipt) != ANX_EPERM)
		return -3110;
	return ANX_OK;
}

int anx_research_day031(void)
{
	struct anx_state_object *knowledge = NULL, *claimed = NULL;
	struct anx_state_object *forged = NULL;
	struct anx_so_create_params params = {0};
	struct anx_route_tuning_state original;
	struct anx_route_tuning_action action = {0};
	struct anx_route_tuning_action changed;
	struct anx_route_evaluation report;
	struct anx_object_handle handle = {0};
	struct anx_cell *caller = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct harness_context context;
	struct anx_engine *engine = NULL;
	anx_oid_t artifact = ANX_UUID_NIL;
	anx_oid_t receipts[3] = {0}, artifacts[5] = {0};
	uint64_t trial = 0;
	uint64_t sentinel = 123;
	int ret = anx_route_tuning_snapshot(&original);
	if (ret != ANX_OK || original.trial_active) return ANX_EBUSY;
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = "target";
	params.payload_size = 6;
	ret = anx_so_create(&params, &knowledge);
	if (ret == ANX_OK) ret = anx_so_seal(&knowledge->oid);
	params.payload = "all checks passed";
	params.payload_size = 17;
	if (ret == ANX_OK) ret = anx_so_create(&params, &claimed);
	if (ret == ANX_OK) ret = anx_so_seal(&claimed->oid);
	if (ret == ANX_OK) ret = anx_engine_register("research-day-031-target", ANX_ENGINE_LOCAL_MODEL, 0, &engine);
	if (ret != ANX_OK) goto out;
	action.schema = 1;
	action.expected_generation = original.generation;
	action.weights = original.weights;
	action.weights.locality_bonus = 1000;
	ret = anx_route_target_capture(&engine->eid, &knowledge->oid, &action.target);
	if (ret == ANX_OK) ret = anx_route_tuning_artifact_create(&action, &claimed->oid, &artifact);
	if (ret != ANX_OK) goto out;
	ret = -3101;
	if (anx_route_tuning_begin_artifact(&artifact, &trial) != ANX_EPERM || trial != 0) goto out;
	ret = anx_route_evaluate(&action, &receipts[0]);
	if (ret == ANX_OK) ret = anx_so_open(&receipts[0], ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) goto out;
	ret = -3102;
	if (anx_so_read_payload(&handle, 0, &report, sizeof(report)) != (int)sizeof(report) ||
	    report.schema != 1 || report.case_count != ANX_ROUTE_HARNESS_CASES || report.failure_count)
		goto out;
	anx_so_close(&handle);
	for (uint32_t i = 0; i < ANX_ROUTE_HARNESS_CASES; i++)
		if (report.observed_winner[i] != 0) goto out;
	params.object_type = ANX_OBJ_STRUCTURED_DATA;
	params.schema_uri = ANX_ROUTE_HARNESS_SCHEMA;
	params.schema_version = "1";
	params.payload = &report;
	params.payload_size = sizeof(report);
	ret = anx_so_create(&params, &forged);
	if (ret == ANX_OK) ret = anx_so_seal(&forged->oid);
	if (ret == ANX_OK) ret = anx_route_tuning_artifact_create(&action, &forged->oid, &artifacts[0]);
	if (ret != ANX_OK) goto out;
	ret = -3103;
	if (anx_route_tuning_begin_artifact(&artifacts[0], &sentinel) != ANX_EPERM || sentinel != 123) goto out;
	changed = action;
	changed.weights.locality_bonus = 999;
	ret = anx_route_tuning_artifact_create(&changed, &receipts[0], &artifacts[1]);
	if (ret != ANX_OK) goto out;
	ret = -3104;
	if (anx_route_tuning_begin_artifact(&artifacts[1], &sentinel) != ANX_EPERM || sentinel != 123) goto out;
	changed = action;
	changed.weights.locality_bonus = changed.weights.local_first_bonus = 0;
	ret = anx_route_evaluate(&changed, &receipts[1]);
	if (ret == ANX_OK) ret = anx_route_tuning_artifact_create(&changed, &receipts[1], &artifacts[2]);
	if (ret == ANX_OK) ret = anx_so_open(&receipts[1], ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) goto out;
	ret = -3105;
	if (anx_so_read_payload(&handle, 0, &report, sizeof(report)) != (int)sizeof(report) ||
	    report.failure_count != 1 || report.observed_winner[0] != 1 ||
	    anx_route_tuning_begin_artifact(&artifacts[2], &sentinel) != ANX_EPERM || sentinel != 123)
		goto out;
	anx_so_close(&handle);
	context.action = action;
	context.receipt = receipts[0];
	ret = anx_external_register_handler("anxresearch031", harness_handler, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch031://evaluate", sizeof(call->endpoint));
	anx_strlcpy(intent.name, "research-day-031-untrusted", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (ret != ANX_OK) goto out;
	caller->ext_call = call;
	caller->execution.allow_side_effects = true;
	ret = anx_cell_run(caller);
	if (ret == ANX_OK) ret = anx_route_tuning_artifact_create(&action, &receipts[0], &artifacts[3]);
	if (ret == ANX_OK) ret = anx_route_tuning_begin_artifact(&artifacts[3], &trial);
	if (ret == ANX_OK) ret = anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	if (ret != ANX_OK) goto out;
	trial = 0;
	ret = anx_route_tuning_snapshot(&original);
	if (ret != ANX_OK) goto out;
	action.expected_generation = original.generation;
	ret = anx_route_evaluate(&action, &receipts[2]);
	if (ret == ANX_OK) ret = anx_route_tuning_artifact_create(&action, &receipts[2], &artifacts[4]);
	if (ret == ANX_OK) ret = anx_route_evaluation_release(&receipts[2]);
	if (ret != ANX_OK) goto out;
	ret = -3106;
	if (anx_route_tuning_begin_artifact(&artifacts[4], &sentinel) != ANX_EPERM || sentinel != 123 ||
	    anx_so_delete(&claimed->oid, false) != ANX_OK ||
	    anx_route_tuning_begin_artifact(&artifact, &sentinel) != ANX_ENOENT || sentinel != 123)
		goto out;
	ret = ANX_OK;
out:
	if (handle.obj) anx_so_close(&handle);
	if (trial) anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	if (caller) anx_cell_destroy(caller);
	if (call) anx_free(call);
	anx_external_unregister_handler("anxresearch031");
	for (uint32_t i = 0; i < 5; i++)
		if (!anx_uuid_is_nil(&artifacts[i])) anx_so_delete(&artifacts[i], false);
	for (uint32_t i = 0; i < 3; i++) {
		if (anx_uuid_is_nil(&receipts[i])) continue;
		anx_route_evaluation_release(&receipts[i]);
		anx_so_delete(&receipts[i], false);
	}
	if (!anx_uuid_is_nil(&artifact)) anx_so_delete(&artifact, false);
	if (knowledge) { anx_so_delete(&knowledge->oid, false); anx_objstore_release(knowledge); }
	if (claimed) { anx_so_delete(&claimed->oid, false); anx_objstore_release(claimed); }
	if (forged) { anx_so_delete(&forged->oid, false); anx_objstore_release(forged); }
	if (engine) anx_engine_unregister(engine);
	return ret;
}
#endif
