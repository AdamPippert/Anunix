/*
 * ebm_world.c — World consistency energy scorer (RFC-0020 Phase 3).
 *
 * Measures how far the proposal's predicted latent diverges from the
 * current belief latent using anx_jepa_divergence().  High divergence
 * means the proposed world state is unexpected → high energy (bad).
 */

#include <anx/ebm.h>
#include <anx/loop.h>
#include <anx/jepa.h>
#include <anx/state_object.h>
#include <anx/string.h>

float ebm_world_consistency(anx_oid_t session_oid, anx_oid_t proposal_oid)
{
	struct anx_loop_session        *s;
	struct anx_loop_proposal_payload pp;
	struct anx_loop_belief_payload   bp;
	struct anx_object_handle         h;
	int                              rc;

	if (!anx_jepa_available())
		return 0.5f;	/* JEPA unavailable: neutral energy */

	/* Read proposal payload to get predicted latent OID */
	rc = anx_so_open(&proposal_oid, ANX_OPEN_READ, &h);
	if (rc != ANX_OK)
		return 0.5f;
	rc = anx_so_read_payload(&h, 0, &pp, sizeof(pp));
	anx_so_close(&h);
	if (rc < 0 || (uint32_t)rc < sizeof(pp))
		return 0.5f;

	/* Need the belief latent: get it from the session's active_belief */
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

	/* Divergence in [0,1]: 0=identical, 1=maximally different */
	{
		float d = anx_jepa_divergence(&pp.latent_oid, &bp.latent_oid);

		if (d < 0.0f)
			return 0.5f;	/* JEPA returned error */
		return d;
	}
}
