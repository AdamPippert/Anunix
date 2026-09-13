#ifndef ANX_OPTIMIZATION_HARNESS_H
#define ANX_OPTIMIZATION_HARNESS_H

#include <anx/tuning.h>

#define ANX_ROUTE_HARNESS_CASES 4U
#define ANX_ROUTE_HARNESS_RECEIPTS 32U
#define ANX_ROUTE_HARNESS_SCHEMA "anx:evaluation/route-harness/v1"

struct anx_route_evaluation {
	uint32_t schema;
	uint8_t action_digest[32];
	uint32_t case_count;
	uint32_t failure_count;
	uint32_t observed_winner[ANX_ROUTE_HARNESS_CASES];
};

/* Trusted harness execution; completed failures also produce sealed receipts. */
int anx_route_evaluate(const struct anx_route_tuning_action *action, anx_oid_t *receipt_out);
/* Internal gate checks private issuance, complete action binding, and the result. */
int anx_route_evaluation_check(const struct anx_route_tuning_action *action,
			       const struct anx_route_artifact_ref *receipt);
/* Revoke the in-memory issuance record; the sealed object remains for audit. */
int anx_route_evaluation_release(const anx_oid_t *receipt);
/* Compiled policy tasks require a current, privately issued passing evaluation. */
int anx_route_evaluated_action_check(const struct anx_route_tuning_action *action);

#endif
