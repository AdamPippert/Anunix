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
#include <anx/route.h>
#include <anx/engine.h>
#include <anx/model_server.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/external_call.h>
#include <anx/sched.h>

/* --- Admission --- */

static bool runtime_needs_trace(const struct anx_cell *cell)
{
	return cell->cell_type == ANX_CELL_TASK_EXTERNAL_CALL || cell->commit.write_trace;
}

static void runtime_trace_created(struct anx_cell_trace *trace, const struct anx_cell *cell)
{
	char name[sizeof(cell->intent.name)];

	/* Retain the entry-time intent even if a handler changes or destroys it. */
	anx_memcpy(name, cell->intent.name, sizeof(name));
	name[sizeof(name) - 1] = '\0';
	anx_trace_append(trace, ANX_TRACE_CREATED, name, ANX_OK);
}

static int runtime_deny(struct anx_cell_trace *trace, enum anx_admission_gate gate,
			int error, const char *reason)
{
	trace->denied_gate = gate;
	anx_trace_append(trace, ANX_TRACE_ADMISSION_DENIED, reason, error);
	return error;
}

static int runtime_finish_trace(struct anx_cell *cell, struct anx_cell_trace *trace)
{
	anx_oid_t final_oid;
	int ret = anx_trace_finalize(trace, &final_oid);
	/* Preserve the prepared record's address if finalization fails. */
	cell->trace_oid = ret == ANX_OK ? final_oid : trace->storage_oid;
	return ret;
}

/* Zero removes a numeric bound, so it cannot replace a bounded parent. */
static bool scope_widens(uint64_t child, uint64_t parent)
{
	return parent && (!child || child > parent);
}

static bool scope_contains(const struct anx_cell *parent,
			   const struct anx_cell *child)
{
	uint32_t i;
	bool linked = false;

	if (parent->child_count > ANX_MAX_CHILD_CELLS)
		return false;
	for (i = 0; i < parent->child_count; i++)
		if (anx_uuid_compare(&parent->child_cids[i], &child->cid) == 0)
			linked = true;
	if (!linked || anx_cell_status_terminal(parent->status) ||
	    !parent->execution.allow_recursive_cells ||
	    parent->recursion_depth >= parent->execution.max_recursion_depth ||
	    parent->recursion_depth >= parent->constraints.max_recursion_depth ||
	    child->recursion_depth != parent->recursion_depth + 1)
		return false;
	if ((child->execution.allow_network && !parent->execution.allow_network) ||
	    (child->execution.allow_remote_models && !parent->execution.allow_remote_models) ||
	    (child->execution.allow_recursive_cells && !parent->execution.allow_recursive_cells) ||
	    (child->execution.allow_side_effects && !parent->execution.allow_side_effects))
		return false;
	if (scope_widens(child->constraints.max_latency_ms, parent->constraints.max_latency_ms) ||
	    scope_widens(child->constraints.max_memory_admission_bytes, parent->constraints.max_memory_admission_bytes) ||
	    scope_widens(child->constraints.max_cost_usd_cents, parent->constraints.max_cost_usd_cents) ||
	    scope_widens(child->cognitive.max_tokens, parent->cognitive.max_tokens) ||
	    scope_widens(child->cognitive.max_reasoning_depth, parent->cognitive.max_reasoning_depth) ||
	    child->constraints.max_child_cells > parent->constraints.max_child_cells ||
	    child->constraints.max_recursion_depth > parent->constraints.max_recursion_depth ||
	    child->execution.max_recursion_depth > parent->execution.max_recursion_depth)
		return false;
	if ((parent->constraints.locality == ANX_LOCAL_ONLY ||
	     parent->constraints.locality == ANX_REMOTE_REQUIRED) &&
	    child->constraints.locality != parent->constraints.locality)
		return false;
	return true;
}

static int runtime_check_scope(struct anx_cell *cell)
{
	struct anx_cell *current = cell;
	int ret = ANX_OK;

	/* Check ancestors too: a revoked root must constrain grandchildren. */
	while (!anx_uuid_is_nil(&current->parent_cid)) {
		struct anx_cell *parent = anx_cell_store_lookup(&current->parent_cid);
		bool allowed;

		if (!parent) {
			ret = ANX_EPERM;
			break;
		}
		anx_spin_lock(&parent->lock);
		allowed = scope_contains(parent, current);
		anx_spin_unlock(&parent->lock);
		if (!allowed) {
			anx_cell_store_release(parent);
			ret = ANX_EPERM;
			break;
		}
		if (current != cell)
			anx_cell_store_release(current);
		current = parent;
	}
	if (ret == ANX_OK && current->recursion_depth != 0)
		ret = ANX_EPERM;
	if (current != cell)
		anx_cell_store_release(current);
	return ret;
}

static int runtime_admit(struct anx_cell *cell, struct anx_cell_trace *trace)
{
	int ret;

	ret = runtime_check_scope(cell);
	if (ret != ANX_OK)
		return runtime_deny(trace, ANX_ADMISSION_SCOPE, ret, "delegated scope denied");

	/* The external handler can change another system before commit. */
	if (cell->cell_type == ANX_CELL_TASK_EXTERNAL_CALL) {
		if (!cell->ext_call)
			return runtime_deny(trace, ANX_ADMISSION_DESCRIPTOR, ANX_EINVAL,
				"external descriptor missing");
		if (!cell->execution.allow_side_effects)
			return runtime_deny(trace, ANX_ADMISSION_AUTHORITY, ANX_EPERM,
				"external authority denied");
		if (!cell->commit.write_trace)
			return runtime_deny(trace, ANX_ADMISSION_AUDIT_REQUIRED, ANX_EPERM,
				"external audit required");
	}

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

	/* Route: select an engine for execution */
	{
		struct anx_route_result route;

		if (anx_route_plan(cell, &route) == ANX_OK &&
		    route.candidate_count > 0 &&
		    route.candidates[route.selected_index].feasible) {
			uint32_t s;

			for (s = 0; s < plan->step_count; s++) {
				if (plan->steps[s].kind == ANX_STEP_DIRECT_EXEC) {
					plan->steps[s].assigned_engine =
						route.candidates[route.selected_index].engine_id;
					break;
				}
			}
		}
	}

	cell->plan_id = plan->plan_id;
	trace->plan_ref = plan->plan_id;

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

static int runtime_submit_model(struct anx_cell *cell,
				struct anx_engine *engine,
				struct anx_model_server *server)
{
	struct anx_infer_request req;
	uint64_t latency_ms = cell->constraints.max_latency_ms;
	int ret;

	anx_memset(&req, 0, sizeof(req));
	req.requestor_cid = cell->cid;
	req.engine_id = engine->eid;
	req.max_tokens = cell->cognitive.max_tokens;
	if (latency_ms) {
		/* Saturate an unrepresentable deadline instead of wrapping. */
		req.deadline_ns = ~(uint64_t)0;
		if (latency_ms <= (~(uint64_t)0 - cell->started_at) / 1000000)
			req.deadline_ns = cell->started_at + latency_ms * 1000000;
	}
	ret = anx_msrv_submit(server, &req);
	return ret == ANX_OK ? req.status : ret;
}

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
			 * External-call task: dispatch via the scheme
			 * registry. This is the designated bridge to
			 * outside systems (Postgres, HTTP, etc.).
			 */
			if (cell->cell_type == ANX_CELL_TASK_EXTERNAL_CALL &&
			    cell->ext_call) {
				int xret;

				/* Count declared bytes and elapsed transport time per attempt. */
				trace->tool.attempted = true;
				trace->tool.request_bytes = cell->ext_call->request_size;
				cell->ext_call->response_size = 0;
				cell->ext_call->status_code = 0;
				trace->tool.started_at = arch_time_now();
				xret = anx_external_invoke(cell->ext_call);
				trace->tool.completed_at = arch_time_now();
				trace->tool.response_bytes = cell->ext_call->response_size;
				trace->tool.transport_result = xret;
				trace->tool.status_code = cell->ext_call->status_code;
				if (xret != ANX_OK) {
					anx_trace_append(trace,
							 ANX_TRACE_STEP_COMPLETED,
							 "external call failed",
							 xret);
					return xret;
				}
				break;
			}

			/* Dispatch to assigned engine if set */
			if (!anx_uuid_is_nil(&step->assigned_engine)) {
				struct anx_engine *eng;

				eng = anx_engine_lookup(&step->assigned_engine);
				if (eng &&
				    (eng->engine_class == ANX_ENGINE_LOCAL_MODEL ||
				     eng->engine_class == ANX_ENGINE_REMOTE_MODEL)) {
					struct anx_model_server *srv;

					srv = anx_msrv_lookup(&step->assigned_engine);
					if (srv) {
						ret = runtime_submit_model(cell, eng, srv);
						if (ret != ANX_OK)
							return ret;
					}
				}
				/* Non-model engines: stub for now */
			}
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

static struct anx_cell *active_cell;

const anx_cid_t *anx_cell_current_id(void)
{
	return active_cell ? &active_cell->cid : NULL;
}

static int runtime_run(struct anx_cell *cell)
{
	struct anx_cell_plan *plan = NULL;
	struct anx_cell_trace *trace = NULL;
	int ret;

	if (!cell)
		return ANX_EINVAL;

	/*
	 * DAG gate: a cell with declared predecessors cannot enter the
	 * pipeline until all of them have COMPLETED. A failed or
	 * cancelled predecessor propagates — this cell is marked FAILED.
	 * No trace is started in the not-ready case so the caller can
	 * retry cleanly.
	 */
	if (cell->dep_count > 0) {
		int deps = anx_cell_deps_satisfied(cell);

		if (deps < 0) {
			cell->error_code = deps;
			anx_strlcpy(cell->error_msg,
				    "dependency failed",
				    sizeof(cell->error_msg));
			anx_cell_transition(cell, ANX_CELL_FAILED);
			return deps;
		}
		if (deps == 0)
			return ANX_EBUSY;	/* retry later */
	}

	ret = anx_trace_create(&cell->cid, &trace);
	if (ret != ANX_OK)
		return ret;

	cell->trace_id = trace->trace_id;
	trace->parent_cell_ref = cell->parent_cid;
	anx_memset(&cell->trace_oid, 0, sizeof(cell->trace_oid));
	anx_sched_cancel(&cell->cid);
	runtime_trace_created(trace, cell);
	if (cell->cell_type == ANX_CELL_TASK_EXTERNAL_CALL) {
		ret = anx_trace_prepare(trace);
		if (ret != ANX_OK) {
			ret = runtime_deny(trace, ANX_ADMISSION_AUDIT_STORAGE, ret,
				"audit reservation failed");
			goto fail;
		}
		cell->trace_oid = trace->storage_oid;
	}

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
	if (cell->status == ANX_CELL_CANCELLED) {
		ret = ANX_ECANCELED;
		goto fail;
	}

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
	if (runtime_needs_trace(cell) && runtime_finish_trace(cell, trace) != ANX_OK) {
		/* Execution has completed; this error must not imply it was rolled back. */
		cell->error_code = ANX_EAUDIT;
		anx_plan_destroy(plan);
		anx_trace_destroy(trace);
		return ANX_EAUDIT;
	}

	anx_plan_destroy(plan);
	anx_trace_destroy(trace);
	return ANX_OK;

fail:
	cell->error_code = ret;
	cell->completed_at = arch_time_now();
	if (cell->status == ANX_CELL_CANCELLED) {
		ret = ANX_ECANCELED;
		cell->error_code = ret;
		anx_trace_append(trace, ANX_TRACE_CANCELLED, "cell cancelled", ret);
	} else {
		anx_cell_transition(cell, ANX_CELL_FAILED);
		anx_trace_append(trace, ANX_TRACE_FAILED, "cell failed", ret);
	}

	if (runtime_needs_trace(cell))
		runtime_finish_trace(cell, trace);

	if (plan)
		anx_plan_destroy(plan);
	anx_trace_destroy(trace);
	return ret;
}

int anx_cell_run(struct anx_cell *cell)
{
	struct anx_cell *previous = active_cell;
	int ret;

	if (!cell)
		return ANX_EINVAL;
	anx_spin_lock(&cell->lock);
	if (cell->status != ANX_CELL_CREATED || cell->runtime_active) {
		anx_spin_unlock(&cell->lock);
		return ANX_EBUSY;
	}
	cell->runtime_active = true;
	cell->refcount++;
	anx_spin_unlock(&cell->lock);
	active_cell = cell;
	ret = runtime_run(cell);
	active_cell = previous;
	cell->runtime_active = false;
	anx_cell_store_release(cell);
	return ret;
}

static int runtime_cancel_tree(struct anx_cell *cell)
{
	anx_cid_t children[ANX_MAX_CHILD_CELLS];
	uint32_t count, i;
	int ret, result = ANX_OK;

	if (!cell)
		return ANX_EINVAL;
	if (!anx_cell_status_terminal(cell->status)) {
		ret = anx_cell_transition(cell, ANX_CELL_CANCELLED);
		if (ret != ANX_OK)
			return ret;
		cell->completed_at = arch_time_now();
		cell->error_code = ANX_ECANCELED;
		if (!cell->runtime_active && runtime_needs_trace(cell)) {
			struct anx_cell_trace *trace;
			if (anx_trace_create(&cell->cid, &trace) == ANX_OK) {
				trace->parent_cell_ref = cell->parent_cid;
				trace->plan_ref = cell->plan_id;
				cell->trace_id = trace->trace_id;
				runtime_trace_created(trace, cell);
				anx_trace_append(trace, ANX_TRACE_CANCELLED,
					"cell cancelled", ANX_ECANCELED);
				anx_trace_finalize(trace, &cell->trace_oid);
				anx_trace_destroy(trace);
			}
		}
	}
	anx_sched_cancel(&cell->cid);
	anx_spin_lock(&cell->lock);
	count = cell->child_count;
	if (count > ANX_MAX_CHILD_CELLS) {
		anx_spin_unlock(&cell->lock);
		return ANX_EINVAL;
	}
	anx_memcpy(children, cell->child_cids, count * sizeof(children[0]));
	anx_spin_unlock(&cell->lock);
	for (i = 0; i < count; i++) {
		struct anx_cell *child = anx_cell_store_lookup(&children[i]);
		if (!child)
			continue;
		if (anx_uuid_compare(&child->parent_cid, &cell->cid) == 0) {
			if (cell->recursion_depth == ~(uint32_t)0 ||
			    child->recursion_depth != cell->recursion_depth + 1)
				ret = ANX_EINVAL;
			else
				ret = runtime_cancel_tree(child);
			if (ret != ANX_OK)
				result = ret;
		}
		anx_cell_store_release(child);
	}

	return result;
}

int anx_cell_cancel(struct anx_cell *cell)
{
	if (!cell)
		return ANX_EINVAL;
	if (anx_cell_status_terminal(cell->status) && cell->status != ANX_CELL_CANCELLED)
		return ANX_EINVAL;
	return runtime_cancel_tree(cell);
}

int anx_cell_derive_child(struct anx_cell *parent,
			  enum anx_cell_type type,
			  const struct anx_cell_intent *intent,
			  struct anx_cell **child_out)
{
	struct anx_cell *child;
	int ret;

	if (!child_out)
		return ANX_EINVAL;
	*child_out = NULL;
	if (!parent)
		return ANX_EINVAL;
	ret = runtime_check_scope(parent);
	if (ret != ANX_OK)
		return ret;

	anx_spin_lock(&parent->lock);
	if (anx_cell_status_terminal(parent->status) ||
	    !parent->execution.allow_recursive_cells ||
	    parent->recursion_depth >= parent->execution.max_recursion_depth ||
	    parent->recursion_depth >= parent->constraints.max_recursion_depth) {
		ret = ANX_EPERM;
		goto out;
	}

	if (parent->child_count >= ANX_MAX_CHILD_CELLS ||
	    parent->child_count >= parent->constraints.max_child_cells) {
		ret = ANX_ENOMEM;
		goto out;
	}

	ret = anx_cell_create(type, intent, &child);
	if (ret != ANX_OK)
		goto out;

	/* Wire up lineage */
	child->parent_cid = parent->cid;
	child->recursion_depth = parent->recursion_depth + 1;

	/* Inherit stricter policies from parent */
	child->constraints = parent->constraints;
	child->execution = parent->execution;
	child->cognitive = parent->cognitive;
	child->routing = parent->routing;
	child->validation = parent->validation;
	child->commit = parent->commit;
	child->retry = parent->retry;
	child->contract = parent->contract;

	/* Record in parent */
	parent->child_cids[parent->child_count] = child->cid;
	parent->child_count++;

	*child_out = child;
out:
	anx_spin_unlock(&parent->lock);
	return ret;
}
