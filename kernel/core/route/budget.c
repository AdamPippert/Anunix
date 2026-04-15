/*
 * budget.c — Budget profiles for route cost management.
 *
 * Three built-in profiles with sensible defaults.
 */

#include <anx/types.h>
#include <anx/budget.h>

static const struct anx_budget_profile profiles[] = {
	[ANX_BUDGET_INTERACTIVE_PRIVATE] = {
		.id = ANX_BUDGET_INTERACTIVE_PRIVATE,
		.version = 1,
		.w_latency = 80,
		.w_cost = 20,
		.w_confidence = 50,
		.w_locality = 90,
		.w_privacy = 100,
		.max_latency_ms = 3000,
		.max_cost_ucents = 50000,	/* $0.50 */
		.allow_remote = false,
	},
	[ANX_BUDGET_BACKGROUND_ENRICHMENT] = {
		.id = ANX_BUDGET_BACKGROUND_ENRICHMENT,
		.version = 1,
		.w_latency = 10,
		.w_cost = 90,
		.w_confidence = 40,
		.w_locality = 30,
		.w_privacy = 50,
		.max_latency_ms = 60000,
		.max_cost_ucents = 10000,	/* $0.10 */
		.allow_remote = true,
	},
	[ANX_BUDGET_CRITICAL_DECISION] = {
		.id = ANX_BUDGET_CRITICAL_DECISION,
		.version = 1,
		.w_latency = 30,
		.w_cost = 10,
		.w_confidence = 100,
		.w_locality = 40,
		.w_privacy = 70,
		.max_latency_ms = 30000,
		.max_cost_ucents = 500000,	/* $5.00 */
		.allow_remote = true,
	},
};

const struct anx_budget_profile *anx_budget_get(enum anx_budget_profile_id id)
{
	if ((int)id < 0 || id >= ANX_BUDGET_CRITICAL_DECISION + 1)
		return NULL;
	return &profiles[id];
}

bool anx_budget_exceeded(const struct anx_budget_profile *budget,
			 const struct anx_route_cost *cost)
{
	if (!budget || !cost)
		return true;

	/* Check hard caps */
	if (budget->max_latency_ms > 0) {
		uint64_t latency_ms = cost->latency_ns / 1000000;

		if (latency_ms > budget->max_latency_ms)
			return true;
	}

	if (budget->max_cost_ucents > 0 &&
	    cost->cost_ucents > budget->max_cost_ucents)
		return true;

	return false;
}
