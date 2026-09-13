#ifndef ANX_TUNING_H
#define ANX_TUNING_H

#include <anx/twin.h>
#include <anx/kernel_profile.h>

#define ANX_ROUTE_OPTIMIZATION_SCHEMA "anx:optimization/route/v1"
#define ANX_ROUTE_OPTIMIZATION_INPUT_MAX (1U << 20)

struct anx_route_artifact_ref {
	anx_oid_t oid;
	uint64_t version;
	uint8_t digest[32];
	uint32_t sensitivity;
};

struct anx_route_target_contract {
	uint32_t schema; /* zero preserves the manual control path */
	struct anx_kernel_profile build;
	anx_eid_t engine_id;
	uint8_t engine_digest[32];
	struct anx_route_artifact_ref knowledge;
};

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
	struct anx_route_target_contract target;
};

enum anx_route_trial_result {
	ANX_ROUTE_TRIAL_REJECT,
	ANX_ROUTE_TRIAL_ACCEPT,
};

int anx_route_tuning_snapshot(struct anx_route_tuning_state *out);
/* Only trusted control code outside an active cell can mutate the policy. */
int anx_route_tuning_begin(const struct anx_route_tuning_action *action, uint64_t *trial_out);
int anx_route_tuning_finish(uint64_t trial, enum anx_route_trial_result result);

struct anx_route_optimization_artifact {
	uint32_t schema;
	struct anx_route_tuning_action action;
	struct anx_route_artifact_ref evaluation;
};

/* Trusted control captures a bounded target and creates a sealed native artifact. */
int anx_route_target_capture(const anx_eid_t *engine, const anx_oid_t *knowledge,
			     struct anx_route_target_contract *out);
int anx_route_target_check(const struct anx_route_target_contract *target);
int anx_route_tuning_artifact_create(const struct anx_route_tuning_action *action,
				     const anx_oid_t *evaluation, anx_oid_t *out);
int anx_route_tuning_begin_artifact(const anx_oid_t *artifact, uint64_t *trial_out);

#endif
