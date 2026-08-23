/*
 * effect.c — Protected Operation ABI: prepare/dispatch/settle
 * (RFC-0028).
 */

#include <anx/types.h>
#include <anx/effect.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/uuid.h>

/* One-way transition table — UNKNOWN is a dead end by construction. */
static const bool effect_transitions[5][5] = {
	/* PREPARED -> */
	[ANX_EFFECT_PREPARED] = {
		[ANX_EFFECT_DISPATCHING] = true,
	},
	/* DISPATCHING -> */
	[ANX_EFFECT_DISPATCHING] = {
		[ANX_EFFECT_COMMITTED] = true,
		[ANX_EFFECT_RESTORED] = true,
		[ANX_EFFECT_UNKNOWN] = true,
	},
	/* COMMITTED, RESTORED, UNKNOWN are all terminal */
};

static int effect_transition(struct anx_pending_effect *effect,
			     enum anx_effect_phase new_phase)
{
	enum anx_effect_phase old;

	if (!effect)
		return ANX_EINVAL;

	old = effect->phase;
	if ((int)old < 0 || old > ANX_EFFECT_UNKNOWN)
		return ANX_EINVAL;
	if ((int)new_phase < 0 || new_phase > ANX_EFFECT_UNKNOWN)
		return ANX_EINVAL;
	if (!effect_transitions[old][new_phase])
		return ANX_EINVAL;

	effect->phase = new_phase;
	return ANX_OK;
}

int anx_effect_prepare(anx_cid_t cell_id, struct anx_sink *sink,
		       const anx_oid_t *object_oid,
		       struct anx_pending_effect **out)
{
	struct anx_cell *cell;
	struct anx_pending_effect *effect;
	int ret;

	if (!out)
		return ANX_EINVAL;

	cell = anx_cell_store_lookup(&cell_id);
	if (!cell)
		return ANX_ENOENT;

	/* CAN_CALL: does this cell's execution policy permit side effects? */
	if (!cell->execution.allow_side_effects) {
		anx_cell_store_release(cell);
		return ANX_EPERM;
	}

	/* CAN_SEND: independent of CAN_CALL — a capability to invoke an
	 * operation never implies authority to send arbitrary data through
	 * it. Skipped if this effect has no Sink (no data-flow component).
	 * A NULL object_oid (no backing object) checks as nil, which
	 * anx_object_get_sensitivity resolves to PUBLIC — a control effect
	 * with no object data is never blocked by information flow, only
	 * by CAN_CALL above. */
	if (sink) {
		anx_oid_t nil_oid = ANX_UUID_NIL;

		ret = anx_sink_check_send(sink, object_oid ? object_oid : &nil_oid);
		if (ret != ANX_OK) {
			anx_cell_store_release(cell);
			return ret;
		}
	}

	anx_cell_store_release(cell);

	effect = anx_zalloc(sizeof(*effect));
	if (!effect)
		return ANX_ENOMEM;

	effect->cell = cell_id;
	effect->sink = sink;
	effect->object_oid = object_oid ? *object_oid : ANX_UUID_NIL;
	effect->phase = ANX_EFFECT_PREPARED;

	*out = effect;
	return ANX_OK;
}

int anx_effect_mark_dispatching(struct anx_pending_effect *effect)
{
	return effect_transition(effect, ANX_EFFECT_DISPATCHING);
}

int anx_effect_commit(struct anx_pending_effect *effect)
{
	return effect_transition(effect, ANX_EFFECT_COMMITTED);
}

int anx_effect_restore(struct anx_pending_effect *effect)
{
	return effect_transition(effect, ANX_EFFECT_RESTORED);
}

int anx_effect_mark_unknown(struct anx_pending_effect *effect)
{
	return effect_transition(effect, ANX_EFFECT_UNKNOWN);
}

void anx_effect_destroy(struct anx_pending_effect *effect)
{
	if (effect)
		anx_free(effect);
}
