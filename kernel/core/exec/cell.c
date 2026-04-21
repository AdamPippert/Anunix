/*
 * cell.c — Execution Cell lifecycle state machine.
 *
 * Enforces the valid status transitions defined in RFC-0003 Section 16.
 */

#include <anx/types.h>
#include <anx/cell.h>

/*
 * Transition table: valid[from][to].
 *
 * created    -> admitted
 * admitted   -> planning, cancelled
 * planning   -> planned, failed, cancelled
 * planned    -> queued, cancelled
 * queued     -> running, cancelled
 * running    -> waiting, validating, failed, cancelled
 * waiting    -> running, failed, cancelled
 * validating -> committing, failed, cancelled
 * committing -> completed, failed, compensating
 * completed  -> (terminal)
 * failed     -> (terminal)
 * cancelled  -> (terminal)
 * compensating -> compensated, failed
 * compensated  -> (terminal)
 */
static const bool valid_transitions[ANX_CELL_STATUS_COUNT][ANX_CELL_STATUS_COUNT] = {
	/* created -> */
	[ANX_CELL_CREATED] = {
		[ANX_CELL_ADMITTED] = true,
		/* DAG gate: a failed predecessor fails this cell before
		 * admission without a trace being started. */
		[ANX_CELL_FAILED] = true,
		[ANX_CELL_CANCELLED] = true,
	},
	/* admitted -> */
	[ANX_CELL_ADMITTED] = {
		[ANX_CELL_PLANNING] = true,
		[ANX_CELL_CANCELLED] = true,
	},
	/* planning -> */
	[ANX_CELL_PLANNING] = {
		[ANX_CELL_PLANNED] = true,
		[ANX_CELL_FAILED] = true,
		[ANX_CELL_CANCELLED] = true,
	},
	/* planned -> */
	[ANX_CELL_PLANNED] = {
		[ANX_CELL_QUEUED] = true,
		[ANX_CELL_CANCELLED] = true,
	},
	/* queued -> */
	[ANX_CELL_QUEUED] = {
		[ANX_CELL_RUNNING] = true,
		[ANX_CELL_CANCELLED] = true,
	},
	/* running -> */
	[ANX_CELL_RUNNING] = {
		[ANX_CELL_WAITING] = true,
		[ANX_CELL_VALIDATING] = true,
		[ANX_CELL_FAILED] = true,
		[ANX_CELL_CANCELLED] = true,
	},
	/* waiting -> */
	[ANX_CELL_WAITING] = {
		[ANX_CELL_RUNNING] = true,
		[ANX_CELL_FAILED] = true,
		[ANX_CELL_CANCELLED] = true,
	},
	/* validating -> */
	[ANX_CELL_VALIDATING] = {
		[ANX_CELL_COMMITTING] = true,
		[ANX_CELL_FAILED] = true,
		[ANX_CELL_CANCELLED] = true,
	},
	/* committing -> */
	[ANX_CELL_COMMITTING] = {
		[ANX_CELL_COMPLETED] = true,
		[ANX_CELL_FAILED] = true,
		[ANX_CELL_COMPENSATING] = true,
	},
	/* compensating -> */
	[ANX_CELL_COMPENSATING] = {
		[ANX_CELL_COMPENSATED] = true,
		[ANX_CELL_FAILED] = true,
	},
	/* completed, failed, cancelled, compensated are terminal */
};

int anx_cell_transition(struct anx_cell *cell, enum anx_cell_status new_status)
{
	enum anx_cell_status old = cell->status;

	if ((int)old < 0 || old >= ANX_CELL_STATUS_COUNT)
		return ANX_EINVAL;
	if ((int)new_status < 0 || new_status >= ANX_CELL_STATUS_COUNT)
		return ANX_EINVAL;
	if (!valid_transitions[old][new_status])
		return ANX_EINVAL;

	cell->status = new_status;
	return ANX_OK;
}

/* --- DAG composition --- */

static bool cid_eq(const anx_cid_t *a, const anx_cid_t *b)
{
	return a->hi == b->hi && a->lo == b->lo;
}

/*
 * Cycle check: would making `prereq` a predecessor of `cell` create a
 * directed cycle? Walks the predecessor closure of `prereq` looking
 * for `cell->cid`. Bounded by ANX_MAX_CELL_DEPS per hop and the live
 * cell graph; uses a small fixed-size visited set.
 */
static bool dep_creates_cycle(const struct anx_cell *cell,
			      const struct anx_cell *prereq)
{
	anx_cid_t visit_stack[ANX_MAX_CELL_DEPS * 4];
	uint32_t stack_len;
	uint32_t i;

	/* Walking the predecessor chain of prereq — if we see cell->cid,
	 * adding this edge would close a cycle. */
	if (cid_eq(&prereq->cid, &cell->cid))
		return true;

	stack_len = 0;
	for (i = 0; i < prereq->dep_count &&
		    stack_len < sizeof(visit_stack) / sizeof(visit_stack[0]);
	     i++)
		visit_stack[stack_len++] = prereq->dep_cids[i];

	while (stack_len > 0) {
		anx_cid_t cur = visit_stack[--stack_len];
		struct anx_cell *node;

		if (cid_eq(&cur, &cell->cid))
			return true;

		node = anx_cell_store_lookup(&cur);
		if (!node)
			continue;
		for (i = 0; i < node->dep_count &&
			    stack_len < sizeof(visit_stack) /
					sizeof(visit_stack[0]);
		     i++)
			visit_stack[stack_len++] = node->dep_cids[i];
		anx_cell_store_release(node);
	}
	return false;
}

int anx_cell_add_dependency(struct anx_cell *cell,
			    const anx_cid_t *prereq_cid)
{
	struct anx_cell *prereq;
	uint32_t i;

	if (!cell || !prereq_cid)
		return ANX_EINVAL;
	if (cid_eq(&cell->cid, prereq_cid))
		return ANX_EINVAL;
	if (cell->dep_count >= ANX_MAX_CELL_DEPS)
		return ANX_ENOMEM;

	for (i = 0; i < cell->dep_count; i++) {
		if (cid_eq(&cell->dep_cids[i], prereq_cid))
			return ANX_EINVAL;		/* duplicate edge */
	}

	prereq = anx_cell_store_lookup(prereq_cid);
	if (!prereq)
		return ANX_ENOENT;

	if (dep_creates_cycle(cell, prereq)) {
		anx_cell_store_release(prereq);
		return ANX_EINVAL;
	}

	cell->dep_cids[cell->dep_count++] = *prereq_cid;
	anx_cell_store_release(prereq);
	return ANX_OK;
}

int anx_cell_set_topology(struct anx_cell *cell,
			  uint64_t bk_lo, uint64_t bk_hi)
{
	if (!cell)
		return ANX_EINVAL;
	if (bk_hi < bk_lo)
		return ANX_EINVAL;

	cell->constraints.topology_bk_lo = bk_lo;
	cell->constraints.topology_bk_hi = bk_hi;
	cell->constraints.topology_bk_set = true;
	return ANX_OK;
}

void anx_cell_clear_topology(struct anx_cell *cell)
{
	if (!cell)
		return;
	cell->constraints.topology_bk_set = false;
	cell->constraints.topology_bk_lo = 0;
	cell->constraints.topology_bk_hi = 0;
}

int anx_cell_deps_satisfied(const struct anx_cell *cell)
{
	uint32_t i;
	int result = 1;		/* optimistic: all satisfied */

	if (!cell)
		return ANX_EINVAL;

	for (i = 0; i < cell->dep_count; i++) {
		struct anx_cell *prereq;
		enum anx_cell_status st;

		prereq = anx_cell_store_lookup(&cell->dep_cids[i]);
		if (!prereq) {
			/* Missing predecessor is treated as a failed dep
			 * so the cell cannot make progress silently. */
			return ANX_ENOENT;
		}
		st = prereq->status;
		anx_cell_store_release(prereq);

		if (st == ANX_CELL_FAILED ||
		    st == ANX_CELL_CANCELLED ||
		    st == ANX_CELL_COMPENSATED)
			return ANX_EPERM;	/* propagate failure */
		if (st != ANX_CELL_COMPLETED)
			result = 0;		/* still pending */
	}
	return result;
}
