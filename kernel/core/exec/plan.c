/*
 * plan.c — Execution Cell Plan management.
 *
 * Plans are generated during the planning phase and describe
 * the execution steps a cell will follow.
 */

#include <anx/types.h>
#include <anx/cell_plan.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/arch.h>

int anx_plan_create(const anx_cid_t *cell_ref, struct anx_cell_plan **out)
{
	struct anx_cell_plan *plan;

	if (!cell_ref || !out)
		return ANX_EINVAL;

	plan = anx_zalloc(sizeof(*plan));
	if (!plan)
		return ANX_ENOMEM;

	anx_uuid_generate(&plan->plan_id);
	plan->cell_ref = *cell_ref;
	plan->created_at = arch_time_now();

	*out = plan;
	return ANX_OK;
}

int anx_plan_add_step(struct anx_cell_plan *plan,
		      enum anx_plan_step_kind kind,
		      const char *description)
{
	struct anx_plan_step *step;

	if (!plan)
		return ANX_EINVAL;
	if (plan->finalized)
		return ANX_EPERM;
	if (plan->step_count >= ANX_MAX_PLAN_STEPS)
		return ANX_ENOMEM;

	step = &plan->steps[plan->step_count];
	step->step_index = plan->step_count;
	step->kind = kind;
	/* engine left nil — assigned by routing */
	if (description)
		anx_strlcpy(step->description, description,
			     sizeof(step->description));

	plan->step_count++;
	return ANX_OK;
}

int anx_plan_finalize(struct anx_cell_plan *plan)
{
	if (!plan)
		return ANX_EINVAL;
	if (plan->finalized)
		return ANX_EPERM;
	if (plan->step_count == 0)
		return ANX_EINVAL;

	plan->finalized = true;
	plan->current_step = 0;
	return ANX_OK;
}

int anx_plan_advance(struct anx_cell_plan *plan)
{
	if (!plan || !plan->finalized)
		return ANX_EINVAL;

	plan->current_step++;
	if (plan->current_step >= plan->step_count)
		return ANX_ENOENT;

	return ANX_OK;
}

void anx_plan_destroy(struct anx_cell_plan *plan)
{
	if (plan)
		anx_free(plan);
}
