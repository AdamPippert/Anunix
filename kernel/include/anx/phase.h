#ifndef ANX_PHASE_H
#define ANX_PHASE_H

#include <anx/engine_lease.h>

#define ANX_PHASE_OWNER_MAX 128U
enum anx_phase_role { ANX_ROLE_CONTROL = 1, ANX_ROLE_ORCHESTRATOR, ANX_ROLE_RUNNER };
enum anx_phase_kind {
	ANX_PHASE_IDLE, ANX_PHASE_ORCHESTRATION, ANX_PHASE_TOOL,
	ANX_PHASE_INFERENCE, ANX_PHASE_REVIEW, ANX_PHASE_WAIT, ANX_PHASE_COUNT
};
struct anx_phase_limit {
	bool enabled;
	enum anx_mem_tier tier;
	uint64_t memory_bytes;
	enum anx_accel_type accelerator;
	uint32_t accelerator_pct;
};
struct anx_phase_contract {
	enum anx_phase_role role;
	struct anx_phase_limit limits[ANX_PHASE_COUNT];
};
struct anx_phase_request {
	enum anx_phase_kind phase;
	uint64_t memory_bytes;
	uint32_t accelerator_pct;
};
struct anx_phase_view {
	anx_cid_t owner;
	enum anx_phase_role role;
	enum anx_phase_kind phase;
	uint64_t epoch;
	anx_eid_t lease_id;
	enum anx_mem_tier tier;
	uint64_t memory_bytes;
	enum anx_accel_type accelerator;
	uint32_t accelerator_pct;
};

/* Controller attaches immutable role limits and retains the cell until detach. */
int anx_phase_attach(const anx_cid_t *owner, const struct anx_phase_contract *contract);
int anx_phase_detach(const anx_cid_t *owner);
/* Hints from the owner or controller can request only its preauthorized limits. */
int anx_phase_get(const anx_cid_t *owner, struct anx_phase_view *out);
int anx_phase_begin(const anx_cid_t *owner, uint64_t epoch, const struct anx_phase_request *request);
int anx_phase_finish(const anx_cid_t *owner, uint64_t epoch);
/* Controller resizing preserves the Cell, phase, and lease identity. */
int anx_phase_resize(const anx_cid_t *owner, uint64_t epoch, uint64_t memory_bytes,
		uint32_t accelerator_pct, struct anx_phase_view *out);

#endif
