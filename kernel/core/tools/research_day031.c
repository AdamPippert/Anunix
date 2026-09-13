/* A caller-authored pass record must not authorize an optimization trial. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/tuning.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

int anx_research_day031(void)
{
	struct anx_state_object *knowledge = NULL, *claimed = NULL;
	struct anx_so_create_params params = {0};
	struct anx_route_tuning_state original;
	struct anx_route_tuning_action action = {0};
	struct anx_engine *engine = NULL;
	anx_oid_t artifact = ANX_UUID_NIL;
	uint64_t trial = 0;
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
	ret = anx_route_target_capture(&engine->eid, &knowledge->oid, &action.target);
	if (ret == ANX_OK) ret = anx_route_tuning_artifact_create(&action, &claimed->oid, &artifact);
	if (ret != ANX_OK) goto out;
	ret = -3101;
	if (anx_route_tuning_begin_artifact(&artifact, &trial) != ANX_EPERM || trial != 0) goto out;
	ret = ANX_OK;
out:
	if (trial) anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	if (!anx_uuid_is_nil(&artifact)) anx_so_delete(&artifact, false);
	if (knowledge) { anx_so_delete(&knowledge->oid, false); anx_objstore_release(knowledge); }
	if (claimed) { anx_so_delete(&claimed->oid, false); anx_objstore_release(claimed); }
	if (engine) anx_engine_unregister(engine);
	return ret;
}
#endif
