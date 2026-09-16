/* Native evidence and decision records for workflow capability promotion. */
#ifndef ANX_PROMOTION_EVIDENCE_H
#define ANX_PROMOTION_EVIDENCE_H

#include <anx/capability.h>

#define ANX_PROMOTION_EVIDENCE_MAGIC 0x41504556U
#define ANX_PROMOTION_DECISION_MAGIC 0x41504443U
#define ANX_PROMOTION_EVIDENCE_SCHEMA "anx:schema/promotion-evidence/v1"
#define ANX_PROMOTION_DECISION_SCHEMA "anx:schema/promotion-decision/v1"

struct anx_promotion_evidence {
	uint32_t magic;
	uint32_t version;
	anx_oid_t candidate_oid;
	anx_oid_t incumbent_oid;
	uint32_t candidates_tried;
	struct anx_promotion_trial trial;
};

struct anx_promotion_decision {
	uint32_t magic;
	uint32_t version;
	anx_oid_t evidence_oid;
	struct anx_promotion_evidence evidence;
	int32_t result;
};

/* Trusted kernel API: callers must authorize capability installation first.
 * Valid sealed evidence produces a sealed decision, including gate failures. */
int anx_cap_install_evidence(const anx_oid_t *evidence_oid,
			     anx_oid_t *decision_oid_out);

#endif
