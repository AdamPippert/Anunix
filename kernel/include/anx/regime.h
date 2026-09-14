/* Deterministic telemetry tracking and controller-bound policy regions. */
#ifndef ANX_REGIME_H
#define ANX_REGIME_H
#include <anx/types.h>

enum anx_regime_state { ANX_REGIME_STABLE, ANX_REGIME_ESCALATED };
#define ANX_REGIME_ESCALATE_THRESHOLD 200
#define ANX_REGIME_STABLE_THRESHOLD 50
#define ANX_REGIME_SLOW_WINDOW 32
#define ANX_REGIME_FAST_WINDOW 4
#define ANX_REGIME_MIN_SAMPLES ANX_REGIME_SLOW_WINDOW
#define ANX_REGIME_RETURN_SAMPLES 3

/* Bounds use the caller's consistent telemetry unit. Calibration is a controller claim. */
struct anx_regime_region {
	uint32_t schema;
	int64_t minimum, maximum;
	uint64_t policy_generation;
};
struct anx_regime_event {
	uint64_t id, region_id, policy_generation;
	int64_t sample, minimum, maximum;
};
struct anx_regime_status {
	bool bound, warming_up, event_claimed;
	enum anx_regime_state state;
	uint32_t samples, return_samples;
	uint64_t region_id, event_id;
	struct anx_regime_region region;
	int64_t slow_ewma, fast_ewma;
};

/* Controller-only mutations. Reset removes the binding but preserves ID monotonicity. */
void anx_regime_init(void);
void anx_regime_reset(void);
int anx_regime_observe(int64_t sample);
int anx_regime_bind(const struct anx_regime_region *region, uint64_t *id_out);
/* One event per exit. An event permits investigation, not policy promotion. */
int anx_regime_claim(struct anx_regime_event *out);
int anx_regime_get(struct anx_regime_status *out);
enum anx_regime_state anx_regime_current(void);
#endif
