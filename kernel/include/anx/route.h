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

/* --- Route result --- */

struct anx_route_result {
	struct anx_route_candidate candidates[ANX_MAX_ROUTE_CANDIDATES];
	uint32_t candidate_count;
	uint32_t selected_index;	/* index of best candidate */
};

/* --- Route Planner API --- */

/* Initialize the route planner */
void anx_route_planner_init(void);

/*
 * Plan a route for a cell. Produces scored candidates.
 * The caller selects the winner from result->selected_index.
 */
int anx_route_plan(struct anx_cell *cell, struct anx_route_result *result);

/*
 * Score a single engine against a cell's requirements.
 * Returns a score (higher = better fit), or negative on error.
 */
int32_t anx_route_score_engine(struct anx_cell *cell,
			       struct anx_engine *engine);

#endif /* ANX_ROUTE_H */
