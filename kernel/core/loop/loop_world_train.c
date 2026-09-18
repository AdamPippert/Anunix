/*
 * loop_world_train.c — IBAL → world model online learning (RFC-0020 Phase 17).
 *
 * After each committed session, the best-candidate proposal's winning
 * action_id is passed to the world model (anx_world_record_winner()), and
 * a fresh system observation is stored and paired with that action in
 * the trajectory buffer, so the model has labeled (s, a) pairs to learn
 * from. When and how the model trains is the backend's business.
 */

#include "loop_internal.h"
#include <anx/loop.h>
#include <anx/world_model.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/kprintf.h>

int anx_loop_world_ingest(anx_oid_t session_oid, const char *world_uri)
{
	struct anx_loop_session *s;
	struct anx_world_obs obs;
	anx_oid_t obs_oid;
	uint32_t action_id = 0;
	int ret;

	if (!world_uri)
		return ANX_EINVAL;

	s = anx_loop_session_get(session_oid);
	if (!s)
		return ANX_ENOENT;

	/* Extract the winning action_id from the best candidate proposal */
	if (!anx_uuid_is_nil(&s->best_candidate))
		anx_loop_proposal_get_action_id(s->best_candidate, &action_id);

	/* Collect a fresh system observation */
	ret = anx_world_observe(&obs);
	if (ret != ANX_OK)
		anx_memset(&obs, 0, sizeof(obs));

	/* Store the observation: best-effort, non-fatal without a model */
	ret = anx_world_observe_store(&obs, &obs_oid);
	if (ret != ANX_OK) {
		kprintf("[loop-world] observe_store failed (%d)\n", ret);
		obs_oid = ANX_UUID_NIL;
	}

	/* Tell the model which action won */
	anx_world_record_winner(action_id);

	/* Record (obs, action) in the trajectory ring buffer for export */
	anx_world_traj_ingest(&obs, action_id, world_uri);

	kprintf("[loop-world] session %016llx: action=%u obs=%s\n",
		(unsigned long long)session_oid.lo, action_id,
		anx_uuid_is_nil(&obs_oid) ? "nil" : "ok");

	return ANX_OK;
}
