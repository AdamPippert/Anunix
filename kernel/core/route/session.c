/* Caller-owned session affinity with current policy checks on every placement. */
#include <anx/route.h>
#include <anx/string.h>
#include <anx/uuid.h>

static bool session_engine_eligible(const struct anx_cell *cell,
				    const struct anx_engine *engine, uint32_t caps)
{
	if (!engine || (engine->status != ANX_ENGINE_AVAILABLE &&
			engine->status != ANX_ENGINE_DEGRADED) ||
	    (engine->engine_class != ANX_ENGINE_LOCAL_MODEL &&
	     engine->engine_class != ANX_ENGINE_REMOTE_MODEL) ||
	    (engine->capabilities & caps) != caps ||
	    engine->quality_score > 100 || engine->cpu_weight > 100 || engine->gpu_weight > 100)
		return false;
	if ((cell->constraints.locality == ANX_LOCAL_ONLY && !engine->is_local) ||
	    (cell->constraints.locality == ANX_REMOTE_REQUIRED && engine->is_local) ||
	    (engine->requires_network && !cell->execution.allow_network) ||
	    (engine->engine_class == ANX_ENGINE_REMOTE_MODEL && !cell->execution.allow_remote_models))
		return false;
	return true;
}

int anx_route_plan_session(struct anx_cell *cell, struct anx_route_session *session,
			   struct anx_route_result *result)
{
	struct anx_route_result next = {0};
	uint32_t i, j, best = 0, affinity = 0;
	bool have_best = false, have_affinity = false, selected_member = false;
	bool unbound;

	if (!cell || !session || !result || session->engine_count == 0 ||
	    session->engine_count > ANX_MAX_ROUTE_CANDIDATES ||
	    (uint32_t)cell->constraints.locality > ANX_REMOTE_REQUIRED ||
	    (uint32_t)cell->routing.strategy > ANX_ROUTE_POLICY_LOCKED ||
	    (cell->constraints.topology_bk_set &&
	     cell->constraints.topology_bk_lo > cell->constraints.topology_bk_hi))
		return ANX_EINVAL;
	if (session->placement_count == ~(uint64_t)0)
		return ANX_EFULL;
	unbound = anx_uuid_is_nil(&session->selected_engine);
	if (unbound != (session->placement_count == 0))
		return ANX_EINVAL;
	for (i = 0; i < session->engine_count; i++) {
		if (anx_uuid_is_nil(&session->eligible_engines[i]))
			return ANX_EINVAL;
		for (j = 0; j < i; j++)
			if (anx_uuid_compare(&session->eligible_engines[i], &session->eligible_engines[j]) == 0)
				return ANX_EINVAL;
		if (anx_uuid_compare(&session->eligible_engines[i], &session->selected_engine) == 0)
			selected_member = true;
	}
	if (!unbound && !selected_member)
		return ANX_EINVAL;
	for (i = 0; i < session->engine_count; i++) {
		struct anx_route_candidate *candidate = &next.candidates[i];
		struct anx_engine *engine = anx_engine_lookup(&session->eligible_engines[i]);
		candidate->engine_id = session->eligible_engines[i];
		candidate->feasible = session_engine_eligible(cell, engine, session->required_caps);
		candidate->score = -1;
		if (!candidate->feasible) {
			anx_strlcpy(candidate->reason, "session backend ineligible", sizeof(candidate->reason));
			continue;
		}
		candidate->score = anx_route_score_engine(cell, engine);
		if (!have_best || candidate->score > next.candidates[best].score) {
			best = i;
			have_best = true;
		}
		if (anx_uuid_compare(&engine->eid, &session->selected_engine) == 0) {
			affinity = i;
			have_affinity = true;
		}
	}
	if (!have_best)
		return ANX_EPERM;
	next.candidate_count = session->engine_count;
	next.selected_index = have_affinity ? affinity : best;
	next.decided_at = ANX_ROUTE_STAGE_KERNEL;
	next.needs_escalation = next.candidates[next.selected_index].score < ANX_ROUTE_ESCALATION_THRESHOLD;
	anx_strlcpy(next.candidates[next.selected_index].reason,
		have_affinity ? "session affinity" : "session scored placement",
		sizeof(next.candidates[next.selected_index].reason));
	session->selected_engine = next.candidates[next.selected_index].engine_id;
	session->placement_count++;
	*result = next;
	return ANX_OK;
}
