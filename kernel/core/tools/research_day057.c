/* Calibrated regions gate investigation without granting policy authority. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/regime.h>
#include <anx/tuning.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>

static int foreign(struct anx_external_call *call, void *context)
{
	struct anx_regime_status before, after;
	struct anx_regime_event rejected, copy;
	uint64_t id = 123;
	(void)call; (void)context;
	anx_regime_get(&before);
	anx_memset(&rejected, 0x55, sizeof(rejected)); copy = rejected;
	if (anx_regime_observe(999999) != ANX_EPERM ||
	    anx_regime_bind(&before.region, &id) != ANX_EPERM || id != 123 ||
	    anx_regime_claim(&rejected) != ANX_EPERM || anx_memcmp(&rejected, &copy, sizeof(copy)))
		return -5710;
	anx_regime_init(); anx_regime_reset();
	anx_regime_get(&after);
	return anx_memcmp(&before, &after, sizeof(before)) ? -5711 : ANX_OK;
}

int anx_research_day057(void)
{
	struct anx_route_tuning_state original, current;
	struct anx_regime_status status, saved;
	struct anx_regime_event event, rejected, untouched;
	struct anx_route_tuning_action action = {0};
	struct anx_cell *caller = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	uint64_t region_id = 0, trial = 0, denied = 123;
	int ret = anx_route_tuning_snapshot(&original);
	if (ret != ANX_OK || original.trial_active) return ANX_EBUSY;
	struct anx_regime_region region = { .schema = 1, .minimum = 850, .maximum = 1150,
		.policy_generation = original.generation };
	anx_regime_reset();
	ret = anx_regime_bind(&region, &region_id);
	if (ret != ANX_OK) goto out;
	/* Keep the original baseline's sustained-change observation. */
	for (uint32_t i = 0; i < 200; i++) anx_regime_observe(1000);
	ret = -5709;
	if (anx_regime_current() != ANX_REGIME_STABLE || anx_regime_claim(&rejected) != ANX_EBUSY)
		goto out;
	for (uint32_t i = 0; i < 220; i++) anx_regime_observe(5000);
	ret = -5701;
	if (anx_regime_current() != ANX_REGIME_ESCALATED) goto out;
	anx_regime_get(&status);
	ret = -5702;
	if (!status.bound || status.warming_up || status.region_id != region_id ||
	    status.fast_ewma - status.slow_ewma >= ANX_REGIME_STABLE_THRESHOLD ||
	    anx_regime_claim(&event) != ANX_OK || event.sample != 5000 ||
	    event.region_id != region_id || event.policy_generation != original.generation) goto out;
	anx_memset(&rejected, 0x55, sizeof(rejected)); untouched = rejected;
	for (uint32_t i = 0; i < 200; i++) {
		if (anx_regime_observe(5000) != ANX_OK || anx_regime_claim(&rejected) != ANX_EBUSY)
			goto out;
	}
	if (anx_memcmp(&untouched, &rejected, sizeof(rejected))) goto out;
	/* Expensive investigation has no authority to bypass the typed feasibility gate. */
	action.schema = 1; action.expected_generation = original.generation;
	action.weights = original.weights; action.weights.cpu_cost_divisor = 0;
	ret = -5703;
	if (anx_route_tuning_begin(&action, &denied) != ANX_EINVAL || denied != 123 ||
	    anx_route_tuning_snapshot(&current) != ANX_OK || anx_memcmp(&current, &original, sizeof(current))) goto out;
	action.weights = original.weights; action.weights.locality_bonus++;
	ret = anx_route_tuning_begin(&action, &trial);
	if (ret != ANX_OK) goto out;
	ret = -5704;
	struct anx_regime_region during = region;
	during.policy_generation = trial;
	if (anx_regime_bind(&during, &denied) != ANX_EBUSY || denied != 123) goto out;
	if (anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT) != ANX_OK) goto out;
	trial = 0;
	anx_route_tuning_snapshot(&current);
	if (current.trial_active || current.generation != original.generation + 2 ||
	    anx_memcmp(&current.weights, &original.weights, sizeof(current.weights))) goto out;
	/* Three consecutive in-range samples end an exit; a boundary bounce restarts the count. */
	anx_regime_observe(850); anx_regime_observe(1150);
	if (anx_regime_current() != ANX_REGIME_ESCALATED) goto out;
	anx_regime_observe(1151); anx_regime_observe(1000); anx_regime_observe(1000);
	if (anx_regime_current() != ANX_REGIME_ESCALATED) goto out;
	anx_regime_observe(1000);
	if (anx_regime_current() != ANX_REGIME_STABLE || anx_regime_claim(&rejected) != ANX_EBUSY) goto out;
	anx_regime_observe(849); anx_regime_get(&status);
	ret = -5705;
	if (status.event_id <= event.id || status.event_claimed ||
	    anx_regime_claim(&rejected) != ANX_EBUSY ||
	    anx_regime_bind(&region, &denied) != ANX_EBUSY || denied != 123) goto out;
	anx_regime_get(&saved);
	if (anx_memcmp(&status, &saved, sizeof(saved))) goto out;
	/* A rejected trial restores weights, but its generation change needs an explicit new binding. */
	region.policy_generation = current.generation;
	ret = anx_regime_bind(&region, &region_id);
	if (ret != ANX_OK) goto out;
	region.minimum = region.maximum = 999999; /* Caller mutation cannot change the private binding. */
	for (uint32_t i = 0; i < ANX_REGIME_MIN_SAMPLES - 1; i++) anx_regime_observe(5000);
	anx_regime_get(&status);
	ret = -5706;
	if (!status.warming_up || status.state != ANX_REGIME_STABLE ||
	    status.region.minimum != 850 || status.region.maximum != 1150 ||
	    anx_regime_claim(&rejected) != ANX_EBUSY || region_id <= event.id) goto out;
	anx_regime_observe(5000);
	if (anx_regime_claim(&event) != ANX_OK || event.policy_generation != current.generation) goto out;
	ret = anx_external_register_handler("anxresearch057", foreign, NULL);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call)); ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch057://observe", sizeof(call->endpoint));
	anx_strlcpy(intent.name, "research-day-057", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (ret != ANX_OK) goto out;
	caller->ext_call = call; caller->execution.allow_side_effects = true;
	ret = anx_cell_run(caller);
	if (ret != ANX_OK) goto out;
	/* Invalid calibration preserves both the published state and caller output. */
	anx_regime_get(&saved);
	region.minimum = 2; region.maximum = 1;
	ret = -5707;
	if (anx_regime_bind(&region, &denied) != ANX_EINVAL || denied != 123 ||
	    anx_regime_bind(NULL, &denied) != ANX_EINVAL || anx_regime_claim(NULL) != ANX_EINVAL ||
	    anx_regime_get(NULL) != ANX_EINVAL) goto out;
	anx_regime_get(&status);
	if (anx_memcmp(&saved, &status, sizeof(status))) goto out;
	/* Full-range inputs exercise safe EWMA arithmetic and unbound abstention. */
	anx_regime_reset();
	ret = -5708;
	if (anx_regime_claim(&rejected) != ANX_ENOTSUP) goto out;
	int64_t highest = (int64_t)(~(uint64_t)0 >> 1), lowest = -highest - 1;
	anx_regime_observe(lowest); anx_regime_observe(highest);
	anx_regime_get(&status);
	if (status.fast_ewma != lowest + (int64_t)(~(uint64_t)0 / ANX_REGIME_FAST_WINDOW) ||
	    status.slow_ewma != lowest + (int64_t)(~(uint64_t)0 / ANX_REGIME_SLOW_WINDOW)) goto out;
	for (uint32_t i = 0; i < 4096; i++)
		if (anx_regime_observe(i % 2 ? lowest : highest) != ANX_OK) goto out;
	region.minimum = lowest; region.maximum = highest;
	if (anx_regime_bind(&region, &region_id) != ANX_OK || region_id <= event.id) goto out;
	for (uint32_t i = 0; i < 200; i++) anx_regime_observe(i % 2 ? lowest : highest);
	if (anx_regime_current() != ANX_REGIME_STABLE || anx_regime_claim(&rejected) != ANX_EBUSY ||
	    anx_memcmp(&untouched, &rejected, sizeof(rejected))) goto out;
	ret = ANX_OK;
out:
	if (trial) anx_route_tuning_finish(trial, ANX_ROUTE_TRIAL_REJECT);
	anx_regime_reset();
	anx_external_unregister_handler("anxresearch057");
	if (caller) anx_cell_destroy(caller);
	anx_free(call);
	return ret;
}
#endif
