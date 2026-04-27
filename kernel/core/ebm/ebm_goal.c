/*
 * ebm_goal.c — Goal alignment energy scorer (RFC-0020 Phase 3).
 *
 * Delegates to anx_loop_goal_alignment_energy() (loop_goal.c), which
 * matches keywords in the session's goal_text against JEPA action
 * categories.  Returns neutral 0.5 when the session has no goal.
 */

#include <anx/ebm.h>
#include <anx/loop.h>
#include <anx/state_object.h>
#include <anx/string.h>

float ebm_goal_alignment(anx_oid_t session_oid, anx_oid_t proposal_oid)
{
	struct anx_loop_session        *s;
	struct anx_loop_proposal_payload pp;
	struct anx_object_handle         h;
	int                              rc;

	s = anx_loop_session_get(session_oid);
	if (!s)
		return 0.5f;

	rc = anx_so_open(&proposal_oid, ANX_OPEN_READ, &h);
	if (rc != ANX_OK)
		return 0.5f;
	rc = anx_so_read_payload(&h, 0, &pp, sizeof(pp));
	anx_so_close(&h);
	if (rc < 0 || (uint32_t)rc < sizeof(pp))
		return 0.5f;

	return anx_loop_goal_alignment_energy(s->goal_text, pp.action_id);
}
