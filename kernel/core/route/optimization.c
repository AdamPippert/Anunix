/* Bind native routing proposals to their declared target and evidence objects. */
#include <anx/tuning.h>
#include <anx/state_object.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/optimization_harness.h>

static void hash_word(struct anx_sha256_ctx *hash, uint64_t value)
{
	uint8_t bytes[8];
	for (uint32_t i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (8 * i));
	anx_sha256_update(hash, bytes, sizeof(bytes));
}

static int object_ref(const anx_oid_t *oid, struct anx_route_artifact_ref *out)
{
	struct anx_object_handle handle = {0};
	struct anx_route_artifact_ref ref = {0};
	int ret = anx_so_open(oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) return ret;
	anx_spin_lock(&handle.obj->lock);
	if (handle.obj->state != ANX_OBJ_SEALED)
		ret = ANX_EPERM;
	else if (!handle.obj->payload || !handle.obj->payload_size ||
		 handle.obj->payload_size > ANX_ROUTE_OPTIMIZATION_INPUT_MAX ||
		 (int)handle.obj->sensitivity < 0 || handle.obj->sensitivity > ANX_SENSITIVITY_RESTRICTED)
		ret = ANX_EINVAL;
	else {
		ref.oid = handle.obj->oid;
		ref.version = handle.obj->version;
		ref.sensitivity = handle.obj->sensitivity;
		anx_sha256(handle.obj->payload, (uint32_t)handle.obj->payload_size, ref.digest);
	}
	anx_spin_unlock(&handle.obj->lock);
	anx_so_close(&handle);
	if (ret == ANX_OK) *out = ref;
	return ret;
}

static bool refs_equal(const struct anx_route_artifact_ref *a, const struct anx_route_artifact_ref *b)
{
	return !anx_uuid_compare(&a->oid, &b->oid) && a->version == b->version &&
	       a->sensitivity == b->sensitivity && !anx_memcmp(a->digest, b->digest, sizeof(a->digest));
}

int anx_route_target_capture(const anx_eid_t *eid, const anx_oid_t *knowledge,
			     struct anx_route_target_contract *out)
{
	struct anx_route_target_contract target = {0};
	struct anx_engine *engine;
	struct anx_sha256_ctx hash;
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!eid || anx_uuid_is_nil(eid) || !knowledge || !out) return ANX_EINVAL;
	engine = anx_engine_lookup(eid);
	if (!engine) return ANX_ENOENT;
	ret = object_ref(knowledge, &target.knowledge);
	if (ret != ANX_OK) return ret;
	target.schema = 1;
	target.build = *anx_kernel_profile_current();
	target.engine_id = *eid;
	anx_sha256_init(&hash);
	anx_spin_lock(&engine->lock);
	uint64_t fields[] = {
		engine->eid.hi, engine->eid.lo, engine->engine_class, engine->status,
		engine->capabilities, engine->supports_private_data, engine->requires_network,
		engine->max_context_tokens, engine->cpu_weight, engine->gpu_weight,
		engine->quality_score, engine->is_local, engine->has_topology_affinity,
		engine->topology_bk_lo, engine->topology_bk_hi,
		engine->model.param_count, engine->model.quant, engine->model.context_window,
		engine->model.bench_tok_per_sec, engine->model.mem_footprint_bytes, engine->model.offline_capable,
	};
	anx_spin_unlock(&engine->lock);
	for (uint32_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) hash_word(&hash, fields[i]);
	anx_sha256_final(&hash, target.engine_digest);
	*out = target;
	return ANX_OK;
}

int anx_route_target_check(const struct anx_route_target_contract *target)
{
	struct anx_route_target_contract current;
	int ret;
	if (!target) return ANX_EINVAL;
	if (!target->schema) {
		struct anx_route_target_contract empty = {0};
		return anx_memcmp(target, &empty, sizeof(empty)) ? ANX_EINVAL : ANX_OK;
	}
	if (target->schema != 1) return ANX_EINVAL;
	ret = anx_kernel_profile_check(&target->build);
	if (ret != ANX_OK) return ret;
	ret = anx_route_target_capture(&target->engine_id, &target->knowledge.oid, &current);
	if (ret != ANX_OK) return ret;
	return refs_equal(&current.knowledge, &target->knowledge) &&
	       !anx_memcmp(current.engine_digest, target->engine_digest, sizeof(current.engine_digest)) ?
	       ANX_OK : ANX_EBUSY;
}

int anx_route_tuning_artifact_create(const struct anx_route_tuning_action *action,
				     const anx_oid_t *evaluation, anx_oid_t *out)
{
	struct anx_route_optimization_artifact artifact = {0};
	struct anx_so_create_params params = {0};
	struct anx_state_object *object;
	anx_oid_t parents[2];
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!action || !evaluation || !out || action->schema != 1 || !action->expected_generation ||
	    action->target.schema != 1 || anx_route_weight_policy_validate(&action->weights) != ANX_OK)
		return ANX_EINVAL;
	ret = anx_route_target_check(&action->target);
	if (ret != ANX_OK) return ret;
	ret = object_ref(evaluation, &artifact.evaluation);
	if (ret != ANX_OK) return ret;
	artifact.schema = 1;
	artifact.action = *action;
	parents[0] = action->target.knowledge.oid;
	parents[1] = *evaluation;
	params.object_type = ANX_OBJ_STRUCTURED_DATA;
	params.schema_uri = ANX_ROUTE_OPTIMIZATION_SCHEMA;
	params.schema_version = "1";
	params.payload = &artifact;
	params.payload_size = sizeof(artifact);
	params.parent_oids = parents;
	params.parent_count = 2;
	ret = anx_so_create(&params, &object);
	if (ret != ANX_OK) return ret;
	ret = anx_so_seal(&object->oid);
	if (ret == ANX_OK) *out = object->oid;
	else anx_so_delete(&object->oid, false);
	anx_objstore_release(object);
	return ret;
}

int anx_route_tuning_begin_artifact(const anx_oid_t *oid, uint64_t *trial_out)
{
	struct anx_route_optimization_artifact artifact;
	struct anx_route_artifact_ref current;
	struct anx_object_handle handle = {0};
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!oid || !trial_out) return ANX_EINVAL;
	ret = anx_so_open(oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) return ret;
	if (handle.obj->state != ANX_OBJ_SEALED)
		ret = ANX_EPERM;
	else if (handle.obj->object_type != ANX_OBJ_STRUCTURED_DATA || handle.obj->payload_size != sizeof(artifact) ||
		 anx_strcmp(handle.obj->schema_uri, ANX_ROUTE_OPTIMIZATION_SCHEMA) ||
		 anx_strcmp(handle.obj->schema_version, "1"))
		ret = ANX_EINVAL;
	else {
		ret = anx_so_read_payload(&handle, 0, &artifact, sizeof(artifact));
		ret = ret == (int)sizeof(artifact) ? ANX_OK : (ret < 0 ? ret : ANX_EINVAL);
	}
	anx_so_close(&handle);
	if (ret != ANX_OK) return ret;
	if (artifact.schema != 1 || artifact.action.target.schema != 1) return ANX_EINVAL;
	ret = object_ref(&artifact.evaluation.oid, &current);
	if (ret != ANX_OK) return ret;
	if (!refs_equal(&current, &artifact.evaluation)) return ANX_EBUSY;
	ret = anx_route_evaluation_check(&artifact.action, &current);
	if (ret != ANX_OK) return ret;
	return anx_route_tuning_begin(&artifact.action, trial_out);
}
