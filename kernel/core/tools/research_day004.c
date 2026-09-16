/* Native promotion checks against an installed incumbent. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/capability.h>
#include <anx/engine.h>
#include <anx/uuid.h>

static void retire_test_cap(struct anx_capability *cap)
{
	if (!cap)
		return;
	if (cap->status == ANX_CAP_INSTALLED || cap->status == ANX_CAP_SUSPENDED)
		anx_cap_uninstall(cap);
	anx_cap_transition(cap, ANX_CAP_RETIRED);
}

int anx_research_day004(void)
{
	struct anx_promotion_trial trial = {0};
	struct anx_capability *incumbent = NULL, *candidate = NULL;
	struct anx_engine *incumbent_engine;
	anx_oid_t unknown;
	bool promote = true;
	int rc;

	trial.n = 1;
	trial.incumbent_scores[0] = trial.candidate_scores[0] = 50;
	if (anx_promotion_gate_evaluate(&trial, 0xffffffffU, &promote) != ANX_OK || promote)
		return -401;
	trial.incumbent_scores[0] = 2147483647;
	trial.candidate_scores[0] = -2147483647 - 1 + 100;
	if (anx_promotion_gate_evaluate(&trial, 1, &promote) != ANX_OK || promote)
		return -402;
	trial.incumbent_scores[0] = -2147483647 - 1;
	trial.candidate_scores[0] = 2147483647;
	if (anx_promotion_gate_evaluate(&trial, 1, &promote) != ANX_OK || !promote)
		return -403;
	if (anx_promotion_gate_evaluate(&trial, 0xffffffffU, &promote) != ANX_OK || promote)
		return -404;
	trial.n = 0;
	promote = true;
	if (anx_promotion_gate_evaluate(&trial, 1, &promote) != ANX_EINVAL || promote)
		return -405;

	rc = anx_cap_create("research-day-004-incumbent", "1", &incumbent);
	if (rc != ANX_OK)
		goto out;
	rc = anx_cap_validate(incumbent);
	if (rc != ANX_OK)
		goto out;
	rc = anx_cap_install(incumbent);
	if (rc != ANX_OK)
		goto out;
	incumbent_engine = anx_engine_lookup(&incumbent->installed_engine_id);
	rc = anx_cap_create("research-day-004-candidate", "2", &candidate);
	if (rc != ANX_OK)
		goto out;
	candidate->supersedes_oid = incumbent->cap_oid;
	rc = anx_cap_validate(candidate);
	if (rc != ANX_OK)
		goto out;
	trial.n = 2;
	trial.incumbent_scores[0] = trial.incumbent_scores[1] = 50;
	trial.candidate_scores[0] = trial.candidate_scores[1] = 50;
	if (anx_cap_install_gated(candidate, &trial, 1) != ANX_EPERM ||
	    incumbent->status != ANX_CAP_INSTALLED || candidate->status != ANX_CAP_VALIDATED ||
	    !anx_uuid_is_nil(&candidate->installed_engine_id) ||
	    anx_engine_lookup(&incumbent->installed_engine_id) != incumbent_engine) {
		rc = -406;
		goto out;
	}
	trial.candidate_scores[0] = trial.candidate_scores[1] = 75;
	anx_uuid_generate(&unknown);
	candidate->supersedes_oid = unknown;
	if (anx_cap_install_gated(candidate, &trial, 1) != ANX_ENOENT ||
	    candidate->status != ANX_CAP_VALIDATED) {
		rc = -407;
		goto out;
	}
	candidate->supersedes_oid = incumbent->cap_oid;
	if (anx_cap_transition(incumbent, ANX_CAP_SUSPENDED) != ANX_OK ||
	    anx_cap_install_gated(candidate, &trial, 1) != ANX_EPERM ||
	    candidate->status != ANX_CAP_VALIDATED ||
	    anx_cap_transition(incumbent, ANX_CAP_INSTALLED) != ANX_OK) {
		rc = -409;
		goto out;
	}
	if (anx_cap_install_gated(candidate, &trial, 1) != ANX_OK ||
	    candidate->status != ANX_CAP_INSTALLED ||
	    !anx_engine_lookup(&candidate->installed_engine_id)) {
		rc = -408;
		goto out;
	}
	rc = ANX_OK;
out:
	retire_test_cap(candidate);
	retire_test_cap(incumbent);
	return rc;
}
#endif
