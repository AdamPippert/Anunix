/*
 * runtime.c — Execution Cell Runtime pipeline.
 *
 * Implements the cell execution pipeline:
 *   admit → plan → queue → execute → validate → commit
 *
 * This initial implementation is synchronous and single-threaded.
 * Routing, decomposition, and network execution are stubbed for
 * later phases.
 */

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/cell_plan.h>
#include <anx/cell_trace.h>
#include <anx/arch.h>
#include <anx/kprintf.h>

/* --- Admission --- */

static int runtime_admit(struct anx_cell *cell, struct anx_cell_trace *trace)
{
	int ret;

	/* Policy checks would go here (credentials, engine allowlist, etc.) */

	ret = anx_cell_transition(cell, ANX_CELL_ADMITTED);
	if (ret != ANX_OK)
		return ret;

	anx_trace_append(trace, ANX_TRACE_ADMITTED, "cell admitted", ANX_OK);
	return ANX_OK;
}

/* --- Planning --- */

static int runtime_plan(struct anx_cell *cell, struct anx_cell_trace *trace,
			struct anx_cell_plan **plan_out)
{
	struct anx_cell_plan *plan;
	int ret;

	ret = anx_cell_transition(cell, ANX_CELL_PLANNING);
	if (ret != ANX_OK)
		return ret;

	ret = anx_plan_create(&cell->cid, &plan);
	if (ret != ANX_OK)
		return ret;

	/*
	 * Minimal plan: one direct execution step, one validation
	 * step, one commit step. Real routing/decomposition comes
	 * in Phase 4.
	 */
	anx_plan_add_step(plan, ANX_STEP_DIRECT_EXEC, "direct execution");

	if (cell->validation.mode != ANX_VALIDATE_NONE)
		anx_plan_add_step(plan, ANX_STEP_VALIDATION, "validate output");

	if (cell->commit.persist_outputs)
		anx_plan_add_step(plan, ANX_STEP_COMMIT, "commit outputs");

	ret = anx_plan_finalize(plan);
	if (ret != ANX_OK) {
		anx_plan_destroy(plan);
		return ret;
	}

	cell->plan_id = plan->plan_id;

	ret = anx_cell_transition(cell, ANX_CELL_PLANNED);
	if (ret != ANX_OK) {
		anx_plan_destroy(plan);
		return ret;
	}

	anx_trace_append(trace, ANX_TRACE_PLAN_GENERATED,
			 "plan generated", ANX_OK);

	*plan_out = plan;
	return ANX_OK;
}

/* --- Execution --- */

static int runtime_execute(struct anx_cell *cell,
			   struct anx_cell_trace *trace,
			   struct anx_cell_plan *plan)
{
	int ret;

	/* Transition through queued to running */
	ret = anx_cell_transition(cell, ANX_CELL_QUEUED);
	if (ret != ANX_OK)
		return ret;

	ret = anx_cell_transition(cell, ANX_CELL_RUNNING);
	if (ret != ANX_OK)
		return ret;

	cell->started_at = arch_time_now();
	cell->attempt_count++;

	/*
	 * Walk plan steps. For now, direct execution is a no-op —
	 * the real engine dispatch comes in Phase 4 (routing).
	 */
	while (plan->current_step < plan->step_count) {
		struct anx_plan_step *step;

		step = &plan->steps[plan->current_step];

		anx_trace_append(trace, ANX_TRACE_STEP_STARTED,
				 step->description, ANX_OK);

		switch (step->kind) {
		case ANX_STEP_DIRECT_EXEC:
			/*
			 * Stub: real engine dispatch will go here.
			 * For now, execution "succeeds" immediately.
			 */
			break;

		case ANX_STEP_CHILD_CELL:
			/* Decomposition — Phase 4 */
			break;

		case ANX_STEP_VALIDATION:
			/* Handled separately in runtime_validate */
			break;

		case ANX_STEP_COMMIT:
			/* Handled separately in runtime_commit */
			break;
		}

		anx_trace_append(trace, ANX_TRACE_STEP_COMPLETED,
				 step->description, ANX_OK);

		if (anx_plan_advance(plan) == ANX_ENOENT)
			break;
	}

	return ANX_OK;
}

/* --- Validation --- */

static int runtime_validate(struct anx_cell *cell,
			    struct anx_cell_trace *trace)
{
	int ret;

	if (cell->validation.mode == ANX_VALIDATE_NONE) {
		anx_trace_append(trace, ANX_TRACE_VALIDATION_PASS,
				 "validation skipped (none)", ANX_OK);
		return ANX_OK;
	}

	ret = anx_cell_transition(cell, ANX_CELL_VALIDATING);
	if (ret != ANX_OK)
		return ret;

	/*
	 * Stub: schema/type/content validation would run here.
	 * For now, validation always passes.
	 */

	anx_trace_append(trace, ANX_TRACE_VALIDATION_PASS,
			 "validation passed", ANX_OK);
	return ANX_OK;
}

/* --- Commit --- */

static int runtime_commit(struct anx_cell *cell,
			  struct anx_cell_trace *trace)
{
	int ret;

	ret = anx_cell_transition(cell, ANX_CELL_COMMITTING);
	if (ret != ANX_OK)
		return ret;

	/*
	 * Stub: persist output State Objects and promote to memory.
	 * Real commit writes come when Memory Control Plane is wired.
	 */

	anx_trace_append(trace, ANX_TRACE_COMMITTED,
			 "outputs committed", ANX_OK);
	return ANX_OK;
}

/* --- Public API --- */

int anx_cell_run(struct anx_cell *cell)
{
	struct anx_cell_plan *plan = NULL;
	struct anx_cell_trace *trace = NULL;
	anx_oid_t trace_oid;
	int ret;

	if (!cell)
		return ANX_EINVAL;

	ret = anx_trace_create(&cell->cid, &trace);
	if (ret != ANX_OK)
		return ret;

	cell->trace_id = trace->trace_id;
	anx_trace_append(trace, ANX_TRACE_CREATED, "cell run started", ANX_OK);

	/* Admission */
	ret = runtime_admit(cell, trace);
	if (ret != ANX_OK)
		goto fail;

	/* Planning */
	ret = runtime_plan(cell, trace, &plan);
	if (ret != ANX_OK)
		goto fail;

	/* Execution */
	ret = runtime_execute(cell, trace, plan);
	if (ret != ANX_OK)
		goto fail;

	/* Validation */
	ret = runtime_validate(cell, trace);
	if (ret != ANX_OK)
		goto fail;

	/* Commit */
	ret = runtime_commit(cell, trace);
	if (ret != ANX_OK)
		goto fail;

	/* Success */
	ret = anx_cell_transition(cell, ANX_CELL_COMPLETED);
	if (ret != ANX_OK)
		goto fail;

	cell->completed_at = arch_time_now();
	anx_trace_append(trace, ANX_TRACE_COMPLETED, "cell completed", ANX_OK);

	/* Finalize trace into a State Object */
	if (cell->commit.write_trace)
		anx_trace_finalize(trace, &trace_oid);

	anx_plan_destroy(plan);
	anx_trace_destroy(trace);
	return ANX_OK;

fail:
	cell->error_code = ret;
	anx_cell_transition(cell, ANX_CELL_FAILED);
	anx_trace_append(trace, ANX_TRACE_FAILED, "cell failed", ret);

	if (cell->commit.write_trace)
		anx_trace_finalize(trace, &trace_oid);

	if (plan)
		anx_plan_destroy(plan);
	anx_trace_destroy(trace);
	return ret;
}

int anx_cell_cancel(struct anx_cell *cell)
{
	if (!cell)
		return ANX_EINVAL;

	return anx_cell_transition(cell, ANX_CELL_CANCELLED);
}

int anx_cell_derive_child(struct anx_cell *parent,
			  enum anx_cell_type type,
			  const struct anx_cell_intent *intent,
			  struct anx_cell **child_out)
{
	struct anx_cell *child;
	int ret;

	if (!parent || !child_out)
		return ANX_EINVAL;

	/* Enforce recursion depth */
	if (parent->recursion_depth >= parent->execution.max_recursion_depth)
		return ANX_EPERM;

	/* Enforce child cell limit */
	if (parent->child_count >= ANX_MAX_CHILD_CELLS)
		return ANX_ENOMEM;

	ret = anx_cell_create(type, intent, &child);
	if (ret != ANX_OK)
		return ret;

	/* Wire up lineage */
	child->parent_cid = parent->cid;
	child->recursion_depth = parent->recursion_depth + 1;

	/* Inherit stricter policies from parent */
	child->constraints = parent->constraints;
	child->execution = parent->execution;

	/* Record in parent */
	parent->child_cids[parent->child_count] = child->cid;
	parent->child_count++;

	*child_out = child;
	return ANX_OK;
}
