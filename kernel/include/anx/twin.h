/*
 * anx/twin.h — Resource Twin (RFC-0029).
 *
 * A Resource Twin is a value snapshot of the engine registry and
 * scheduler queue depths at a point in time. It supports cheap what-if
 * routing simulation — replaying feasibility and scoring against the
 * frozen snapshot under a candidate weight policy — without touching
 * live engine state or actually admitting/routing anything.
 *
 * Scope: the Twin simulates the deterministic weighted-scoring
 * component of route planning (RFC-0005 Section 13), mirroring
 * anx_route_score_engine's dimensions. It intentionally does not
 * include the JEPA route-score delta (a live, stateful, non-reproducible
 * predictor term) or the full escalation heuristic — see
 * docs/design/regime-gated-scheduling.md for the scope rationale.
 */

#ifndef ANX_TWIN_H
#define ANX_TWIN_H

#include <anx/types.h>
#include <anx/engine.h>
#include <anx/sched.h>
#include <anx/cell.h>

#define ANX_TWIN_MAX_ENGINES	32

/* --- Readiness (folds the topic plan's Readiness Contract in here) --- */

enum anx_readiness {
	ANX_READY_NONE,		/* does not exist / offline / administratively disabled */
	ANX_READY_USABLE,	/* exists, transitioning (registered/loading/draining/unloading) */
	ANX_READY_READY,	/* provisioned: loaded-not-serving, or serving-but-degraded */
	ANX_READY_HEALTHY,	/* ready and actively serving within envelope */
};

/* Derive readiness from an engine's current status. */
enum anx_readiness anx_readiness_from_status(enum anx_engine_status status);

/* --- Frozen engine snapshot --- */

struct anx_twin_engine_snapshot {
	anx_eid_t eid;
	enum anx_engine_class engine_class;
	enum anx_engine_status status;
	enum anx_readiness readiness;
	bool supports_private_data;
	bool requires_network;
	uint32_t cpu_weight;
	uint32_t gpu_weight;
	uint32_t quality_score;
	bool is_local;
	bool has_topology_affinity;
	uint64_t topology_bk_lo;
	uint64_t topology_bk_hi;
};

/* --- Resource Twin --- */

struct anx_resource_twin {
	struct anx_twin_engine_snapshot engines[ANX_TWIN_MAX_ENGINES];
	uint32_t engine_count;
	uint32_t queue_depth[ANX_QUEUE_CLASS_COUNT];
	anx_time_t taken_at;
};

/* --- Candidate routing weight policy ---
 *
 * Mirrors the scoring dimensions anx_route_score_engine already applies,
 * as tunable weights instead of hardcoded constants. A candidate policy
 * is what an AI proposal would vary; anx_route_weight_policy_incumbent()
 * reproduces the live function's current constants, so simulating with
 * the incumbent policy against a snapshot taken immediately before a
 * live anx_route_plan() call should reproduce the same winner.
 */
struct anx_route_weight_policy {
	int32_t locality_bonus;
	int32_t local_first_bonus;
	int32_t gpu_cost_divisor;		/* must be nonzero */
	int32_t cpu_cost_divisor;		/* must be nonzero */
	int32_t degraded_penalty;
	int32_t private_data_bonus;
	int32_t topology_overlap_bonus;
	int32_t topology_mismatch_penalty;
};

void anx_route_weight_policy_incumbent(struct anx_route_weight_policy *out);

struct anx_twin_simulate_result {
	uint32_t candidate_count;	/* feasible engines considered */
	uint32_t winner_index;		/* index into twin->engines[] */
	int32_t winner_score;
	int32_t margin;			/* winner_score - runner_up_score */
	bool has_margin;		/* false when fewer than 2 feasible candidates */
};

/* --- Resource Twin API --- */

void anx_twin_init(void);

/* Take a value-copy snapshot of the current engine registry + queue depths. */
int anx_twin_snapshot(struct anx_resource_twin **out);

void anx_twin_destroy(struct anx_resource_twin *twin);

/*
 * Replay feasibility + weighted scoring for `cell` against the frozen
 * snapshot under `policy`, without touching live state. Returns ANX_OK
 * with candidate_count == 0 (and no winner set) if no snapshot engine
 * is feasible for this cell — that is a valid, non-error outcome.
 */
int anx_twin_simulate(struct anx_resource_twin *twin,
		      struct anx_cell *cell,
		      const struct anx_route_weight_policy *policy,
		      struct anx_twin_simulate_result *result_out);

#endif /* ANX_TWIN_H */
