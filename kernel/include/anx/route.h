/*
 * anx/route.h — Routing Plane (RFC-0005 Sections 9-13).
 *
 * The Routing Plane evaluates intent, constraints, and available
 * engines to select candidate execution plans with scoring.
 */

#ifndef ANX_ROUTE_H
#define ANX_ROUTE_H

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/engine.h>

#define ANX_MAX_ROUTE_CANDIDATES	8

/* --- Route candidate --- */

struct anx_route_candidate {
	anx_eid_t engine_id;
	anx_eid_t fallback_engine_id;
	int32_t score;			/* higher is better */
	bool feasible;
	char reason[64];		/* why selected or rejected */
};

/* --- Route stages (staged routing) --- */

enum anx_route_stage {
	ANX_ROUTE_STAGE_KERNEL,		/* deterministic feasibility + scoring */
	ANX_ROUTE_STAGE_LOCAL_SVC,	/* semantic candidate generation */
	ANX_ROUTE_STAGE_RLM_PLANNER,	/* slow-path planning */
};

/* --- Route result --- */

#define ANX_ROUTE_ESCALATION_THRESHOLD	30

struct anx_route_result {
	struct anx_route_candidate candidates[ANX_MAX_ROUTE_CANDIDATES];
	uint32_t candidate_count;
	uint32_t selected_index;	/* index of best candidate */
	enum anx_route_stage decided_at;
	bool needs_escalation;		/* stage 2/3 recommended */
};

/* --- Route Planner API --- */

/* Initialize the route planner */
void anx_route_planner_init(void);

/*
 * Plan a route for a cell. Produces scored candidates.
 * The caller selects the winner from result->selected_index.
 */
int anx_route_plan(struct anx_cell *cell, struct anx_route_result *result);

/* Caller-owned placement hint for one compatible model backend pool.
 * Initialize the selected engine and placement count to zero. The caller
 * supplies unique eligible EIDs and owns logical model compatibility. */
struct anx_route_session {
	anx_eid_t eligible_engines[ANX_MAX_ROUTE_CANDIDATES];
	uint32_t engine_count;
	uint32_t required_caps;
	anx_eid_t selected_engine;
	uint64_t placement_count;
};

/* Reuse eligible affinity or select a scored fallback; errors preserve both outputs. */
int anx_route_plan_session(struct anx_cell *cell, struct anx_route_session *session,
			   struct anx_route_result *result);

#define ANX_CONTINUITY_MAX_HOLD_NS 30000000000ULL
#define ANX_CONTINUITY_COST_MAX_MS 3600000U

/* Caller-owned advice about a versioned state object in logical L0/L1 residency. */
struct anx_continuity_hint {
	uint32_t schema;
	anx_eid_t engine_id;
	anx_oid_t state_oid;
	uint64_t state_version;
	uint64_t created_at_ns;
	uint64_t expires_at_ns;
	uint32_t return_probability_permille;
	uint32_t restoration_cost_ms;
	uint32_t reservation_cost_ms;
	uint32_t interference_cost_ms;
};

/* Expiring state reuse replaces indefinite affinity for this placement call. */
int anx_route_plan_continuity(struct anx_cell *cell, struct anx_route_session *session,
			      const struct anx_continuity_hint *hint, struct anx_route_result *result);

/*
 * Score a single engine against a cell's requirements.
 * Valid scores may be negative. Invalid arguments return -1.
 */
int32_t anx_route_score_engine(struct anx_cell *cell,
			       struct anx_engine *engine);

struct anx_route_weight_policy;
/* Score against one copied policy throughout a complete routing decision. */
int32_t anx_route_score_with_policy(struct anx_cell *cell, struct anx_engine *engine,
				    const struct anx_route_weight_policy *policy);

#endif /* ANX_ROUTE_H */
