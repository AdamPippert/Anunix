/* A target-bound proposal cannot activate after its declared environment changes. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/tuning.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

int anx_research_day030(void)
{
	struct anx_route_tuning_state original, current;
	struct anx_route_tuning_action action = {0}, wrong;
	struct anx_state_object *knowledge = NULL, *evaluation = NULL;
	struct anx_state_object *unsealed = NULL;
	struct anx_object_handle handle = {0};
	struct anx_route_optimization_artifact record;
	struct anx_so_create_params params = {0};
	struct anx_engine *engine = NULL;
	anx_oid_t artifact = ANX_UUID_NIL;
	uint64_t trial = 0, sentinel = 123;
	int ret = anx_route_tuning_snapshot(&original);
	if (ret != ANX_OK || original.trial_active) return ANX_EBUSY;
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = "target knowledge";
	params.payload_size = 16;
	ret = anx_so_create(&params, &knowledge);
	if (ret == ANX_OK) ret = anx_so_seal(&knowledge->oid);
	params.payload = "evaluation";
	params.payload_size = 10;
	if (ret == ANX_OK) ret = anx_so_create(&params, &evaluation);
	if (ret == ANX_OK) ret = anx_so_seal(&evaluation->oid);
	if (ret == ANX_OK) ret = anx_engine_register("research-day-030-target", ANX_ENGINE_LOCAL_MODEL, 0, &engine);
	if (ret != ANX_OK) goto out;
	action.schema = 1;
	action.expected_generation = original.generation;
	action.weights = original.weights;
	action.weights.locality_bonus = original.weights.locality_bonus == 1000 ? 999 : 1000;
	ret = anx_route_target_capture(&engine->eid, &knowledge->oid, &action.target);
	if (ret == ANX_OK) ret = anx_route_tuning_artifact_create(&action, &evaluation->oid, &artifact);
	if (ret != ANX_OK) goto out;
	engine->gpu_weight = 1;
	ret = -3001;
	int result = anx_route_tuning_begin_artifact(&artifact, &trial);
	if (result != ANX_EBUSY || trial != 0) goto out;
	ret = -3002;
	if (anx_route_tuning_snapshot(&current) != ANX_OK || anx_memcmp(&current, &original, sizeof(current))) goto out;
	engine->gpu_weight = 0;
	engine->model.context_window = 4096;
	if (anx_route_tuning_begin_artifact(&artifact, &sentinel) != ANX_EBUSY || sentinel != 123) goto out;
	engine->model.context_window = 0;
	ret = anx_so_open(&artifact, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) goto out;
	ret = -3007;
	if (anx_so_read_payload(&handle, 0, &record, sizeof(record)) != (int)sizeof(record)) goto out;
	anx_so_close(&handle);
	params.object_type = ANX_OBJ_STRUCTURED_DATA;
	params.schema_uri = ANX_ROUTE_OPTIMIZATION_SCHEMA;
	params.schema_version = "1";
	params.payload = &record;
	params.payload_size = sizeof(record);
	ret = anx_so_create(&params, &unsealed);
	if (ret != ANX_OK) goto out;
	ret = -3008;
	if (anx_route_tuning_begin_artifact(&unsealed->oid, &sentinel) != ANX_EPERM || sentinel != 123) goto out;
	wrong = action;
	wrong.target.build.research_test ^= 1;
	ret = -3003;
	if (anx_route_tuning_begin(&wrong, &sentinel) != ANX_ENOTSUP || sentinel != 123) goto out;
	ret = anx_object_set_sensitivity(&knowledge->oid, ANX_SENSITIVITY_INTERNAL);
	if (ret != ANX_OK) goto out;
	ret = -3004;
	if (anx_route_tuning_begin_artifact(&artifact, &sentinel) != ANX_EBUSY || sentinel != 123) goto out;
	anx_object_set_sensitivity(&knowledge->oid, ANX_SENSITIVITY_PUBLIC);
	anx_object_set_sensitivity(&evaluation->oid, ANX_SENSITIVITY_INTERNAL);
	if (anx_route_tuning_begin_artifact(&artifact, &sentinel) != ANX_EBUSY || sentinel != 123) goto out;
	anx_object_set_sensitivity(&evaluation->oid, ANX_SENSITIVITY_PUBLIC);
	ret = anx_route_tuning_begin_artifact(&artifact, &trial);
	if (ret != ANX_OK) goto out;
	ret = -3005;
	if (anx_route_tuning_snapshot(&current) != ANX_OK || !current.trial_active ||
	    anx_memcmp(&current.weights, &action.weights, sizeof(current.weights)) ||
	    anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT) != ANX_OK)
		goto out;
	trial = 0;
	if (anx_route_tuning_begin_artifact(&artifact, &sentinel) != ANX_EBUSY || sentinel != 123) goto out;
	anx_engine_unregister(engine); engine = NULL;
	ret = -3006;
	if (anx_route_tuning_begin_artifact(&artifact, &sentinel) != ANX_ENOENT || sentinel != 123) goto out;
	ret = ANX_OK;
out:
	if (handle.obj) anx_so_close(&handle);
	if (trial) anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	if (!anx_uuid_is_nil(&artifact)) anx_so_delete(&artifact, false);
	if (knowledge) { anx_so_delete(&knowledge->oid, false); anx_objstore_release(knowledge); }
	if (evaluation) { anx_so_delete(&evaluation->oid, false); anx_objstore_release(evaluation); }
	if (unsealed) { anx_so_delete(&unsealed->oid, false); anx_objstore_release(unsealed); }
	if (engine) anx_engine_unregister(engine);
	return ret;
}
#endif
