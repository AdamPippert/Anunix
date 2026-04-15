/*
 * anx/budget.h — Budget profiles for route cost management.
 *
 * Named profiles define acceptable tradeoffs between latency,
 * cost, confidence, locality, and privacy. Three built-in
 * profiles cover common workload patterns.
 */

#ifndef ANX_BUDGET_H
#define ANX_BUDGET_H

#include <anx/types.h>

/* --- Budget profile identifiers --- */

enum anx_budget_profile_id {
	ANX_BUDGET_INTERACTIVE_PRIVATE,		/* low latency, local only */
	ANX_BUDGET_BACKGROUND_ENRICHMENT,	/* cost-aggressive, batch */
	ANX_BUDGET_CRITICAL_DECISION,		/* quality over cost */
	ANX_BUDGET_CUSTOM,
	ANX_BUDGET_PROFILE_COUNT,
};

/* --- Budget profile --- */

struct anx_budget_profile {
	enum anx_budget_profile_id id;
	uint32_t version;

	/* Scoring weights (0-100) */
	uint32_t w_latency;
	uint32_t w_cost;
	uint32_t w_confidence;
	uint32_t w_locality;
	uint32_t w_privacy;

	/* Hard caps */
	uint64_t max_latency_ms;
	uint32_t max_cost_ucents;	/* micro-cents */
	bool allow_remote;
};

/* --- Route cost accounting --- */

struct anx_route_cost {
	uint64_t latency_ns;
	uint32_t tokens_input;
	uint32_t tokens_output;
	uint32_t cost_ucents;
	uint32_t compute_ms;
};

/* --- Budget API --- */

/* Get a built-in budget profile */
const struct anx_budget_profile *anx_budget_get(enum anx_budget_profile_id id);

/* Check if a route cost exceeds a budget's hard caps */
bool anx_budget_exceeded(const struct anx_budget_profile *budget,
			 const struct anx_route_cost *cost);

#endif /* ANX_BUDGET_H */
