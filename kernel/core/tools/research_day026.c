/* Expiring continuity advice cannot turn unusable state into engine authority. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/route.h>
#include <anx/memplane.h>
#include <anx/state_object.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/uuid.h>

static int choose(struct anx_cell *cell, struct anx_route_session *session,
		  const struct anx_continuity_hint *hint, const anx_eid_t *expected, bool reuse)
{
	struct anx_route_result result;
	uint64_t count = session->placement_count;
	int ret = anx_route_plan_continuity(cell, session, hint, &result);
	if (ret != ANX_OK)
		return ret;
	if (anx_uuid_compare(&session->selected_engine, expected) || session->placement_count != count + 1 ||
	    !result.candidates[result.selected_index].feasible ||
	    (anx_strcmp(result.candidates[result.selected_index].reason, "continuity state reuse") == 0) != reuse)
		return -2602;
	return ANX_OK;
}

int anx_research_day026(void)
{
	struct anx_cell *cell = NULL;
	struct anx_engine *a = NULL, *b = NULL;
	struct anx_state_object *object = NULL;
	struct anx_mem_entry *memory = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_so_create_params params = {0};
	struct anx_route_session session = {0}, saved_session;
	struct anx_route_result result, saved_result;
	struct anx_continuity_hint hint = {0}, valid;
	int ret;
	anx_strlcpy(intent.name, "research-day-026-continuity", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &cell);
	if (ret != ANX_OK)
		goto out;
	cell->constraints.locality = ANX_LOCAL_ONLY;
	cell->execution.allow_network = false;
	cell->execution.allow_remote_models = false;
	ret = anx_engine_register("research-day-026-a", ANX_ENGINE_LOCAL_MODEL, ANX_CAP_SUMMARIZATION, &a);
	if (ret == ANX_OK)
		ret = anx_engine_register("research-day-026-b", ANX_ENGINE_LOCAL_MODEL, ANX_CAP_SUMMARIZATION, &b);
	if (ret != ANX_OK)
		goto out;
	a->is_local = b->is_local = true;
	a->quality_score = 90;
	b->quality_score = 10;
	session.eligible_engines[0] = a->eid;
	session.eligible_engines[1] = b->eid;
	session.engine_count = 2;
	session.required_caps = ANX_CAP_SUMMARIZATION;
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = "continuation";
	params.payload_size = 12;
	params.creator_cell = cell->cid;
	ret = anx_so_create(&params, &object);
	if (ret == ANX_OK)
		ret = anx_so_seal(&object->oid);
	if (ret == ANX_OK)
		ret = anx_memplane_admit(&object->oid, ANX_ADMIT_CACHEABLE, &memory);
	if (ret == ANX_OK)
		ret = anx_memplane_set_validation(memory, ANX_MEMVAL_VALIDATED);
	if (ret != ANX_OK)
		goto out;
	hint.schema = 1;
	hint.engine_id = b->eid;
	hint.state_oid = object->oid;
	hint.state_version = object->version;
	hint.created_at_ns = arch_time_now();
	hint.expires_at_ns = hint.created_at_ns + ANX_CONTINUITY_MAX_HOLD_NS;
	hint.return_probability_permille = 900;
	hint.restoration_cost_ms = 1000;
	hint.reservation_cost_ms = 20;
	hint.interference_cost_ms = 50;
	valid = hint;
	ret = -2601;
	if (choose(cell, &session, &hint, &b->eid, true) != ANX_OK)
		goto out;
	hint.interference_cost_ms = 2000;
	ret = choose(cell, &session, &hint, &a->eid, false);
	if (ret != ANX_OK)
		goto out;
	hint = valid;
	hint.return_probability_permille = 0;
	ret = choose(cell, &session, &hint, &a->eid, false);
	if (ret != ANX_OK)
		goto out;
	hint = valid;
	hint.created_at_ns = 0;
	hint.expires_at_ns = 1;
	ret = choose(cell, &session, &hint, &a->eid, false);
	if (ret != ANX_OK)
		goto out;
	hint = valid;
	anx_memplane_set_validation(memory, ANX_MEMVAL_STALE);
	ret = choose(cell, &session, &hint, &a->eid, false);
	if (ret != ANX_OK)
		goto out;
	anx_memplane_set_validation(memory, ANX_MEMVAL_VALIDATED);
	anx_memplane_demote(memory, ANX_MEM_L0);
	anx_memplane_demote(memory, ANX_MEM_L1);
	ret = choose(cell, &session, &hint, &a->eid, false);
	if (ret != ANX_OK)
		goto out;
	anx_memplane_promote(memory, ANX_MEM_L0);
	object->access_policy.rule_count = 1;
	object->access_policy.rules[0].principal = cell->cid;
	object->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	object->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	ret = choose(cell, &session, &hint, &a->eid, false);
	object->access_policy.rule_count = 0;
	if (ret != ANX_OK)
		goto out;
	hint.state_version++;
	ret = choose(cell, &session, &hint, &a->eid, false);
	if (ret != ANX_OK)
		goto out;
	hint = valid;
	b->requires_network = true;
	ret = choose(cell, &session, &hint, &a->eid, false);
	if (ret != ANX_OK)
		goto out;
	b->requires_network = false;
	b->status = ANX_ENGINE_DRAINING;
	ret = choose(cell, &session, &hint, &a->eid, false);
	if (ret != ANX_OK)
		goto out;
	b->status = ANX_ENGINE_AVAILABLE;
	ret = choose(cell, &session, &hint, &b->eid, true);
	if (ret != ANX_OK)
		goto out;
	anx_memset(&result, 0x5a, sizeof(result));
	saved_result = result;
	saved_session = session;
	hint.return_probability_permille = 1001;
	ret = -2603;
	if (anx_route_plan_continuity(cell, &session, &hint, &result) != ANX_EINVAL ||
	    anx_memcmp(&session, &saved_session, sizeof(session)) || anx_memcmp(&result, &saved_result, sizeof(result)))
		goto out;
	hint = valid;
	cell->constraints.locality = ANX_REMOTE_REQUIRED;
	b->engine_class = ANX_ENGINE_REMOTE_MODEL;
	b->is_local = false;
	ret = -2604;
	if (anx_route_plan_continuity(cell, &session, &hint, &result) != ANX_EPERM ||
	    anx_memcmp(&session, &saved_session, sizeof(session)) || anx_memcmp(&result, &saved_result, sizeof(result)))
		goto out;
	ret = ANX_OK;
out:
	if (object) object->access_policy.rule_count = 0;
	if (memory) anx_memplane_forget(memory, ANX_FORGET_HARD_DELETE);
	if (object) anx_objstore_release(object);
	if (a) anx_engine_unregister(a);
	if (b) anx_engine_unregister(b);
	if (cell) anx_cell_destroy(cell);
	return ret;
}
#endif
