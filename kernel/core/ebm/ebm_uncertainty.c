/*
 * ebm_uncertainty.c — Epistemic uncertainty energy scorer (RFC-0020 Phase 3).
 *
 * Reads the uncertainty field from the session's current belief state.
 * High uncertainty → high energy (system doesn't trust its own belief
 * about the world, so proposals derived from it are less reliable).
 */

#include <anx/ebm.h>
#include <anx/loop.h>
#include <anx/state_object.h>
#include <anx/string.h>

float ebm_epistemic_uncertainty(anx_oid_t session_oid,
				anx_oid_t proposal_oid __attribute__((unused)))
{
	struct anx_loop_session      *s;
	struct anx_loop_belief_payload bp;
	struct anx_object_handle       h;
	int                            rc;

	s = anx_loop_session_get(session_oid);
	if (!s)
		return 0.5f;

	rc = anx_so_open(&s->active_belief, ANX_OPEN_READ, &h);
	if (rc != ANX_OK)
		return 0.5f;
	rc = anx_so_read_payload(&h, 0, &bp, sizeof(bp));
	anx_so_close(&h);
	if (rc < 0 || (uint32_t)rc < sizeof(bp))
		return 0.5f;

	/* uncertainty field is already in [0, 1] */
	return bp.uncertainty;
}
