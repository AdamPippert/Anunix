/* Baseline adapter copies typed intent without validating its declared domain. */
#include <anx/tuning.h>
#include <anx/string.h>
void anx_route_policy_defaults(struct anx_route_policy_task *out)
{
	anx_memset(out, 0, sizeof(*out));
	out->schema = 1; out->variable_count = ANX_ROUTE_POLICY_VARIABLES; out->constraint_count = 1;
	for (uint32_t i = 0; i < ANX_ROUTE_POLICY_VARIABLES; i++) {
		out->variables[i] = (struct anx_policy_range){0, 1000, ANX_POLICY_SCORE, ANX_POLICY_DEFAULT};
		if (i == 2 || i == 3) out->variables[i] = (struct anx_policy_range){1, 1000, ANX_POLICY_DIVISOR, ANX_POLICY_DEFAULT};
		if (i == 4 || i == 7) out->variables[i] = (struct anx_policy_range){-1000, 0, ANX_POLICY_SCORE, ANX_POLICY_DEFAULT};
	}
	out->constraints[0] = (struct anx_policy_constraint){ANX_POLICY_FEASIBLE, {1, ANX_TWIN_MAX_ENGINES, ANX_POLICY_COUNT, ANX_POLICY_DEFAULT}};
}
int anx_route_policy_validate(const struct anx_route_policy_task *task, const struct anx_route_weight_policy *weights)
{ (void)task; (void)weights; return ANX_OK; }
int anx_route_policy_compile(const struct anx_route_policy_task *task, const struct anx_route_tuning_action *base,
			     struct anx_route_tuning_action *out)
{ *out = *base; out->task = *task; return ANX_OK; }
int anx_route_policy_result_check(const struct anx_route_policy_task *task, const struct anx_resource_twin *twin,
				  const struct anx_twin_simulate_result *result)
{ (void)task; (void)twin; (void)result; return ANX_OK; }
void anx_route_policy_digest(const struct anx_route_policy_task *task, uint8_t out[32])
{ (void)task; anx_memset(out, 0, 32); }
