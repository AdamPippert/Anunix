#include <anx/tuning.h>
#include <anx/string.h>

int anx_route_tuning_snapshot(struct anx_route_tuning_state *out)
{
	if (!out)
		return ANX_EINVAL;
	anx_memset(out, 0, sizeof(*out));
	anx_route_weight_policy_incumbent(&out->weights);
	out->generation = 1;
	return ANX_OK;
}

int anx_route_tuning_begin(const struct anx_route_tuning_action *action, uint64_t *trial_out)
{
	(void)action; (void)trial_out;
	return ANX_ENOSYS;
}

int anx_route_tuning_finish(uint64_t trial, enum anx_route_trial_result result)
{
	(void)trial; (void)result;
	return ANX_ENOSYS;
}
