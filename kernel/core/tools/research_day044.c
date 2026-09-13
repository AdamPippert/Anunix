/* Baseline weight activation has no per-request validated profile envelope. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/tuning.h>

int anx_research_day044(void)
{
	struct anx_route_tuning_state original, current;
	struct anx_route_tuning_action action = { .schema = 1 };
	uint64_t trial = 0;
	int ret = anx_route_tuning_snapshot(&original);
	if (ret != ANX_OK || original.trial_active) return ANX_EBUSY;
	action.expected_generation = original.generation;
	action.weights = original.weights;
	action.weights.locality_bonus++;
	ret = anx_route_tuning_begin(&action, &trial);
	if (ret != ANX_OK) return ret;
	anx_route_tuning_snapshot(&current);
	/* No request-specific validation exists to retain the incumbent here. */
	ret = current.weights.locality_bonus == original.weights.locality_bonus ? ANX_OK : -4401;
	anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	return ret;
}
#endif
