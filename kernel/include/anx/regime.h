/*
 * anx/regime.h — Regime Detector (RFC-0029).
 *
 * A small, deterministic change-point detector that gates *whether* an
 * AI-proposed scheduling/routing policy is even considered. It does not
 * generate or evaluate policy itself — it only decides whether current
 * telemetry has left a validated operating envelope. Escalation is the
 * trigger to run the (separately gated) promotion pipeline; it is not
 * itself a promotion decision.
 *
 * Design: two EWMAs (a slow baseline, a fast tracker) over an arbitrary
 * caller-chosen integer metric. Escalates when the trackers diverge past
 * ANX_REGIME_ESCALATE_THRESHOLD; returns to stable only once they
 * reconverge under the smaller ANX_REGIME_STABLE_THRESHOLD (hysteresis,
 * to avoid flapping at the boundary).
 */

#ifndef ANX_REGIME_H
#define ANX_REGIME_H

#include <anx/types.h>

enum anx_regime_state {
	ANX_REGIME_STABLE,
	ANX_REGIME_ESCALATED,
};

/* Divergence thresholds, in the same units as the observed samples. */
#define ANX_REGIME_ESCALATE_THRESHOLD	200
#define ANX_REGIME_STABLE_THRESHOLD	50

/* EWMA smoothing windows (larger = slower to move). */
#define ANX_REGIME_SLOW_WINDOW		32
#define ANX_REGIME_FAST_WINDOW		4

/* Minimum samples observed before the detector will report ESCALATED —
 * avoids a false escalation from startup transients before the slow
 * baseline has meaningfully converged. */
#define ANX_REGIME_MIN_SAMPLES		ANX_REGIME_SLOW_WINDOW

void anx_regime_init(void);

/* Reset all observed state back to freshly-initialized (test isolation). */
void anx_regime_reset(void);

/* Feed one telemetry sample (caller picks the metric and its units —
 * e.g. observed_latency_ns from a route feedback record). */
int anx_regime_observe(int64_t sample);

enum anx_regime_state anx_regime_current(void);

#endif /* ANX_REGIME_H */
