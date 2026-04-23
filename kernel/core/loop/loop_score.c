/*
 * loop_score.c — ANX_OBJ_SCORE creation (RFC-0020 Phase 2).
 *
 * Score objects are created by EBM cells and attached to proposals.
 * Each score captures one energy function's verdict on one proposal.
 */

#include <anx/loop.h>
#include <anx/state_object.h>
#include <anx/types.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include "loop_internal.h"

int anx_loop_score_create(anx_oid_t session_oid, anx_oid_t target_oid,
			  const char *scorer_id, float scalar,
			  enum anx_loop_score_class threshold_class,
			  float confidence, anx_oid_t *score_oid_out)
{
	struct anx_loop_score_payload payload;
	struct anx_so_create_params   cp;
	struct anx_state_object      *obj;
	int rc;

	if (!scorer_id || !score_oid_out)
		return ANX_EINVAL;

	anx_memset(&payload, 0, sizeof(payload));
	payload.session_oid     = session_oid;
	payload.target_oid      = target_oid;
	payload.scalar          = scalar;
	payload.threshold_class = threshold_class;
	payload.confidence      = confidence;
	payload.component_count = 0;
	anx_strlcpy(payload.scorer_id, scorer_id, sizeof(payload.scorer_id));

	anx_memset(&cp, 0, sizeof(cp));
	cp.object_type    = ANX_OBJ_SCORE;
	cp.schema_uri     = "anx:schema/loop/score/v1";
	cp.schema_version = "1";
	cp.payload        = &payload;
	cp.payload_size   = sizeof(payload);

	rc = anx_so_create(&cp, &obj);
	if (rc != ANX_OK) {
		kprintf("[loop_score] so_create failed (%d)\n", rc);
		return rc;
	}

	*score_oid_out = obj->oid;
	return ANX_OK;
}
