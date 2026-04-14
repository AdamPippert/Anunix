/*
 * anx/cell_plan.h — Execution Cell Plan (RFC-0003 Section 18).
 *
 * A Cell Plan is the executable plan produced after decomposition
 * and routing. It specifies substeps, engine assignments, and
 * fallback paths.
 */

#ifndef ANX_CELL_PLAN_H
#define ANX_CELL_PLAN_H

#include <anx/types.h>

#define ANX_MAX_PLAN_STEPS	16

/* --- Plan step kinds --- */

enum anx_plan_step_kind {
	ANX_STEP_DIRECT_EXEC,		/* execute directly with engine */
	ANX_STEP_CHILD_CELL,		/* spawn a child cell */
	ANX_STEP_VALIDATION,		/* run validation */
	ANX_STEP_COMMIT,		/* commit outputs */
};

/* --- Plan step --- */

struct anx_plan_step {
	uint32_t step_index;
	enum anx_plan_step_kind kind;
	anx_eid_t assigned_engine;	/* engine to use (nil = unassigned) */
	anx_eid_t fallback_engine;	/* fallback engine (nil = none) */
	char description[128];
};

/* --- The Cell Plan --- */

struct anx_cell_plan {
	anx_pid_t plan_id;
	anx_cid_t cell_ref;		/* cell this plan belongs to */

	struct anx_plan_step steps[ANX_MAX_PLAN_STEPS];
	uint32_t step_count;
	uint32_t current_step;

	anx_time_t created_at;
	bool finalized;
};

/* --- Plan API --- */

/* Create a plan for a cell */
int anx_plan_create(const anx_cid_t *cell_ref, struct anx_cell_plan **out);

/* Add a step to a plan */
int anx_plan_add_step(struct anx_cell_plan *plan,
		      enum anx_plan_step_kind kind,
		      const char *description);

/* Finalize a plan (no more steps can be added) */
int anx_plan_finalize(struct anx_cell_plan *plan);

/* Advance to the next step. Returns ANX_ENOENT when done. */
int anx_plan_advance(struct anx_cell_plan *plan);

/* Destroy a plan */
void anx_plan_destroy(struct anx_cell_plan *plan);

#endif /* ANX_CELL_PLAN_H */
