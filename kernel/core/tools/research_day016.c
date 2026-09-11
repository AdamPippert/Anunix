/* Revalidate governed operations at the final pre-dispatch boundary. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/effect.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct effect_boundary_state {
	struct anx_cell *owner;
	struct anx_pending_effect *prepared;
	bool checked;
};

static int boundary_handler(struct anx_external_call *call, void *context)
{
	struct effect_boundary_state *state = context;
	struct anx_pending_effect *other = NULL;
	int ret;
	(void)call;
	ret = anx_effect_prepare(state->owner->cid, NULL, NULL, &other);
	if (other)
		anx_effect_destroy(other);
	if (ret != ANX_EPERM || anx_effect_mark_dispatching(state->prepared) != ANX_EPERM ||
	    state->prepared->phase != ANX_EFFECT_PREPARED)
		return ANX_EIO;
	state->checked = true;
	return ANX_OK;
}

int anx_research_day016(void)
{
	struct anx_cell *owner = NULL, *caller = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call call = {0};
	struct anx_state_object *object = NULL;
	struct anx_so_create_params params = {0};
	struct anx_pending_effect *effect = NULL, *bad = NULL;
	struct anx_sink *sink;
	struct effect_boundary_state state = {0};
	anx_oid_t missing;
	int rc;

	anx_strlcpy(intent.name, "research-day-016-owner", sizeof(intent.name));
	rc = anx_cell_create(ANX_CELL_TASK_SIDE_EFFECT, &intent, &owner);
	if (rc != ANX_OK)
		goto out;
	owner->execution.allow_side_effects = true;
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = "effect";
	params.payload_size = 6;
	params.creator_cell = owner->cid;
	rc = anx_so_create(&params, &object);
	if (rc != ANX_OK)
		goto out;
	rc = anx_sink_register("research-day-016", ANX_SENSITIVITY_PUBLIC, &sink);
	if (rc != ANX_OK)
		goto out;
	rc = anx_effect_prepare(owner->cid, sink, &object->oid, &effect);
	if (rc != ANX_OK)
		goto out;
	owner->execution.allow_side_effects = false;
	rc = -1601;
	if (anx_effect_mark_dispatching(effect) != ANX_EPERM || effect->phase != ANX_EFFECT_PREPARED)
		goto out;
	owner->execution.allow_side_effects = true;
	rc = -1602;
	if (anx_object_set_sensitivity(&object->oid, ANX_SENSITIVITY_CONFIDENTIAL) != ANX_OK ||
	    anx_effect_mark_dispatching(effect) != ANX_EPERM || effect->phase != ANX_EFFECT_PREPARED)
		goto out;
	rc = -1603;
	if (anx_object_set_sensitivity(&object->oid, ANX_SENSITIVITY_PUBLIC) != ANX_OK ||
	    anx_effect_mark_dispatching(effect) != ANX_OK)
		goto out;
	owner->execution.allow_side_effects = false;
	rc = -1604;
	if (anx_effect_mark_unknown(effect) != ANX_OK || effect->phase != ANX_EFFECT_UNKNOWN ||
	    anx_effect_mark_dispatching(effect) != ANX_EINVAL)
		goto out;
	anx_effect_destroy(effect);
	effect = NULL;
	owner->execution.allow_side_effects = true;
	anx_uuid_generate(&missing);
	rc = -1605;
	if (anx_effect_prepare(owner->cid, sink, &missing, &bad) != ANX_ENOENT || bad)
		goto out;
	rc = anx_effect_prepare(owner->cid, NULL, NULL, &effect);
	if (rc != ANX_OK)
		goto out;
	state.owner = owner;
	state.prepared = effect;
	rc = anx_external_register_handler("anxresearch016", boundary_handler, &state);
	if (rc != ANX_OK)
		goto out;
	anx_strlcpy(intent.name, "research-day-016-caller", sizeof(intent.name));
	rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (rc != ANX_OK)
		goto out;
	anx_strlcpy(call.endpoint, "anxresearch016://boundary", sizeof(call.endpoint));
	caller->ext_call = &call;
	caller->execution.allow_side_effects = true;
	rc = -1606;
	if (anx_cell_run(caller) != ANX_OK || !state.checked)
		goto out;
	rc = -1607;
	if (anx_cell_cancel(owner) != ANX_OK || anx_effect_mark_dispatching(effect) != ANX_EPERM ||
	    effect->phase != ANX_EFFECT_PREPARED ||
	    anx_effect_prepare(owner->cid, NULL, NULL, &bad) != ANX_EPERM || bad)
		goto out;
	if (anx_cell_destroy(owner) != ANX_OK)
		goto out;
	owner = NULL;
	rc = -1608;
	if (anx_effect_mark_dispatching(effect) != ANX_ENOENT || effect->phase != ANX_EFFECT_PREPARED)
		goto out;
	rc = ANX_OK;
out:
	if (bad)
		anx_effect_destroy(bad);
	if (effect)
		anx_effect_destroy(effect);
	if (object)
		anx_objstore_release(object);
	if (caller)
		anx_cell_destroy(caller);
	if (owner)
		anx_cell_destroy(owner);
	anx_external_unregister_handler("anxresearch016");
	return rc;
}
#endif
