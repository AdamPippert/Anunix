/*
 * ebm_constraint.c — Constraint compliance energy scorer (RFC-0020 Phase 3).
 *
 * Measures how much of the session's iteration budget has been consumed.
 * Near-budget sessions pay higher energy, biasing arbitration to commit
 * sooner.  Returns a low base energy when no budget constraint is active.
 */

#include <anx/ebm.h>
#include <anx/loop.h>
#include <anx/string.h>

float ebm_constraint_compliance(anx_oid_t session_oid,
				anx_oid_t proposal_oid __attribute__((unused)))
{
	struct anx_loop_session *s;
	float ratio;

	s = anx_loop_session_get(session_oid);
	if (!s)
		return 0.5f;

	if (s->max_iterations == 0)
		return 0.1f;	/* unconstrained: low energy */

	ratio = (float)s->iteration / (float)s->max_iterations;

	/*
	 * Linear ramp: 0.1 at start → 0.6 near budget exhaustion.
	 * Never reaches 1.0 — budget halting is enforced separately.
	 */
	if (ratio < 0.0f) ratio = 0.0f;
	if (ratio > 1.0f) ratio = 1.0f;
	return 0.1f + ratio * 0.5f;
}
