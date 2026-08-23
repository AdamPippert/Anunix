/*
 * test_regime_detector.c — Regime Detector (RFC-0029).
 */

#include <anx/types.h>
#include <anx/regime.h>

int test_regime_detector(void)
{
	uint32_t i;

	anx_regime_init();

	/* Stationary trace with small noise stays STABLE. */
	for (i = 0; i < 200; i++) {
		int64_t noise = (int64_t)(i % 7) - 3;	/* -3..3 */
		if (anx_regime_observe(1000 + noise) != ANX_OK)
			return -1;
	}
	if (anx_regime_current() != ANX_REGIME_STABLE)
		return -2;

	/* Sudden, sustained shift far outside the noise band escalates. */
	for (i = 0; i < 20; i++) {
		if (anx_regime_observe(5000) != ANX_OK)
			return -3;
	}
	if (anx_regime_current() != ANX_REGIME_ESCALATED)
		return -4;

	/* Once the trace stabilizes at the new level long enough for the
	 * slow baseline to catch up, the detector returns to STABLE. */
	for (i = 0; i < 200; i++) {
		int64_t noise = (int64_t)(i % 7) - 3;
		if (anx_regime_observe(5000 + noise) != ANX_OK)
			return -5;
	}
	if (anx_regime_current() != ANX_REGIME_STABLE)
		return -6;

	/* Reset returns to a clean, stable, zero-sample state. */
	anx_regime_reset();
	if (anx_regime_current() != ANX_REGIME_STABLE)
		return -7;

	/* Below ANX_REGIME_MIN_SAMPLES, even a huge single jump must not
	 * escalate — the detector suppresses judgment during warmup. */
	if (anx_regime_observe(0) != ANX_OK)
		return -8;
	if (anx_regime_observe(1000000) != ANX_OK)
		return -8;
	if (anx_regime_current() != ANX_REGIME_STABLE)
		return -9;

	/* Bad args on the null-checked calls (observe has none — sample is
	 * a plain value, not a pointer) are not applicable; current() has
	 * no failure mode by design (always returns a valid enum value). */

	return 0;
}
