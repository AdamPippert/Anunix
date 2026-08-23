/*
 * regime.c — Regime Detector implementation.
 *
 * Two-EWMA change-point detector with hysteresis. Single global
 * detector, matching the singleton-with-init() convention used by
 * sched/scheduler.c and route/feedback.c — this is one system-wide
 * "has telemetry left the validated envelope" signal, not a per-caller
 * instance.
 */

#include <anx/types.h>
#include <anx/regime.h>
#include <anx/spinlock.h>

static struct anx_spinlock regime_lock;
static int64_t slow_ewma;
static int64_t fast_ewma;
static uint32_t sample_count;
static enum anx_regime_state state;

void anx_regime_init(void)
{
	anx_spin_init(&regime_lock);
	anx_regime_reset();
}

void anx_regime_reset(void)
{
	anx_spin_lock(&regime_lock);
	slow_ewma = 0;
	fast_ewma = 0;
	sample_count = 0;
	state = ANX_REGIME_STABLE;
	anx_spin_unlock(&regime_lock);
}

static int64_t iabs64(int64_t v)
{
	return v < 0 ? -v : v;
}

int anx_regime_observe(int64_t sample)
{
	int64_t divergence;

	anx_spin_lock(&regime_lock);

	if (sample_count == 0) {
		slow_ewma = sample;
		fast_ewma = sample;
	} else {
		/* ewma += (sample - ewma) / window */
		slow_ewma += (sample - slow_ewma) / ANX_REGIME_SLOW_WINDOW;
		fast_ewma += (sample - fast_ewma) / ANX_REGIME_FAST_WINDOW;
	}
	if (sample_count < ANX_REGIME_MIN_SAMPLES + 1)
		sample_count++;

	divergence = iabs64(fast_ewma - slow_ewma);

	if (sample_count >= ANX_REGIME_MIN_SAMPLES) {
		if (state == ANX_REGIME_STABLE &&
		    divergence > ANX_REGIME_ESCALATE_THRESHOLD)
			state = ANX_REGIME_ESCALATED;
		else if (state == ANX_REGIME_ESCALATED &&
			 divergence < ANX_REGIME_STABLE_THRESHOLD)
			state = ANX_REGIME_STABLE;
	}

	anx_spin_unlock(&regime_lock);
	return ANX_OK;
}

enum anx_regime_state anx_regime_current(void)
{
	enum anx_regime_state s;

	anx_spin_lock(&regime_lock);
	s = state;
	anx_spin_unlock(&regime_lock);

	return s;
}
