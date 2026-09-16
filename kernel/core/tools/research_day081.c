#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/route_catalog.h>
#include <anx/route.h>
#include <anx/external_call.h>
#include <anx/effect_fence.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
struct fixture081 {
	struct anx_cell *owner, *foreign;
	struct anx_engine *engine;
	struct anx_route_tuning_state original;
	struct anx_route_weight_policy weights[3];
	struct anx_route_catalog_view catalog, output;
	anx_oid_t profiles[3];
	struct anx_external_call call;
	bool foreign_active;
};
static bool scores081(struct fixture081 *f, struct anx_cell *cell, int status, const struct anx_route_weight_policy *weights)
{
	struct anx_route_result route;
	if (anx_route_plan(cell, &route) != ANX_OK || route.profile_status != status || route.profile_applied != (status == ANX_OK)) return false;
	for (uint32_t i = 0; i < route.candidate_count; i++)
		if (!anx_uuid_compare(&route.candidates[i].engine_id, &f->engine->eid))
			return route.candidates[i].feasible && route.candidates[i].score == anx_route_score_with_policy(cell, f->engine, weights);
	return false;
}
static void select081(struct fixture081 *f, struct anx_cell *cell, uint32_t index)
{
	cell->routing.catalog_id = f->catalog.id;
	cell->routing.catalog_epoch = f->catalog.epoch;
	cell->routing.catalog_index = index;
}
static int active081(struct anx_external_call *call, void *arg)
{
	struct fixture081 *f = arg; (void)call;
	if (anx_route_catalog_create(&f->owner->cid, f->profiles, 3, &f->output) != ANX_EPERM ||
	    anx_route_catalog_replace(f->catalog.id, f->catalog.epoch, f->profiles, 3, &f->output) != ANX_EPERM ||
	    anx_route_catalog_destroy(f->catalog.id) != ANX_EPERM) return -8109;
	if (f->foreign_active) {
		struct anx_route_weight_policy result = f->original.weights;
		return anx_route_catalog_get(f->catalog.id, &f->output) == ANX_EPERM &&
			anx_route_catalog_choose(f->catalog.id, f->catalog.epoch, 0, f->owner, &f->original, &result) == ANX_EPERM &&
			!anx_memcmp(&result, &f->original.weights, sizeof(result)) &&
			scores081(f, f->foreign, ANX_EPERM, &f->original.weights) ? ANX_OK : -8110;
	}
	if (anx_route_catalog_get(f->catalog.id, &f->output) != ANX_OK) return -8111;
	for (uint32_t i = 0; i < 3; i++) {
		select081(f, f->owner, i);
		if (!scores081(f, f->owner, ANX_OK, &f->weights[2-i])) return -8112;
	}
	select081(f, f->owner, ~(uint32_t)0);
	return scores081(f, f->owner, ANX_EINVAL, &f->original.weights) ? ANX_OK : -8113;
}
int anx_research_day081(void)
{
	struct fixture081 *f = anx_zalloc(sizeof(*f));
	struct anx_cell_intent intent = {0};
	struct anx_state_object *profile = NULL, *forged = NULL;
	struct anx_effect_fence_view fence;
	struct anx_route_profile_case input = { .strategy = ANX_ROUTE_LOCAL_FIRST, .locality = ANX_LOCAL_ONLY };
	struct anx_route_tuning_state after;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	ret = anx_route_tuning_snapshot(&f->original);
	if (ret != ANX_OK || f->original.trial_active) { ret = ANX_EBUSY; goto out; }
	anx_strlcpy(intent.name, "research-day-081", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &f->owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &f->foreign);
	if (ret == ANX_OK) ret = anx_engine_register("research-day-081", ANX_ENGINE_DETERMINISTIC_TOOL, 0, &f->engine);
	if (ret != ANX_OK) goto out;
	f->engine->is_local = true; f->engine->status = ANX_ENGINE_AVAILABLE; f->engine->quality_score = 100;
	f->owner->routing.strategy = f->foreign->routing.strategy = input.strategy;
	f->owner->constraints.locality = f->foreign->constraints.locality = input.locality;
	f->owner->execution.allow_side_effects = f->foreign->execution.allow_side_effects = true;
	ret = anx_effect_fence_create(&fence);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(f->owner, &fence.id);
	for (uint32_t i = 0; ret == ANX_OK && i < 3; i++) {
		f->weights[i] = f->original.weights; f->weights[i].locality_bonus += i + 1;
		ret = anx_route_profile_compile(&f->weights[i], &input, 1, &f->profiles[i]);
	}
	if (ret != ANX_OK) goto out;
	ret = -8101;
	if (anx_route_catalog_create(&f->owner->cid, f->profiles, 3, &f->catalog) != ANX_OK) goto out;
	if (anx_route_catalog_create(&f->owner->cid, f->profiles, 3, &f->output) != ANX_EEXIST) { ret = -8102; goto out; }
	f->output = f->catalog; f->output.profiles[0] = ANX_UUID_NIL; f->output.count = 4;
	for (uint32_t i = 0; i < 3; i++) {
		select081(f, f->owner, i);
		if (!scores081(f, f->owner, ANX_OK, &f->weights[i])) { ret = -8102; goto out; }
	}
	/* A valid legacy profile cannot bypass rejection of an explicit catalog selection. */
	f->owner->routing.profile_oid = f->profiles[0];
	select081(f, f->owner, 3);
	if (!scores081(f, f->owner, ANX_EINVAL, &f->original.weights)) { ret = -8103; goto out; }
	select081(f, f->owner, 0); f->owner->routing.catalog_epoch++;
	if (!scores081(f, f->owner, ANX_EBUSY, &f->original.weights)) { ret = -8103; goto out; }
	select081(f, f->owner, 0); f->owner->routing.catalog_id = ~(uint64_t)0;
	if (!scores081(f, f->owner, ANX_ENOENT, &f->original.weights)) { ret = -8103; goto out; }
	select081(f, f->owner, 0);
	profile = anx_objstore_lookup(&f->profiles[0]);
	if (!profile) { ret = ANX_ENOENT; goto out; }
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA, .schema_uri = ANX_ROUTE_PROFILE_SCHEMA,
		.schema_version = "1", .payload = profile->payload, .payload_size = profile->payload_size };
	ret = anx_so_create(&params, &forged);
	if (ret == ANX_OK) ret = anx_so_seal(&forged->oid);
	if (ret != ANX_OK) goto out;
	f->output = f->catalog;
	if (anx_route_catalog_replace(f->catalog.id, f->catalog.epoch, &forged->oid, 1, &f->output) != ANX_EPERM ||
	    anx_memcmp(&f->output, &f->catalog, sizeof(f->catalog)) || !scores081(f, f->owner, ANX_OK, &f->weights[0])) { ret = -8104; goto out; }
	anx_oid_t duplicate[2] = {f->profiles[0], f->profiles[0]};
	if (anx_route_catalog_replace(f->catalog.id, f->catalog.epoch, duplicate, 2, &f->output) != ANX_EEXIST ||
	    anx_route_catalog_replace(f->catalog.id, f->catalog.epoch, f->profiles, 0, &f->output) != ANX_EINVAL ||
	    anx_route_catalog_replace(f->catalog.id, f->catalog.epoch, f->profiles, 5, &f->output) != ANX_EINVAL) { ret = -8104; goto out; }
	((uint8_t *)profile->payload)[0] ^= 1;
	bool rejected = scores081(f, f->owner, ANX_EPERM, &f->original.weights);
	((uint8_t *)profile->payload)[0] ^= 1;
	if (!rejected) { ret = -8105; goto out; }
	profile->access_policy.rule_count = 1;
	profile->access_policy.rules[0] = (struct anx_access_rule){ .operations = ANX_ACCESS_READ_PAYLOAD, .effect = ANX_EFFECT_DENY };
	rejected = scores081(f, f->owner, ANX_EPERM, &f->original.weights); profile->access_policy.rule_count = 0;
	if (!rejected) { ret = -8105; goto out; }
	f->engine->quality_score--;
	rejected = scores081(f, f->owner, ANX_EBUSY, &f->original.weights); f->engine->quality_score++;
	if (!rejected) { ret = -8105; goto out; }
	ret = anx_effect_fence_hold(f->owner);
	if (ret != ANX_OK) goto out;
	if (!scores081(f, f->owner, ANX_EBUSY, &f->original.weights)) { ret = -8106; goto out; }
	ret = anx_effect_fence_get(&fence.id, &fence);
	if (ret == ANX_OK) ret = anx_effect_fence_transition(&fence.id, fence.generation, ANX_FENCE_RUNNING);
	if (ret != ANX_OK) goto out;
	select081(f, f->owner, 1);
	ret = anx_route_profile_release(&f->profiles[1]);
	if (ret != ANX_OK) goto out;
	if (!scores081(f, f->owner, ANX_EPERM, &f->original.weights)) { ret = -8117; goto out; }
	anx_so_delete(&f->profiles[1], false); f->profiles[1] = ANX_UUID_NIL;
	ret = anx_route_profile_compile(&f->weights[1], &input, 1, &f->profiles[1]);
	if (ret != ANX_OK) goto out;
	anx_oid_t reversed[3] = {f->profiles[2], f->profiles[1], f->profiles[0]};
	ret = anx_route_catalog_replace(f->catalog.id, f->catalog.epoch, reversed, 3, &f->catalog);
	if (ret != ANX_OK) goto out;
	if (f->catalog.epoch != 2 || !scores081(f, f->owner, ANX_EBUSY, &f->original.weights) ||
	    anx_route_catalog_replace(f->catalog.id, 1, reversed, 3, &f->output) != ANX_EBUSY) { ret = -8107; goto out; }
	select081(f, f->owner, 0); select081(f, f->foreign, 0);
	ret = anx_external_register_handler("anxresearch081", active081, f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint, "anxresearch081://select", sizeof(f->call.endpoint));
	f->foreign_active = true; f->foreign->ext_call = &f->call; ret = anx_cell_run(f->foreign);
	if (ret == ANX_OK) { f->foreign_active = false; f->owner->ext_call = &f->call; ret = anx_cell_run(f->owner); }
	if (ret != ANX_OK) goto out;
	/* Terminal ownership prevents another catalog use. */
	select081(f, f->owner, 0);
	if (!scores081(f, f->owner, ANX_EBUSY, &f->original.weights)) { ret = -8114; goto out; }
	ret = anx_route_catalog_destroy(f->catalog.id); f->catalog.id = 0;
	if (ret != ANX_OK) goto out;
	if (!scores081(f, f->owner, ANX_ENOENT, &f->original.weights)) { ret = -8115; goto out; }
	anx_route_tuning_snapshot(&after);
	if (anx_memcmp(&after, &f->original, sizeof(after))) { ret = -8116; goto out; }
	ret = ANX_OK;
	kprintf("day081 policies=3 catalog_epochs=2 actual_scores=checked invalid=incumbent foreign=denied global_policy=unchanged\n");
out:
	anx_external_unregister_handler("anxresearch081");
	if (f->catalog.id) anx_route_catalog_destroy(f->catalog.id);
	if (profile) anx_objstore_release(profile);
	if (forged) { anx_so_delete(&forged->oid, false); anx_objstore_release(forged); }
	for (uint32_t i = 0; i < 3; i++) if (!anx_uuid_is_nil(&f->profiles[i])) { anx_route_profile_release(&f->profiles[i]); anx_so_delete(&f->profiles[i], false); }
	if (f->owner) anx_cell_destroy(f->owner);
	if (f->foreign) anx_cell_destroy(f->foreign);
	if (f->engine) anx_engine_unregister(f->engine);
	if (ret != ANX_OK) kprintf("day081 native failure rc=%d\n", ret);
	anx_free(f); return ret;
}
#endif
