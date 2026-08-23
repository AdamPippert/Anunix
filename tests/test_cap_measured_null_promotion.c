/*
 * test_cap_measured_null_promotion.c — Measured-null promotion gate
 * (RFC-0029), extending RFC-0007's capability lifecycle.
 */

#include <anx/types.h>
#include <anx/capability.h>
#include <anx/engine.h>
#include <anx/state_object.h>
#include <anx/uuid.h>

static int make_validated_successor(const char *name,
				    const anx_oid_t *supersedes,
				    struct anx_capability **out)
{
	struct anx_capability *cap;
	int ret;

	ret = anx_cap_create(name, "1.0", &cap);
	if (ret != ANX_OK)
		return ret;

	cap->supersedes_oid = *supersedes;

	ret = anx_cap_validate(cap);
	if (ret != ANX_OK)
		return ret;

	*out = cap;
	return ANX_OK;
}

int test_cap_measured_null_promotion(void)
{
	struct anx_capability *incumbent;
	struct anx_capability *winner;
	struct anx_capability *noisy;
	struct anx_promotion_trial trial;
	bool promote;
	int ret;
	uint32_t i;

	anx_objstore_init();
	anx_engine_registry_init();
	anx_cap_store_init();

	/* Install a fresh incumbent (no supersedes_oid — plain install). */
	ret = anx_cap_create("incumbent", "1.0", &incumbent);
	if (ret != ANX_OK)
		return -1;
	ret = anx_cap_validate(incumbent);
	if (ret != ANX_OK)
		return -2;
	ret = anx_cap_install(incumbent);
	if (ret != ANX_OK)
		return -3;

	/* anx_promotion_gate_evaluate: bad args */
	if (anx_promotion_gate_evaluate(NULL, 1, &promote) != ANX_EINVAL)
		return -4;
	{
		struct anx_promotion_trial empty;
		empty.n = 0;
		if (anx_promotion_gate_evaluate(&empty, 1, &promote) != ANX_EINVAL)
			return -5;
	}

	/* Case 1: candidate genuinely better on every paired run (margin
	 * 20 >= base margin 5*1) -> promotes at num_candidates_tried=1. */
	trial.n = 8;
	for (i = 0; i < trial.n; i++) {
		trial.incumbent_scores[i] = 50;
		trial.candidate_scores[i] = 70;	/* +20 every time */
	}
	promote = false;
	ret = anx_promotion_gate_evaluate(&trial, 1, &promote);
	if (ret != ANX_OK)
		return -6;
	if (!promote)
		return -7;

	/* Case 2: noise-level differences straddling zero -> never
	 * promotes, regardless of a decent mean. */
	trial.n = 6;
	trial.incumbent_scores[0] = 50; trial.candidate_scores[0] = 55;
	trial.incumbent_scores[1] = 50; trial.candidate_scores[1] = 48;	/* worse */
	trial.incumbent_scores[2] = 50; trial.candidate_scores[2] = 53;
	trial.incumbent_scores[3] = 50; trial.candidate_scores[3] = 52;
	trial.incumbent_scores[4] = 50; trial.candidate_scores[4] = 51;
	trial.incumbent_scores[5] = 50; trial.candidate_scores[5] = 54;
	promote = true;
	ret = anx_promotion_gate_evaluate(&trial, 1, &promote);
	if (ret != ANX_OK)
		return -8;
	if (promote)
		return -9;	/* must reject: one paired run was WORSE */

	/* Case 3: multiplicity correction is real, not cosmetic. A trial
	 * whose worst margin is exactly 12 promotes when only 1 candidate
	 * was tried (needs >= 5) but not when 4 were tried (needs >= 20). */
	trial.n = 4;
	for (i = 0; i < trial.n; i++) {
		trial.incumbent_scores[i] = 50;
		trial.candidate_scores[i] = 62;	/* +12 every time */
	}
	promote = false;
	ret = anx_promotion_gate_evaluate(&trial, 1, &promote);
	if (ret != ANX_OK || !promote)
		return -10;

	promote = true;
	ret = anx_promotion_gate_evaluate(&trial, 4, &promote);
	if (ret != ANX_OK)
		return -11;
	if (promote)
		return -12;	/* same trial, more candidates tried -> rejected */

	/* --- Wire the gate into anx_cap_install / anx_cap_install_gated --- */

	/* A candidate declaring supersedes_oid must go through the gated
	 * path — plain anx_cap_install() refuses it outright. */
	ret = make_validated_successor("winner", &incumbent->cap_oid, &winner);
	if (ret != ANX_OK)
		return -13;
	if (anx_cap_install(winner) != ANX_EPERM)
		return -14;

	/* Gated install with no incumbent-comparison data (n=0) is EINVAL,
	 * already covered above; a genuinely nil supersedes_oid must use
	 * the plain path instead of the gated one. */
	{
		struct anx_capability *fresh;
		ret = anx_cap_create("fresh", "1.0", &fresh);
		if (ret != ANX_OK)
			return -15;
		ret = anx_cap_validate(fresh);
		if (ret != ANX_OK)
			return -15;
		trial.n = 1;
		trial.incumbent_scores[0] = 0;
		trial.candidate_scores[0] = 100;
		if (anx_cap_install_gated(fresh, &trial, 1) != ANX_EINVAL)
			return -16;
	}

	/* Gated install with a losing trial is rejected and the candidate
	 * stays VALIDATED (not installed). */
	trial.n = 6;
	trial.incumbent_scores[0] = 50; trial.candidate_scores[0] = 55;
	trial.incumbent_scores[1] = 50; trial.candidate_scores[1] = 48;
	trial.incumbent_scores[2] = 50; trial.candidate_scores[2] = 53;
	trial.incumbent_scores[3] = 50; trial.candidate_scores[3] = 52;
	trial.incumbent_scores[4] = 50; trial.candidate_scores[4] = 51;
	trial.incumbent_scores[5] = 50; trial.candidate_scores[5] = 54;
	ret = make_validated_successor("noisy_challenger", &incumbent->cap_oid, &noisy);
	if (ret != ANX_OK)
		return -17;
	ret = anx_cap_install_gated(noisy, &trial, 1);
	if (ret != ANX_EPERM)
		return -18;
	if (noisy->status != ANX_CAP_VALIDATED)
		return -19;

	/* Gated install with a clearly winning trial succeeds. */
	trial.n = 8;
	for (i = 0; i < trial.n; i++) {
		trial.incumbent_scores[i] = 50;
		trial.candidate_scores[i] = 70;
	}
	ret = anx_cap_install_gated(winner, &trial, 1);
	if (ret != ANX_OK)
		return -20;
	if (winner->status != ANX_CAP_INSTALLED)
		return -21;
	if (anx_uuid_is_nil(&winner->installed_engine_id))
		return -22;

	return 0;
}
