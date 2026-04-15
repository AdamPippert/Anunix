/*
 * anx/route_feedback.h — Route outcome recording.
 *
 * Records outcome signals from completed routes to enable
 * future route improvement. Stored as a ring buffer of the
 * last N feedback records.
 */

#ifndef ANX_ROUTE_FEEDBACK_H
#define ANX_ROUTE_FEEDBACK_H

#include <anx/types.h>

/* --- Route outcome --- */

enum anx_route_outcome {
	ANX_ROUTE_SUCCESS,
	ANX_ROUTE_PARTIAL,
	ANX_ROUTE_FALLBACK_USED,
	ANX_ROUTE_FAILED,
};

/* --- Route feedback record --- */

struct anx_route_feedback {
	anx_pid_t plan_ref;
	anx_eid_t engine_id;

	/* Observed outcomes */
	uint64_t observed_latency_ns;
	uint32_t observed_cost_ucents;
	uint32_t tokens_used;
	bool validation_passed;
	uint32_t retry_count;

	enum anx_route_outcome outcome;
	anx_time_t recorded_at;
};

/* --- Feedback API --- */

/* Initialize the feedback subsystem */
void anx_route_feedback_init(void);

/* Record a route outcome */
int anx_route_record_feedback(const struct anx_route_feedback *fb);

/* Query recent feedback for an engine */
int anx_route_get_feedback(const anx_eid_t *engine_id,
			   struct anx_route_feedback *out,
			   uint32_t max_results,
			   uint32_t *found);

#endif /* ANX_ROUTE_FEEDBACK_H */
