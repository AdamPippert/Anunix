/*
 * loop_belief.c — ANX_OBJ_BELIEF_STATE creation and management (RFC-0020 Phase 2).
 *
 * A belief state captures the system's current understanding at one
 * loop iteration: it observes the OS, encodes the observation through
 * JEPA, and stores both the raw observation and its latent vector as
 * content-addressed State Objects.
 *
 * anx_loop_belief_create() is the main entry point; called once per
 * iteration by the recurrent update cell.
 */

#include <anx/loop.h>
#include <anx/jepa.h>
#include <anx/state_object.h>
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "loop_internal.h"

/* ------------------------------------------------------------------ */
/* OID generation                                                      */
/* ------------------------------------------------------------------ */

static uint64_t g_belief_oid_seq;

static anx_oid_t __attribute__((unused)) belief_oid_generate(void)
{
	anx_oid_t oid;

	oid.hi = 0x414e584c424c4946ULL;	/* "ANXLBLIF" */
	oid.lo = ++g_belief_oid_seq;
	return oid;
}

/* ------------------------------------------------------------------ */
/* Belief state creation                                               */
/* ------------------------------------------------------------------ */

int anx_loop_belief_create(anx_oid_t session_oid, uint32_t iteration,
			   anx_oid_t parent_belief_oid,
			   anx_oid_t *belief_oid_out)
{
	struct anx_jepa_obs      obs;
	anx_oid_t                obs_oid;
	anx_oid_t                latent_oid;
	struct anx_loop_belief_payload payload;
	struct anx_so_create_params    cp;
	struct anx_state_object       *obj;
	int rc;

	if (!belief_oid_out)
		return ANX_EINVAL;

	/* Step 1: collect current system observation */
	rc = anx_jepa_observe(&obs);
	if (rc != ANX_OK) {
		if (anx_jepa_available()) {
			kprintf("[loop_belief] observe failed (%d)\n", rc);
			return rc;
		}
		/* JEPA unavailable: continue with zero observation */
		anx_memset(&obs, 0, sizeof(obs));
	}

	/* Step 2: persist observation as ANX_OBJ_JEPA_OBS */
	if (anx_jepa_available()) {
		rc = anx_jepa_observe_store(&obs, &obs_oid);
		if (rc != ANX_OK) {
			kprintf("[loop_belief] observe_store failed (%d)\n", rc);
			return rc;
		}
	} else {
		anx_memset(&obs_oid, 0, sizeof(obs_oid));
	}

	/* Step 3: encode → ANX_OBJ_JEPA_LATENT */
	rc = anx_jepa_encode(&obs_oid, &latent_oid);
	if (rc != ANX_OK) {
		/*
		 * JEPA unavailable or not yet trained; proceed with a
		 * zero latent OID so the loop can still run structurally.
		 */
		kprintf("[loop_belief] encode unavailable, using zero latent\n");
		anx_memset(&latent_oid, 0, sizeof(latent_oid));
	}

	/* Step 4: build the belief payload */
	anx_memset(&payload, 0, sizeof(payload));
	payload.session_oid      = session_oid;
	payload.iteration        = iteration;
	payload.latent_oid       = latent_oid;
	payload.obs_oid          = obs_oid;
	payload.parent_belief_oid = parent_belief_oid;
	/*
	 * Phase 3 uncertainty: 0.3 when JEPA encoder is live (we have a
	 * real latent), 0.8 when JEPA is unavailable (structural run only).
	 * A future pass will refine this using prediction variance across
	 * the full action vocabulary.
	 */
	payload.uncertainty = anx_jepa_available() ? 0.3f : 0.8f;
	payload.context_count     = 0;

	/* Step 5: store as ANX_OBJ_BELIEF_STATE */
	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type  = ANX_OBJ_BELIEF_STATE;
	cp.schema_uri   = "anx:schema/loop/belief-state/v1";
	cp.schema_version = "1";
	cp.payload      = &payload;
	cp.payload_size = sizeof(payload);

	rc = anx_so_create(&cp, &obj);
	if (rc != ANX_OK) {
		kprintf("[loop_belief] so_create failed (%d)\n", rc);
		return rc;
	}

	/* Use a deterministic OID so EBM can look this back up */
	*belief_oid_out = obj->oid;

	/* Step 6: update the session's active belief */
	anx_loop_session_set_belief(session_oid, *belief_oid_out);

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Latent retrieval                                                    */
/* ------------------------------------------------------------------ */

int anx_loop_belief_get_latent(anx_oid_t belief_oid,
			       anx_oid_t *latent_oid_out)
{
	struct anx_object_handle    handle;
	struct anx_loop_belief_payload payload;
	int rc;

	if (!latent_oid_out)
		return ANX_EINVAL;

	rc = anx_so_open(&belief_oid, ANX_OPEN_READ, &handle);
	if (rc != ANX_OK)
		return rc;

	rc = anx_so_read_payload(&handle, 0, &payload, sizeof(payload));
	anx_so_close(&handle);

	if (rc < (int)sizeof(payload))
		return ANX_EIO;

	*latent_oid_out = payload.latent_oid;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Uncertainty retrieval                                               */
/* ------------------------------------------------------------------ */

int anx_loop_belief_get_uncertainty(anx_oid_t belief_oid,
				    float *uncertainty_out)
{
	struct anx_object_handle    handle;
	struct anx_loop_belief_payload payload;
	int rc;

	if (!uncertainty_out)
		return ANX_EINVAL;

	rc = anx_so_open(&belief_oid, ANX_OPEN_READ, &handle);
	if (rc != ANX_OK)
		return rc;

	rc = anx_so_read_payload(&handle, 0, &payload, sizeof(payload));
	anx_so_close(&handle);

	if (rc < (int)sizeof(payload))
		return ANX_EIO;

	*uncertainty_out = payload.uncertainty;
	return ANX_OK;
}
