#include <anx/tuning.h>
#include <anx/string.h>
#include <anx/spinlock.h>
#include <anx/revision.h>
#include <anx/uuid.h>
#include <anx/optimization_harness.h>

static struct anx_spinlock tuning_lock = ANX_SPINLOCK_INIT;
static struct anx_route_tuning_state active = {
	.weights = {20, 30, 5, 10, -25, 10, 25, -15},
	.generation = 1,
};
static struct anx_route_weight_policy previous;
static anx_cid_t trial_owner;

int anx_route_tuning_snapshot(struct anx_route_tuning_state *out)
{
	bool irq_state;
	if (!out)
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&tuning_lock, &irq_state);
	anx_memset(out, 0, sizeof(*out));
	out->weights = active.weights;
	out->generation = active.generation;
	out->trial_active = active.trial_active;
	anx_spin_unlock_irqrestore(&tuning_lock, irq_state);
	return ANX_OK;
}

int anx_route_tuning_begin(const struct anx_route_tuning_action *action, uint64_t *trial_out)
{
	const anx_cid_t *caller = anx_cell_current_id();
	struct anx_route_tuning_action proposal;
	bool irq_state;
	int ret = anx_revision_check(ANX_REVISION_PARAMETERS, true);
	if (ret != ANX_OK)
		return ret;
	if (!action || !trial_out)
		return ANX_EINVAL;
	proposal = *action;
	if (proposal.schema != 1 || !proposal.expected_generation ||
	    anx_route_policy_validate(&proposal.task, &proposal.weights) != ANX_OK)
		return ANX_EINVAL;
	ret = anx_route_target_check(&proposal.target);
	if (ret != ANX_OK)
		return ret;
	if (proposal.task.schema) {
		ret = anx_route_evaluated_action_check(&proposal);
		if (ret != ANX_OK) return ret;
	}
	anx_spin_lock_irqsave(&tuning_lock, &irq_state);
	if (active.trial_active || proposal.expected_generation != active.generation)
		ret = ANX_EBUSY;
	else if (active.generation >= ~(uint64_t)0 - 1)
		ret = ANX_EFULL;
	else {
		previous = active.weights;
		active.weights = proposal.weights;
		active.trial_active = true;
		trial_owner = caller ? *caller : ANX_UUID_NIL;
		*trial_out = ++active.generation;
	}
	anx_spin_unlock_irqrestore(&tuning_lock, irq_state);
	return ret;
}

int anx_route_tuning_finish(uint64_t trial, enum anx_route_trial_result result)
{
	const anx_cid_t *caller = anx_cell_current_id();
	bool irq_state;
	int ret = anx_revision_check(ANX_REVISION_PARAMETERS, true);
	if (ret != ANX_OK)
		return ret;
	if (!trial || (result != ANX_ROUTE_TRIAL_REJECT && result != ANX_ROUTE_TRIAL_ACCEPT))
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&tuning_lock, &irq_state);
	if (!active.trial_active)
		ret = ANX_ENOENT;
	else if (caller && anx_uuid_compare(caller, &trial_owner))
		ret = ANX_EPERM;
	else if (trial != active.generation)
		ret = ANX_EBUSY;
	else {
		if (result == ANX_ROUTE_TRIAL_REJECT)
			active.weights = previous;
		active.trial_active = false;
		trial_owner = ANX_UUID_NIL;
		active.generation++;
	}
	anx_spin_unlock_irqrestore(&tuning_lock, irq_state);
	return ret;
}
