/* Typed policy intent must be feasible and measured before a live trial. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/tuning.h>
#include <anx/optimization_harness.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

int anx_research_day040(void)
{
	struct anx_route_tuning_state original, current;
	struct anx_route_tuning_action base = {0}, compiled, sentinel, changed;
	struct anx_route_policy_task task;
	struct anx_state_object *knowledge = NULL;
	struct anx_engine *engine = NULL;
	struct anx_so_create_params p = {0};
	struct anx_object_handle h = {0};
	struct anx_route_evaluation report;
	anx_oid_t receipts[2] = {0}, artifacts[3] = {0}, denied = ANX_UUID_NIL;
	uint64_t trial = 0, untouched = 123;
	int ret = anx_route_tuning_snapshot(&original);
	if (ret != ANX_OK || original.trial_active) return ANX_EBUSY;
	p.object_type = ANX_OBJ_BYTE_DATA; p.payload = "policy-target"; p.payload_size = 13;
	ret = anx_so_create(&p, &knowledge);
	if (ret == ANX_OK) ret = anx_so_seal(&knowledge->oid);
	if (ret == ANX_OK) ret = anx_engine_register("research-day-040-target", ANX_ENGINE_LOCAL_MODEL, 0, &engine);
	if (ret != ANX_OK) goto out;
	base.schema = 1; base.expected_generation = original.generation; base.weights = original.weights;
	base.weights.locality_bonus = 1000;
	ret = anx_route_target_capture(&engine->eid, &knowledge->oid, &base.target);
	if (ret != ANX_OK) goto out;
	anx_route_policy_defaults(&task);
	task.constraints[0].range.minimum = 3; task.constraints[0].range.maximum = 2;
	anx_memset(&compiled, 0x5a, sizeof(compiled)); sentinel = compiled;
	ret = -4001;
	if (anx_route_policy_compile(&task, &base, &compiled) != ANX_EINVAL || anx_memcmp(&compiled, &sentinel, sizeof(compiled))) goto out;
	changed = base; changed.task = task;
	if (anx_route_evaluate(&changed, &denied) != ANX_EINVAL || !anx_uuid_is_nil(&denied) ||
	    anx_route_tuning_begin(&changed, &untouched) != ANX_EINVAL || untouched != 123) goto out;
	anx_route_policy_defaults(&task);
	task.variables[2].unit = ANX_POLICY_PERCENT;
	ret = -4002;
	if (anx_route_policy_compile(&task, &base, &compiled) != ANX_EINVAL) goto out;
	anx_route_policy_defaults(&task);
	task.variables[0].maximum = 999;
	if (anx_route_policy_compile(&task, &base, &compiled) != ANX_EINVAL) goto out;
	anx_route_policy_defaults(&task);
	task.constraints[0].metric = 99;
	if (anx_route_policy_compile(&task, &base, &compiled) != ANX_EINVAL) goto out;
	anx_route_policy_defaults(&task);
	task.variables[0].origin = 0;
	if (anx_route_policy_compile(&task, &base, &compiled) != ANX_EINVAL) goto out;
	anx_route_policy_defaults(&task);
	task.constraints[0].range.minimum = task.constraints[0].range.maximum = 2;
	task.constraints[0].range.origin = ANX_POLICY_OPERATOR;
	ret = anx_route_policy_compile(&task, &base, &compiled);
	if (ret == ANX_OK) ret = anx_route_evaluate(&compiled, &receipts[0]);
	if (ret == ANX_OK) ret = anx_route_tuning_artifact_create(&compiled, &receipts[0], &artifacts[0]);
	if (ret == ANX_OK) ret = anx_so_open(&receipts[0], ANX_OPEN_READ, &h);
	if (ret != ANX_OK) goto out;
	ret = -4003;
	if (anx_so_read_payload(&h, 0, &report, sizeof(report)) != sizeof(report) || report.failure_count != 3 ||
	    anx_route_tuning_begin_artifact(&artifacts[0], &untouched) != ANX_EPERM ||
	    anx_route_tuning_begin(&compiled, &untouched) != ANX_EPERM || untouched != 123) goto out;
	anx_so_close(&h);
	anx_route_policy_defaults(&task);
	ret = anx_route_policy_compile(&task, &base, &compiled);
	if (ret != ANX_OK) goto out;
	ret = -4004;
	if (anx_route_tuning_begin(&compiled, &untouched) != ANX_EPERM || untouched != 123) goto out;
	ret = anx_route_evaluate(&compiled, &receipts[1]);
	if (ret == ANX_OK) ret = anx_route_tuning_artifact_create(&compiled, &receipts[1], &artifacts[1]);
	if (ret != ANX_OK) goto out;
	changed = compiled;
	changed.task.constraints[0].range.maximum--;
	ret = anx_route_tuning_artifact_create(&changed, &receipts[1], &artifacts[2]);
	if (ret != ANX_OK) goto out;
	ret = -4005;
	if (anx_route_tuning_begin_artifact(&artifacts[2], &untouched) != ANX_EPERM || untouched != 123) goto out;
	ret = anx_route_tuning_begin_artifact(&artifacts[1], &trial);
	if (ret == ANX_OK) ret = anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	if (ret != ANX_OK) goto out;
	trial = 0;
	ret = anx_route_tuning_snapshot(&current);
	if (ret == ANX_OK && (current.trial_active || anx_memcmp(&current.weights, &original.weights, sizeof(original.weights)))) ret = -4006;
out:
	anx_so_close(&h);
	if (trial) anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	for (uint32_t i = 0; i < 2; i++) if (!anx_uuid_is_nil(&receipts[i])) {
		anx_route_evaluation_release(&receipts[i]); anx_so_delete(&receipts[i], false);
	}
	for (uint32_t i = 0; i < 3; i++) if (!anx_uuid_is_nil(&artifacts[i])) anx_so_delete(&artifacts[i], false);
	if (knowledge) { anx_so_delete(&knowledge->oid, false); anx_objstore_release(knowledge); }
	if (engine) anx_engine_unregister(engine);
	return ret;
}
#endif
