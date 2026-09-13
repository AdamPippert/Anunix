#ifndef ANX_TUNING_H
#define ANX_TUNING_H

#include <anx/twin.h>

struct anx_route_tuning_state {
	struct anx_route_weight_policy weights;
	uint64_t generation;
	bool trial_active;
};

/* Fixed action schema; no command strings or caller-defined controls. */
struct anx_route_tuning_action {
	uint32_t schema;
	uint64_t expected_generation;
	struct anx_route_weight_policy weights;
};

enum anx_route_trial_result {
	ANX_ROUTE_TRIAL_REJECT,
	ANX_ROUTE_TRIAL_ACCEPT,
};

int anx_route_tuning_snapshot(struct anx_route_tuning_state *out);
/* Only trusted control code outside an active cell can mutate the policy. */
int anx_route_tuning_begin(const struct anx_route_tuning_action *action, uint64_t *trial_out);
int anx_route_tuning_finish(uint64_t trial, enum anx_route_trial_result result);

#endif
