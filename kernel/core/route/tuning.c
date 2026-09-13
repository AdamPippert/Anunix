#include <anx/tuning.h>
#include <anx/string.h>
#include <anx/spinlock.h>

static struct anx_spinlock tuning_lock = ANX_SPINLOCK_INIT;
static struct anx_route_tuning_state active = {
	.weights = {20, 30, 5, 10, -25, 10, 25, -15},
	.generation = 1,
};
static struct anx_route_weight_policy previous;

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
	struct anx_route_tuning_action proposal;
	bool irq_state;
	int ret = ANX_OK;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!action || !trial_out)
		return ANX_EINVAL;
	proposal = *action;
	if (proposal.schema != 1 || !proposal.expected_generation ||
	    anx_route_weight_policy_validate(&proposal.weights) != ANX_OK)
		return ANX_EINVAL;
	ret = anx_route_target_check(&proposal.target);
	if (ret != ANX_OK)
		return ret;
	anx_spin_lock_irqsave(&tuning_lock, &irq_state);
	if (active.trial_active || proposal.expected_generation != active.generation)
		ret = ANX_EBUSY;
	else if (active.generation >= ~(uint64_t)0 - 1)
		ret = ANX_EFULL;
	else {
		previous = active.weights;
		active.weights = proposal.weights;
		active.trial_active = true;
		*trial_out = ++active.generation;
	}
	anx_spin_unlock_irqrestore(&tuning_lock, irq_state);
	return ret;
}

int anx_route_tuning_finish(uint64_t trial, enum anx_route_trial_result result)
{
	bool irq_state;
	int ret = ANX_OK;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!trial || (result != ANX_ROUTE_TRIAL_REJECT && result != ANX_ROUTE_TRIAL_ACCEPT))
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&tuning_lock, &irq_state);
	if (!active.trial_active)
		ret = ANX_ENOENT;
	else if (trial != active.generation)
		ret = ANX_EBUSY;
	else {
		if (result == ANX_ROUTE_TRIAL_REJECT)
			active.weights = previous;
		active.trial_active = false;
		active.generation++;
	}
	anx_spin_unlock_irqrestore(&tuning_lock, irq_state);
	return ret;
}
