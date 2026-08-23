/*
 * promotion.c — Measured-null promotion gate (RFC-0029).
 *
 * See kernel/include/anx/capability.h for the full rationale (pure
 * integer worst-case min-margin test, deliberately not a t-test — this
 * kernel's floating point is restricted to a small whitelist of
 * directories that accept the "never runs in interrupt context"
 * constraint, and a capability promotion decision has no reason to
 * take that on).
 */

#include <anx/types.h>
#include <anx/capability.h>

int anx_promotion_gate_evaluate(const struct anx_promotion_trial *trial,
				uint32_t num_candidates_tried,
				bool *promote_out)
{
	int32_t min_margin;
	int32_t required_margin;
	uint32_t i;

	if (!trial || !promote_out)
		return ANX_EINVAL;
	if (trial->n == 0 || trial->n > ANX_PROMOTION_TRIAL_MAX)
		return ANX_EINVAL;
	if (num_candidates_tried == 0)
		num_candidates_tried = 1;

	*promote_out = false;

	min_margin = trial->candidate_scores[0] - trial->incumbent_scores[0];
	for (i = 1; i < trial->n; i++) {
		int32_t margin = trial->candidate_scores[i] -
				  trial->incumbent_scores[i];
		if (margin < min_margin)
			min_margin = margin;
	}

	required_margin = ANX_PROMOTION_MIN_MARGIN_BASE *
			   (int32_t)num_candidates_tried;

	if (min_margin >= required_margin)
		*promote_out = true;

	return ANX_OK;
}
