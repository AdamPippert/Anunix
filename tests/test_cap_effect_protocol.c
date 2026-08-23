/*
 * test_cap_effect_protocol.c — Prepare/dispatch/settle effect protocol
 * (RFC-0028).
 */

#include <anx/types.h>
#include <anx/effect.h>
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/uuid.h>
#include <anx/string.h>

static int make_cell(bool allow_side_effects, struct anx_cell **out)
{
	struct anx_cell_intent intent;
	int ret;

	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "effect_test_cell", sizeof(intent.name));

	ret = anx_cell_create(ANX_CELL_TASK_SIDE_EFFECT, &intent, out);
	if (ret != ANX_OK)
		return ret;

	(*out)->execution.allow_side_effects = allow_side_effects;
	return ANX_OK;
}

int test_cap_effect_protocol(void)
{
	struct anx_cell *cell;
	struct anx_cell *denied_cell;
	struct anx_state_object *obj;
	struct anx_so_create_params params;
	struct anx_sink *sink;
	struct anx_pending_effect *effect;
	int ret;

	anx_objstore_init();
	anx_cell_store_init();
	anx_sink_registry_init();

	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_BYTE_DATA;
	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK)
		return -1;

	ret = anx_sink_register("test-sink", ANX_SENSITIVITY_RESTRICTED, &sink);
	if (ret != ANX_OK)
		return -2;

	ret = make_cell(true, &cell);
	if (ret != ANX_OK)
		return -3;

	/* --- Happy path: PREPARED -> DISPATCHING -> COMMITTED --- */

	ret = anx_effect_prepare(cell->cid, sink, &obj->oid, &effect);
	if (ret != ANX_OK)
		return -10;
	if (effect->phase != ANX_EFFECT_PREPARED)
		return -11;
	if (anx_uuid_compare(&effect->cell, &cell->cid) != 0)
		return -12;

	ret = anx_effect_mark_dispatching(effect);
	if (ret != ANX_OK)
		return -13;
	if (effect->phase != ANX_EFFECT_DISPATCHING)
		return -14;

	ret = anx_effect_commit(effect);
	if (ret != ANX_OK)
		return -15;
	if (effect->phase != ANX_EFFECT_COMMITTED)
		return -16;

	/* Terminal: no further transition is valid from COMMITTED */
	if (anx_effect_mark_dispatching(effect) == ANX_OK)
		return -17;
	if (anx_effect_commit(effect) == ANX_OK)
		return -18;
	if (anx_effect_restore(effect) == ANX_OK)
		return -19;
	if (anx_effect_mark_unknown(effect) == ANX_OK)
		return -20;

	anx_effect_destroy(effect);

	/* --- prepare fails: CAN_CALL denied (execution.allow_side_effects
	 * is false) --- */

	ret = make_cell(false, &denied_cell);
	if (ret != ANX_OK)
		return -30;

	ret = anx_effect_prepare(denied_cell->cid, sink, &obj->oid, &effect);
	if (ret != ANX_EPERM)
		return -31;

	/* --- prepare fails: CAN_SEND denied (object sensitivity exceeds
	 * the Sink's ceiling) --- */

	{
		struct anx_sink *low_sink;

		ret = anx_sink_register("low-sink", ANX_SENSITIVITY_PUBLIC, &low_sink);
		if (ret != ANX_OK)
			return -40;

		ret = anx_object_set_sensitivity(&obj->oid, ANX_SENSITIVITY_CONFIDENTIAL);
		if (ret != ANX_OK)
			return -41;

		ret = anx_effect_prepare(cell->cid, low_sink, &obj->oid, &effect);
		if (ret != ANX_EPERM)
			return -42;

		/* A cell with side-effect authority is still blocked from
		 * sending sensitive data to a low-ceiling Sink — CAN_CALL
		 * passing does not imply CAN_SEND passes. */
	}

	/* --- prepare with no Sink: CAN_SEND is skipped entirely --- */

	ret = anx_effect_prepare(cell->cid, NULL, &obj->oid, &effect);
	if (ret != ANX_OK)
		return -50;
	anx_effect_destroy(effect);

	/* --- Restore from DISPATCHING (certified-not-started) --- */

	ret = anx_effect_prepare(cell->cid, sink, NULL, &effect);
	if (ret != ANX_OK)
		return -60;
	ret = anx_effect_mark_dispatching(effect);
	if (ret != ANX_OK)
		return -61;
	ret = anx_effect_restore(effect);
	if (ret != ANX_OK)
		return -62;
	if (effect->phase != ANX_EFFECT_RESTORED)
		return -63;

	/* Terminal: RESTORED accepts no further transition either */
	if (anx_effect_mark_unknown(effect) == ANX_OK)
		return -64;

	anx_effect_destroy(effect);

	/* --- Unknown outcome after a fault mid-dispatch is terminal: it
	 * must never be silently retried or resolved to success/failure --- */

	ret = anx_effect_prepare(cell->cid, sink, NULL, &effect);
	if (ret != ANX_OK)
		return -70;
	ret = anx_effect_mark_dispatching(effect);
	if (ret != ANX_OK)
		return -71;
	ret = anx_effect_mark_unknown(effect);
	if (ret != ANX_OK)
		return -72;
	if (effect->phase != ANX_EFFECT_UNKNOWN)
		return -73;

	/* Every possible next transition must be rejected — this is the
	 * core invariant: an ambiguous outcome is never resolved after
	 * the fact by anything reading it later. */
	if (anx_effect_mark_dispatching(effect) == ANX_OK)
		return -74;
	if (anx_effect_commit(effect) == ANX_OK)
		return -75;
	if (anx_effect_restore(effect) == ANX_OK)
		return -76;
	if (anx_effect_mark_unknown(effect) == ANX_OK)
		return -77;

	anx_effect_destroy(effect);

	/* --- Cannot skip PREPARED and go straight to DISPATCHING again,
	 * or commit/restore/unknown before dispatching has begun --- */

	ret = anx_effect_prepare(cell->cid, sink, NULL, &effect);
	if (ret != ANX_OK)
		return -80;
	if (anx_effect_commit(effect) == ANX_OK)
		return -81;
	if (anx_effect_restore(effect) == ANX_OK)
		return -82;
	if (anx_effect_mark_unknown(effect) == ANX_OK)
		return -83;
	if (effect->phase != ANX_EFFECT_PREPARED)
		return -84;
	anx_effect_destroy(effect);

	/* --- Bad args --- */

	if (anx_effect_prepare(cell->cid, sink, NULL, NULL) != ANX_EINVAL)
		return -90;
	{
		anx_cid_t bogus;

		anx_uuid_generate(&bogus);
		if (anx_effect_prepare(bogus, sink, NULL, &effect) != ANX_ENOENT)
			return -91;
	}

	return 0;
}
