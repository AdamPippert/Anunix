/*
 * loop_proposal.c — ANX_OBJ_WORLD_PROPOSAL and ANX_OBJ_COUNTEREXAMPLE
 *                   management (RFC-0020 Phase 2).
 *
 * A world proposal wraps a JEPA-predicted latent (or LLM/retrieval
 * content) as a candidate world hypothesis for one loop iteration.
 * After EBM scoring, proposals are either selected, rejected, or
 * committed.  Rejected proposals become counterexamples.
 */

#include <anx/loop.h>
#include <anx/state_object.h>
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "loop_internal.h"

/* ------------------------------------------------------------------ */
/* World proposal creation                                             */
/* ------------------------------------------------------------------ */

int anx_loop_proposal_create_jepa(anx_oid_t session_oid, uint32_t iteration,
				  anx_oid_t predicted_latent_oid,
				  uint32_t action_id,
				  anx_oid_t *proposal_oid_out)
{
	struct anx_loop_proposal_payload payload;
	struct anx_so_create_params      cp;
	struct anx_state_object         *obj;
	int rc;

	if (!proposal_oid_out)
		return ANX_EINVAL;

	anx_memset(&payload, 0, sizeof(payload));
	payload.session_oid  = session_oid;
	payload.iteration    = iteration;
	payload.source       = ANX_LOOP_PROPOSAL_JEPA;
	payload.latent_oid   = predicted_latent_oid;
	payload.action_id    = action_id;
	payload.status       = ANX_LOOP_PROPOSAL_CANDIDATE;
	payload.score_count  = 0;

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_WORLD_PROPOSAL;
	cp.schema_uri     = "anx:schema/loop/world-proposal/v1";
	cp.schema_version = "1";
	cp.payload        = &payload;
	cp.payload_size   = sizeof(payload);

	rc = anx_so_create(&cp, &obj);
	if (rc != ANX_OK) {
		kprintf("[loop_proposal] so_create failed (%d)\n", rc);
		return rc;
	}

	*proposal_oid_out = obj->oid;

	/* Register with session */
	anx_loop_session_add_candidate(session_oid, *proposal_oid_out);

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Latent retrieval from proposal                                      */
/* ------------------------------------------------------------------ */

int anx_loop_proposal_get_latent(anx_oid_t proposal_oid,
				 anx_oid_t *latent_oid_out)
{
	struct anx_object_handle         handle;
	struct anx_loop_proposal_payload payload;
	int rc;

	if (!latent_oid_out)
		return ANX_EINVAL;

	rc = anx_so_open(&proposal_oid, ANX_OPEN_READ, &handle);
	if (rc != ANX_OK)
		return rc;

	rc = anx_so_read_payload(&handle, 0, &payload, sizeof(payload));
	anx_so_close(&handle);

	if (rc != ANX_OK)
		return rc;

	*latent_oid_out = payload.latent_oid;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Action ID retrieval from proposal                                   */
/* ------------------------------------------------------------------ */

int anx_loop_proposal_get_action_id(anx_oid_t proposal_oid,
				    uint32_t *action_id_out)
{
	struct anx_object_handle         handle;
	struct anx_loop_proposal_payload payload;
	int rc;

	if (!action_id_out)
		return ANX_EINVAL;

	rc = anx_so_open(&proposal_oid, ANX_OPEN_READ, &handle);
	if (rc != ANX_OK)
		return rc;

	rc = anx_so_read_payload(&handle, 0, &payload, sizeof(payload));
	anx_so_close(&handle);

	if (rc != ANX_OK)
		return rc;

	*action_id_out = payload.action_id;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Score attachment                                                    */
/* ------------------------------------------------------------------ */

int anx_loop_proposal_set_score(anx_oid_t proposal_oid, float aggregate)
{
	struct anx_object_handle         handle;
	struct anx_loop_proposal_payload payload;
	int rc;

	rc = anx_so_open(&proposal_oid, ANX_OPEN_READWRITE, &handle);
	if (rc != ANX_OK)
		return rc;

	rc = anx_so_read_payload(&handle, 0, &payload, sizeof(payload));
	if (rc >= (int)sizeof(payload)) {
		payload.aggregate_score = aggregate;
		rc = anx_so_write_payload(&handle, 0, &payload, sizeof(payload));
	} else {
		rc = ANX_EIO;
	}

	anx_so_close(&handle);
	return rc;
}

int anx_loop_proposal_add_score(anx_oid_t proposal_oid, anx_oid_t score_oid)
{
	struct anx_object_handle         handle;
	struct anx_loop_proposal_payload payload;
	int rc;

	rc = anx_so_open(&proposal_oid, ANX_OPEN_READWRITE, &handle);
	if (rc != ANX_OK)
		return rc;

	rc = anx_so_read_payload(&handle, 0, &payload, sizeof(payload));
	if (rc >= (int)sizeof(payload)) {
		if (payload.score_count < ANX_LOOP_PROPOSAL_MAX_SCORES) {
			payload.score_oids[payload.score_count++] = score_oid;
			rc = anx_so_write_payload(&handle, 0, &payload,
						  sizeof(payload));
		} else {
			rc = ANX_ENOMEM;
		}
	} else {
		rc = ANX_EIO;
	}

	anx_so_close(&handle);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Counterexample recording                                            */
/* ------------------------------------------------------------------ */

int anx_loop_counterexample_record(anx_oid_t session_oid,
				   anx_oid_t rejected_proposal_oid,
				   uint32_t reason, float rejection_score,
				   const char *context_summary)
{
	struct anx_loop_counterexample_payload payload;
	struct anx_so_create_params            cp;
	struct anx_state_object               *obj;

	anx_memset(&payload, 0, sizeof(payload));
	payload.session_oid    = session_oid;
	payload.rejected_oid   = rejected_proposal_oid;
	payload.reason         = reason;
	payload.rejection_score = rejection_score;

	if (context_summary)
		anx_strlcpy(payload.context_summary, context_summary,
			    sizeof(payload.context_summary));

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_COUNTEREXAMPLE;
	cp.schema_uri     = "anx:schema/loop/counterexample/v1";
	cp.schema_version = "1";
	cp.payload        = &payload;
	cp.payload_size   = sizeof(payload);

	/* Non-fatal: counterexample storage failure should not halt the loop */
	if (anx_so_create(&cp, &obj) == ANX_OK) {
		unsigned int rs_i = (unsigned int)rejection_score;
		unsigned int rs_f = (unsigned int)(
			(rejection_score - (float)rs_i) * 1000.0f + 0.5f);

		kprintf("[loop] counterexample stored: reason=%u score=%u.%03u\n",
			reason, rs_i, rs_f);
	}

	return ANX_OK;
}
