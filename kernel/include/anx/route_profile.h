#ifndef ANX_ROUTE_PROFILE_H
#define ANX_ROUTE_PROFILE_H
#include <anx/tuning.h>

#define ANX_ROUTE_PROFILE_CASES_MAX 8U
#define ANX_ROUTE_PROFILE_MAX 32U
#define ANX_ROUTE_PROFILE_SCHEMA "anx:route/profile/v1"
struct anx_route_profile_case {
	enum anx_routing_strategy strategy;
	enum anx_locality locality;
	bool allow_network, allow_remote_models, topology_set;
	uint64_t topology_lo, topology_hi;
};
struct anx_route_profile_case_result {
	anx_eid_t winner;
	int32_t incumbent_score, candidate_score, incumbent_margin, candidate_margin;
	bool has_margin;
};
struct anx_route_profile {
	uint32_t schema, case_count;
	struct anx_kernel_profile build;
	struct anx_route_tuning_state incumbent;
	struct anx_route_weight_policy weights;
	struct anx_resource_twin environment;
	uint8_t environment_digest[32];
	struct anx_route_profile_case cases[ANX_ROUTE_PROFILE_CASES_MAX];
	struct anx_route_profile_case_result results[ANX_ROUTE_PROFILE_CASES_MAX];
};

/* Payload bytes and compiler-owned scratch, excluding allocator/object overhead. */
struct anx_route_profile_budget {
	uint64_t artifact_payload_bytes;
	uint64_t compiler_scratch_bytes;
	uint32_t simulation_calls;
};
int anx_route_profile_compile_bounded(const struct anx_route_weight_policy *weights,
		const struct anx_route_profile_case *cases, uint32_t count,
		const struct anx_route_profile_budget *budget, anx_oid_t *out);

/* The trusted compiler records finite cases and requires incumbent-winner preservation. */
int anx_route_profile_compile(const struct anx_route_weight_policy *weights,
		const struct anx_route_profile_case *cases, uint32_t count, anx_oid_t *out);
int anx_route_profile_release(const anx_oid_t *profile);
/* Read-only selection; failure leaves the supplied incumbent weights untouched. */
int anx_route_profile_choose(const anx_oid_t *profile, const struct anx_cell *cell,
		const struct anx_route_tuning_state *incumbent, struct anx_route_weight_policy *out);
#endif
