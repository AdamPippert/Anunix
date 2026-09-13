#ifndef ANX_ROUTE_POLICY_TASK_H
#define ANX_ROUTE_POLICY_TASK_H
#include <anx/types.h>

#define ANX_ROUTE_POLICY_VARIABLES 8U
#define ANX_ROUTE_POLICY_CONSTRAINTS 4U
enum anx_policy_unit { ANX_POLICY_COUNT = 1, ANX_POLICY_SCORE, ANX_POLICY_PERCENT, ANX_POLICY_DIVISOR };
enum anx_policy_origin { ANX_POLICY_OPERATOR = 1, ANX_POLICY_DEFAULT };
enum anx_policy_metric { ANX_POLICY_FEASIBLE = 1, ANX_POLICY_MARGIN, ANX_POLICY_CPU, ANX_POLICY_GPU };
struct anx_policy_range { int32_t minimum, maximum; uint32_t unit, origin; };
struct anx_policy_constraint { uint32_t metric; struct anx_policy_range range; };
struct anx_route_policy_task {
	uint32_t schema, variable_count, constraint_count;
	struct anx_policy_range variables[ANX_ROUTE_POLICY_VARIABLES];
	struct anx_policy_constraint constraints[ANX_ROUTE_POLICY_CONSTRAINTS];
};
struct anx_route_tuning_action;
struct anx_route_weight_policy;
struct anx_resource_twin;
struct anx_twin_simulate_result;
void anx_route_policy_defaults(struct anx_route_policy_task *out);
int anx_route_policy_validate(const struct anx_route_policy_task *task, const struct anx_route_weight_policy *weights);
int anx_route_policy_compile(const struct anx_route_policy_task *task, const struct anx_route_tuning_action *base,
			     struct anx_route_tuning_action *out);
int anx_route_policy_result_check(const struct anx_route_policy_task *task, const struct anx_resource_twin *twin,
				  const struct anx_twin_simulate_result *result);
void anx_route_policy_digest(const struct anx_route_policy_task *task, uint8_t out[32]);
#endif
