/*
 * loop_jepa_train.c — IBAL → JEPA online training pipeline (RFC-0020 Phase 17).
 *
 * After each committed session, the best-candidate proposal's winning action_id
 * is extracted and passed to anx_jepa_record_winner() so the online training
 * step counter advances.  A fresh system observation is collected and stored as
 * ANX_OBJ_JEPA_OBS so the predictor has labeled (s, a) pairs to learn from.
 *
 * The actual VICReg training step fires automatically when the JEPA subsystem's
 * internal batch accumulator reaches its batch_size threshold.
 */

#include "loop_internal.h"
#include <anx/loop.h>
#include <anx/jepa.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/kprintf.h>

int anx_loop_jepa_ingest(anx_oid_t session_oid, const char *world_uri)
{
	struct anx_loop_session *s;
	struct anx_jepa_obs obs;
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
	ret = anx_jepa_observe(&obs);
	if (ret != ANX_OK)
		anx_memset(&obs, 0, sizeof(obs));

	/* Store as ANX_OBJ_JEPA_OBS — best-effort, non-fatal if JEPA absent */
	ret = anx_jepa_observe_store(&obs, &obs_oid);
	if (ret != ANX_OK) {
		kprintf("[loop-jepa] observe_store failed (%d)\n", ret);
		obs_oid = ANX_UUID_NIL;
	}

	/* Advance the online training step counter */
	anx_jepa_record_winner(action_id);

	kprintf("[loop-jepa] session %016llx: action=%u obs=%s\n",
		(unsigned long long)session_oid.lo, action_id,
		anx_uuid_is_nil(&obs_oid) ? "nil" : "ok");

	return ANX_OK;
}
