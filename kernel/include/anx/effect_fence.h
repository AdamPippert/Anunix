#ifndef ANX_EFFECT_FENCE_H
#define ANX_EFFECT_FENCE_H

#include <anx/types.h>
#include <anx/state_object.h>

#define ANX_EFFECT_FENCE_MAX 256U

enum anx_effect_fence_state {
	ANX_FENCE_RUNNING,
	ANX_FENCE_HELD,
	ANX_FENCE_REJECTED,
	ANX_FENCE_CANCELLED,
	ANX_FENCE_TIMED_OUT,
};

struct anx_effect_fence_view {
	anx_oid_t id;
	uint64_t epoch;
	uint64_t generation;
	enum anx_effect_fence_state state;
	enum anx_sensitivity read_sensitivity;
	anx_oid_t read_origin;
	uint64_t read_count;
};

struct anx_cell;
struct anx_pending_effect;
struct anx_sink;

/* Trusted control code creates, binds, and changes run fences. */
int anx_effect_fence_create(struct anx_effect_fence_view *out);
int anx_effect_fence_get(const anx_oid_t *id, struct anx_effect_fence_view *out);
int anx_effect_fence_bind(struct anx_cell *cell, const anx_oid_t *id);
int anx_effect_fence_transition(const anx_oid_t *id, uint64_t expected_generation,
				enum anx_effect_fence_state state);
/* A branch can request a hold or cancellation for its inherited run fence. */
int anx_effect_fence_hold(struct anx_cell *cell);
int anx_effect_fence_cancel(struct anx_cell *cell);
/* Nil bindings retain legacy behavior. HELD returns EBUSY; closed runs return EPERM. */
int anx_effect_fence_check(const struct anx_cell *cell, anx_oid_t *id_out, uint64_t *epoch_out);
/* Internal payload-read hook: monotonically accumulate the active run's label. */
int anx_effect_fence_observe_read(const anx_oid_t *oid, enum anx_sensitivity sensitivity);
/* A bound run's accumulated label constrains every later protected effect. */
int anx_effect_fence_check_sink(const struct anx_cell *cell, const struct anx_sink *sink);
/* Internal phase gate. Call anx_effect_mark_dispatching for complete authorization. */
int anx_effect_fence_dispatch(struct anx_pending_effect *effect);

#endif
