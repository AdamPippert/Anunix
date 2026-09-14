#include <anx/regime.h>
#include <anx/spinlock.h>
#include <anx/string.h>
#include <anx/cell.h>
#include <anx/tuning.h>

static struct anx_spinlock regime_lock = ANX_SPINLOCK_INIT;
static struct anx_regime_status detector;
static struct anx_regime_event event;
static uint64_t next_id;

static void clear_locked(void)
{
	anx_memset(&detector, 0, sizeof(detector));
	anx_memset(&event, 0, sizeof(event));
	detector.warming_up = true;
}
void anx_regime_reset(void)
{
	bool irq;
	if (anx_cell_current_id()) return;
	anx_spin_lock_irqsave(&regime_lock, &irq);
	clear_locked();
	anx_spin_unlock_irqrestore(&regime_lock, irq);
}
void anx_regime_init(void) { anx_regime_reset(); }

int anx_regime_bind(const struct anx_regime_region *region, uint64_t *id_out)
{
	struct anx_regime_region copy;
	struct anx_route_tuning_state policy;
	bool irq;
	int ret = ANX_OK;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!region || !id_out) return ANX_EINVAL;
	copy = *region;
	if (copy.schema != 1 || copy.minimum > copy.maximum || !copy.policy_generation)
		return ANX_EINVAL;
	anx_spin_lock_irqsave(&regime_lock, &irq);
	anx_route_tuning_snapshot(&policy);
	if (policy.trial_active || copy.policy_generation != policy.generation) ret = ANX_EBUSY;
	else if (next_id == ~(uint64_t)0) ret = ANX_EFULL;
	else {
		clear_locked();
		detector.bound = true;
		detector.region.schema = copy.schema;
		detector.region.minimum = copy.minimum;
		detector.region.maximum = copy.maximum;
		detector.region.policy_generation = copy.policy_generation;
		*id_out = detector.region_id = ++next_id;
	}
	anx_spin_unlock_irqrestore(&regime_lock, irq);
	return ret;
}

/* Unsigned distances cover the full signed range without subtraction overflow. */
static uint64_t distance(int64_t a, int64_t b)
{
	return a >= b ? (uint64_t)a - (uint64_t)b : (uint64_t)b - (uint64_t)a;
}
static int64_t update(int64_t value, int64_t sample, uint32_t window)
{
	int64_t step = (int64_t)(distance(sample, value) / window);
	return sample >= value ? value + step : value - step;
}

int anx_regime_observe(int64_t sample)
{
	bool irq;
	int ret = ANX_OK;
	if (anx_cell_current_id()) return ANX_EPERM;
	anx_spin_lock_irqsave(&regime_lock, &irq);
	if (!detector.samples) detector.slow_ewma = detector.fast_ewma = sample;
	else {
		detector.slow_ewma = update(detector.slow_ewma, sample, ANX_REGIME_SLOW_WINDOW);
		detector.fast_ewma = update(detector.fast_ewma, sample, ANX_REGIME_FAST_WINDOW);
	}
	if (detector.samples < ANX_REGIME_MIN_SAMPLES) detector.samples++;
	detector.warming_up = detector.samples < ANX_REGIME_MIN_SAMPLES;
	if (detector.warming_up) goto out;
	if (!detector.bound) {
		uint64_t divergence = distance(detector.fast_ewma, detector.slow_ewma);
		if (detector.state == ANX_REGIME_STABLE && divergence > ANX_REGIME_ESCALATE_THRESHOLD)
			detector.state = ANX_REGIME_ESCALATED;
		else if (detector.state == ANX_REGIME_ESCALATED && divergence < ANX_REGIME_STABLE_THRESHOLD)
			detector.state = ANX_REGIME_STABLE;
	} else if (sample < detector.region.minimum || sample > detector.region.maximum) {
		detector.return_samples = 0;
		if (detector.state == ANX_REGIME_STABLE) {
			detector.state = ANX_REGIME_ESCALATED;
			if (next_id == ~(uint64_t)0) { ret = ANX_EFULL; goto out; }
			event.id = detector.event_id = ++next_id;
			event.region_id = detector.region_id;
			event.policy_generation = detector.region.policy_generation;
			event.sample = sample;
			event.minimum = detector.region.minimum;
			event.maximum = detector.region.maximum;
			detector.event_claimed = false;
		}
	} else if (detector.state == ANX_REGIME_ESCALATED &&
		   ++detector.return_samples >= ANX_REGIME_RETURN_SAMPLES) {
		detector.state = ANX_REGIME_STABLE;
		detector.return_samples = 0;
		detector.event_id = 0;
		detector.event_claimed = false;
		anx_memset(&event, 0, sizeof(event));
	}
out:
	anx_spin_unlock_irqrestore(&regime_lock, irq);
	return ret;
}

int anx_regime_claim(struct anx_regime_event *out)
{
	struct anx_route_tuning_state policy;
	bool irq;
	int ret = ANX_OK;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&regime_lock, &irq);
	anx_route_tuning_snapshot(&policy);
	if (!detector.bound) ret = ANX_ENOTSUP;
	else if (detector.warming_up || detector.state != ANX_REGIME_ESCALATED ||
		 !event.id || detector.event_claimed || policy.trial_active ||
		 policy.generation != event.policy_generation) ret = ANX_EBUSY;
	else { *out = event; detector.event_claimed = true; }
	anx_spin_unlock_irqrestore(&regime_lock, irq);
	return ret;
}
int anx_regime_get(struct anx_regime_status *out)
{
	bool irq;
	if (!out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&regime_lock, &irq);
	*out = detector;
	anx_spin_unlock_irqrestore(&regime_lock, irq);
	return ANX_OK;
}
enum anx_regime_state anx_regime_current(void)
{
	struct anx_regime_status copy;
	anx_regime_get(&copy);
	return copy.state;
}
