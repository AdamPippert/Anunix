#include <anx/route.h>
#include <anx/memplane.h>
#include <anx/state_object.h>
#include <anx/identity.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/uuid.h>

static bool state_usable(const struct anx_cell *cell, const struct anx_continuity_hint *hint)
{
	struct anx_state_object *object = anx_objstore_lookup(&hint->state_oid);
	struct anx_mem_entry *memory;
	bool flags, usable;
	if (!object)
		return false;
	anx_spin_lock_irqsave(&object->lock, &flags);
	usable = object->state == ANX_OBJ_SEALED && object->version == hint->state_version &&
		 object->payload && object->payload_size && object->access_policy.rule_count <= ANX_MAX_ACCESS_RULES &&
		 anx_access_evaluate(&object->access_policy, &cell->cid, &object->creator_cell,
				     ANX_ACCESS_READ_PAYLOAD) == ANX_OK;
	anx_spin_unlock_irqrestore(&object->lock, flags);
	if (usable) {
		memory = anx_memplane_lookup(&hint->state_oid);
		usable = false;
		if (memory) {
			anx_spin_lock_irqsave(&memory->lock, &flags);
			usable = memory->validation == ANX_MEMVAL_VALIDATED &&
				 memory->profile != ANX_ADMIT_QUARANTINED &&
				 (memory->tier_mask & (ANX_TIER_BIT(ANX_MEM_L0) | ANX_TIER_BIT(ANX_MEM_L1)));
			anx_spin_unlock_irqrestore(&memory->lock, flags);
			anx_memplane_release(memory);
		}
	}
	anx_objstore_release(object);
	return usable;
}

int anx_route_plan_continuity(struct anx_cell *cell, struct anx_route_session *session,
			      const struct anx_continuity_hint *hint, struct anx_route_result *result)
{
	struct anx_route_session next_session;
	struct anx_route_result next;
	const anx_cid_t *active = anx_cell_current_id();
	uint64_t now = arch_time_now();
	uint64_t benefit, cost;
	uint32_t best = 0, resident = 0;
	bool have_best = false, have_resident = false, reuse;
	int ret;
	if (!cell || !session || !hint || !result || hint->schema != 1 ||
	    anx_uuid_is_nil(&hint->engine_id) || anx_uuid_is_nil(&hint->state_oid) || !hint->state_version ||
	    hint->expires_at_ns <= hint->created_at_ns || hint->created_at_ns > now ||
	    hint->expires_at_ns - hint->created_at_ns > ANX_CONTINUITY_MAX_HOLD_NS ||
	    hint->return_probability_permille > 1000 ||
	    hint->restoration_cost_ms > ANX_CONTINUITY_COST_MAX_MS ||
	    hint->reservation_cost_ms > ANX_CONTINUITY_COST_MAX_MS ||
	    hint->interference_cost_ms > ANX_CONTINUITY_COST_MAX_MS)
		return ANX_EINVAL;
	if ((active && anx_uuid_compare(active, &cell->cid)) || anx_identity_admit(cell, NULL) != ANX_OK)
		return ANX_EPERM;
	/* Preserve the session validator, permission filters, and one copied scoring policy. */
	next_session = *session;
	ret = anx_route_plan_session(cell, &next_session, &next);
	if (ret != ANX_OK)
		return ret;
	for (uint32_t i = 0; i < next.candidate_count; i++) {
		if (!next.candidates[i].feasible)
			continue;
		if (!have_best || next.candidates[i].score > next.candidates[best].score) {
			best = i;
			have_best = true;
		}
		if (!anx_uuid_compare(&next.candidates[i].engine_id, &hint->engine_id)) {
			resident = i;
			have_resident = true;
		}
	}
	benefit = (uint64_t)hint->return_probability_permille * hint->restoration_cost_ms;
	cost = ((uint64_t)hint->reservation_cost_ms + hint->interference_cost_ms) * 1000;
	reuse = have_resident && now < hint->expires_at_ns && benefit > cost && state_usable(cell, hint);
	next.selected_index = reuse ? resident : best;
	next.needs_escalation = next.candidates[next.selected_index].score < ANX_ROUTE_ESCALATION_THRESHOLD;
	/* Clear the prior affinity reason before publishing the continuity decision. */
	for (uint32_t i = 0; i < next.candidate_count; i++)
		if (next.candidates[i].feasible)
			anx_strlcpy(next.candidates[i].reason, "continuity scored candidate", sizeof(next.candidates[i].reason));
	anx_strlcpy(next.candidates[next.selected_index].reason,
		reuse ? "continuity state reuse" : "continuity scored fallback",
		sizeof(next.candidates[next.selected_index].reason));
	next_session.selected_engine = next.candidates[next.selected_index].engine_id;
	*session = next_session;
	*result = next;
	return ANX_OK;
}
