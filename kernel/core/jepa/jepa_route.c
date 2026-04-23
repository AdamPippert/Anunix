/*
 * jepa_route.c — JEPA route planner integration hook.
 *
 * Adds a bounded ±ANX_JEPA_ROUTE_DELTA_MAX score delta to the route
 * planner's engine scoring.  The delta is derived from the cosine
 * similarity between the current latent state and the predicted state
 * after committing to each route candidate.
 *
 * The delta is bounded so JEPA can nudge but never override a dominant
 * symbolic preference (a score gap > 2*delta cannot be reversed).
 */

#include "jepa_internal.h"
#include <anx/types.h>
#include <anx/route.h>
#include <anx/string.h>
#include <anx/alloc.h>

int32_t anx_jepa_route_score_delta(const struct anx_route_candidate *candidate)
{
	struct anx_jepa_ctx *ctx;
	anx_oid_t pred_oid;
	uint32_t action_id;
	float divergence;
	int rc;

	if (!candidate || !anx_jepa_available())
		return 0;

	ctx = anx_jepa_ctx_get();

	/* Require a valid current latent; zero OID means no state yet */
	{
		anx_oid_t zero;
		anx_memset(&zero, 0, sizeof(zero));
		if (anx_memcmp(&ctx->encoder_weights_oid,
			       &zero, sizeof(zero)) == 0)
			return 0;
	}

	/*
	 * Map the route candidate's engine selection to an action_id.
	 * If the candidate is the selected (primary) engine, use
	 * ANX_JEPA_ACT_ROUTE_LOCAL for local engines and
	 * ANX_JEPA_ACT_ROUTE_REMOTE otherwise.  For fallback engines
	 * use ANX_JEPA_ACT_ROUTE_FALLBACK.
	 */
	{
		struct anx_engine *eng = anx_engine_lookup(&candidate->engine_id);

		if (!eng)
			return 0;

		if (candidate->fallback_engine_id.hi != candidate->engine_id.hi ||
		    candidate->fallback_engine_id.lo != candidate->engine_id.lo) {
			/* This candidate has a fallback set — treat as primary */
			action_id = eng->is_local ?
				    ANX_JEPA_ACT_ROUTE_LOCAL :
				    ANX_JEPA_ACT_ROUTE_REMOTE;
		} else {
			action_id = ANX_JEPA_ACT_ROUTE_FALLBACK;
		}
	}

	/*
	 * Predict the post-routing latent using the most recently encoded
	 * state.  We re-use the last encode result (tracked in ctx) rather
	 * than encoding again to avoid per-candidate overhead.
	 *
	 * Since we need the last latent OID, the caller (planner.c) is
	 * responsible for calling anx_jepa_encode_obs() once before
	 * invoking anx_route_score_engine() in a scoring loop.  If no
	 * latent has been produced, we return 0.
	 */
	{
		/* Placeholder until planner wiring is complete. */
		(void)action_id;
		return 0;
	}

	/* Unreachable until planner wiring is complete (Task 10). */
	(void)pred_oid;
	(void)divergence;
	(void)rc;

	return 0;
}
