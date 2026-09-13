/* A finite typed policy problem precedes evaluation and live activation. */
#include <anx/tuning.h>
#include <anx/string.h>
#include <anx/crypto.h>
void anx_route_policy_defaults(struct anx_route_policy_task *out)
{
	if (!out) return;
	anx_memset(out, 0, sizeof(*out));
	out->schema = 1; out->variable_count = ANX_ROUTE_POLICY_VARIABLES; out->constraint_count = 1;
	for (uint32_t i = 0; i < ANX_ROUTE_POLICY_VARIABLES; i++) {
		out->variables[i] = (struct anx_policy_range){0, 1000, ANX_POLICY_SCORE, ANX_POLICY_DEFAULT};
		if (i == 2 || i == 3) out->variables[i] = (struct anx_policy_range){1, 1000, ANX_POLICY_DIVISOR, ANX_POLICY_DEFAULT};
		if (i == 4 || i == 7) out->variables[i] = (struct anx_policy_range){-1000, 0, ANX_POLICY_SCORE, ANX_POLICY_DEFAULT};
	}
	out->constraints[0] = (struct anx_policy_constraint){ANX_POLICY_FEASIBLE, {1, ANX_TWIN_MAX_ENGINES, ANX_POLICY_COUNT, ANX_POLICY_DEFAULT}};
}
static bool valid_range(const struct anx_policy_range *r, uint32_t unit, int32_t low, int32_t high)
{
	return r->unit == unit && (r->origin == ANX_POLICY_OPERATOR || r->origin == ANX_POLICY_DEFAULT) &&
		r->minimum >= low && r->maximum <= high && r->minimum <= r->maximum;
}

static int validate_task(const struct anx_route_policy_task *task)
{
	struct anx_route_policy_task defaults;
	if (!task) return ANX_EINVAL;
	if (!task->schema) {
		struct anx_route_policy_task empty = {0};
		return anx_memcmp(task, &empty, sizeof(empty)) ? ANX_EINVAL : ANX_OK;
	}
	if (task->schema != 1 || task->variable_count != ANX_ROUTE_POLICY_VARIABLES ||
	    !task->constraint_count || task->constraint_count > ANX_ROUTE_POLICY_CONSTRAINTS) return ANX_EINVAL;
	anx_route_policy_defaults(&defaults);
	for (uint32_t i = 0; i < ANX_ROUTE_POLICY_VARIABLES; i++)
		if (!valid_range(&task->variables[i], defaults.variables[i].unit,
				 defaults.variables[i].minimum, defaults.variables[i].maximum)) return ANX_EINVAL;
	for (uint32_t i = 0; i < ANX_ROUTE_POLICY_CONSTRAINTS; i++) {
		const struct anx_policy_constraint *c = &task->constraints[i];
		if (i >= task->constraint_count) {
			struct anx_policy_constraint empty = {0};
			if (anx_memcmp(c, &empty, sizeof(empty))) return ANX_EINVAL;
			continue;
		}
		for (uint32_t j = 0; j < i; j++) if (task->constraints[j].metric == c->metric) return ANX_EINVAL;
		switch (c->metric) {
		case ANX_POLICY_FEASIBLE:
			if (!valid_range(&c->range, ANX_POLICY_COUNT, 0, ANX_TWIN_MAX_ENGINES)) return ANX_EINVAL;
			break;
		case ANX_POLICY_MARGIN:
			if (!valid_range(&c->range, ANX_POLICY_SCORE, 0, 6300)) return ANX_EINVAL;
			break;
		case ANX_POLICY_CPU:
		case ANX_POLICY_GPU:
			if (!valid_range(&c->range, ANX_POLICY_COST_WEIGHT, 0, 100)) return ANX_EINVAL;
			break;
		default: return ANX_EINVAL;
		}
	}
	return ANX_OK;
}

int anx_route_policy_validate(const struct anx_route_policy_task *task, const struct anx_route_weight_policy *weights)
{
	int ret = validate_task(task);
	if (ret != ANX_OK) return ret;
	ret = anx_route_weight_policy_validate(weights);
	if (ret != ANX_OK || !task->schema) return ret;
	int32_t values[] = {weights->locality_bonus, weights->local_first_bonus, weights->gpu_cost_divisor,
		weights->cpu_cost_divisor, weights->degraded_penalty, weights->private_data_bonus,
		weights->topology_overlap_bonus, weights->topology_mismatch_penalty};
	for (uint32_t i = 0; i < ANX_ROUTE_POLICY_VARIABLES; i++)
		if (values[i] < task->variables[i].minimum || values[i] > task->variables[i].maximum) return ANX_EINVAL;
	return ANX_OK;
}
int anx_route_policy_compile(const struct anx_route_policy_task *task, const struct anx_route_tuning_action *base,
			     struct anx_route_tuning_action *out)
{
	if (!task || !base || !out || task->schema != 1 || base->schema != 1 ||
	    !base->expected_generation || base->target.schema != 1) return ANX_EINVAL;
	struct anx_route_tuning_action candidate = *base;
	candidate.task = *task;
	int ret = anx_route_policy_validate(&candidate.task, &candidate.weights);
	if (ret == ANX_OK) ret = anx_route_target_check(&candidate.target);
	if (ret == ANX_OK) *out = candidate;
	return ret;
}
int anx_route_policy_result_check(const struct anx_route_policy_task *task, const struct anx_resource_twin *twin,
				  const struct anx_twin_simulate_result *result)
{
	int ret = validate_task(task);
	if (ret != ANX_OK || !task->schema) return ret;
	if (!twin || !result || twin->engine_count > ANX_TWIN_MAX_ENGINES ||
	    result->candidate_count > twin->engine_count) return ANX_EINVAL;
	for (uint32_t i = 0; i < task->constraint_count; i++) {
		const struct anx_policy_constraint *c = &task->constraints[i];
		int32_t value;
		if (c->metric == ANX_POLICY_FEASIBLE) value = (int32_t)result->candidate_count;
		else {
			if (!result->candidate_count || result->winner_index >= twin->engine_count) return ANX_EPERM;
			if (c->metric == ANX_POLICY_MARGIN) {
				if (!result->has_margin) return ANX_EPERM;
				value = result->margin;
			} else if (c->metric == ANX_POLICY_CPU) value = (int32_t)twin->engines[result->winner_index].cpu_weight;
			else value = (int32_t)twin->engines[result->winner_index].gpu_weight;
		}
		if (value < c->range.minimum || value > c->range.maximum) return ANX_EPERM;
	}
	return ANX_OK;
}

static void hash_word(struct anx_sha256_ctx *hash, uint32_t value)
{
	uint8_t bytes[4];
	for (uint32_t i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (8 * i));
	anx_sha256_update(hash, bytes, sizeof(bytes));
}

static void hash_range(struct anx_sha256_ctx *hash, const struct anx_policy_range *r)
{
	hash_word(hash, (uint32_t)r->minimum); hash_word(hash, (uint32_t)r->maximum);
	hash_word(hash, r->unit); hash_word(hash, r->origin);
}

void anx_route_policy_digest(const struct anx_route_policy_task *task, uint8_t out[32])
{
	struct anx_sha256_ctx hash;
	anx_sha256_init(&hash);
	hash_word(&hash, task->schema); hash_word(&hash, task->variable_count); hash_word(&hash, task->constraint_count);
	for (uint32_t i = 0; i < ANX_ROUTE_POLICY_VARIABLES; i++) hash_range(&hash, &task->variables[i]);
	for (uint32_t i = 0; i < ANX_ROUTE_POLICY_CONSTRAINTS; i++) {
		hash_word(&hash, task->constraints[i].metric); hash_range(&hash, &task->constraints[i].range);
	}
	anx_sha256_final(&hash, out);
}
