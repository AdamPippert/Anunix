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
