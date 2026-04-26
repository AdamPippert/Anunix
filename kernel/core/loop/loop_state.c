/*
 * loop_state.c — IBAL EBM weight persistence and run helper (RFC-0020 Phase 9).
 *
 * anx_ibal_save_state() captures current per-action energy statistics into
 * an ANX_OBJ_MEMORY_CONSOLIDATION state object.  anx_ibal_load_state() reads
 * them back.  anx_ibal_run() wraps the session create + advance loop into
 * a single blocking call.
 */

#include <anx/ibal.h>
#include <anx/cexl.h>
#include <anx/diag.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include "loop_internal.h"

/* ------------------------------------------------------------------ */
/* Payload                                                             */
/* ------------------------------------------------------------------ */

#define IBAL_STATE_MAGIC   0x4942414CU	/* "IBAL" */
#define IBAL_STATE_VERSION 1U

struct ibal_state_payload {
	uint32_t magic;
	uint32_t version;
	uint32_t n_weights;
	float    weights[ANX_EBM_MAX_SCORERS];
	struct anx_loop_action_stats action_stats[ANX_MEMORY_ACT_COUNT];
};

/* ------------------------------------------------------------------ */
/* Phase 9: save                                                       */
/* ------------------------------------------------------------------ */

int anx_ibal_save_state(anx_oid_t *oid_out)
{
	struct ibal_state_payload  pay;
	struct anx_so_create_params cp;
	struct anx_state_object    *obj;
	int rc;

	if (!oid_out)
		return ANX_EINVAL;

	anx_memset(&pay, 0, sizeof(pay));
	pay.magic     = IBAL_STATE_MAGIC;
	pay.version   = IBAL_STATE_VERSION;
	pay.n_weights = ANX_EBM_MAX_SCORERS;
	/* weights[] and action_stats[] remain zeroed — no trained weights yet */

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_MEMORY_CONSOLIDATION;
	cp.schema_uri     = "anx:schema/ibal/state/v1";
	cp.schema_version = "1";
	cp.payload        = &pay;
	cp.payload_size   = sizeof(pay);

	rc = anx_so_create(&cp, &obj);
	if (rc != ANX_OK) {
		kprintf("[ibal] save_state: so_create failed (%d)\n", rc);
		return rc;
	}

	*oid_out = obj->oid;

	rc = anx_so_seal(oid_out);
	if (rc != ANX_OK) {
		kprintf("[ibal] save_state: seal failed (%d)\n", rc);
		return rc;
	}

	kprintf("[ibal] state saved: %016llx\n",
		(unsigned long long)oid_out->lo);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Phase 9: load                                                       */
/* ------------------------------------------------------------------ */

int anx_ibal_load_state(anx_oid_t state_oid)
{
	struct anx_object_handle   handle;
	struct ibal_state_payload  pay;
	int rc;

	rc = anx_so_open(&state_oid, ANX_OPEN_READ, &handle);
	if (rc != ANX_OK)
		return rc;

	rc = anx_so_read_payload(&handle, 0, &pay, sizeof(pay));
	anx_so_close(&handle);

	if (rc < 0)
		return rc;
	if ((uint32_t)rc < sizeof(pay))
		return ANX_EINVAL;
	if (pay.magic != IBAL_STATE_MAGIC || pay.version != IBAL_STATE_VERSION)
		return ANX_EINVAL;

	kprintf("[ibal] state loaded: %u weights, %u action slots\n",
		pay.n_weights, ANX_MEMORY_ACT_COUNT);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Convenience run wrapper                                             */
/* ------------------------------------------------------------------ */

int anx_ibal_run(const struct anx_loop_create_params *params,
		 anx_oid_t *session_oid_out)
{
	anx_oid_t sid;
	struct anx_loop_session_info info;
	int rc;

	if (!params || !session_oid_out)
		return ANX_EINVAL;

	(void)anx_trace_begin("ibal.session");

	rc = anx_loop_session_create(params, &sid);
	if (rc != ANX_OK) {
		(void)anx_trace_end("ibal.session");
		return rc;
	}

	do {
		rc = anx_loop_session_advance(sid);
		if (rc != ANX_OK)
			break;
		rc = anx_loop_session_status_get(sid, &info);
		if (rc != ANX_OK)
			break;
	} while (info.status == ANX_LOOP_RUNNING);

	/* Emit a fault artifact for unexpectedly aborted sessions */
	if (anx_loop_session_status_get(sid, &info) == ANX_OK &&
	    info.status == ANX_LOOP_ABORTED) {
		anx_diag_fault("ibal.abort", (uint64_t)sid.lo,
			       "loop", "session aborted unexpectedly");
	}

	*session_oid_out = sid;

	/* Post-session pipeline: counterexample signal + JEPA observation */
	{
		const char *world = params->world_uri[0]
			? params->world_uri : "anx:world/os-default";
		anx_loop_cexl_process(sid, world);
		anx_loop_jepa_ingest(sid, world);
	}

	(void)anx_trace_end("ibal.session");

	return ANX_OK;
}
