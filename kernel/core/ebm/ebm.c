/*
 * ebm.c — Energy-Based Model scorer registry and iteration driver (RFC-0020 Phase 3).
 *
 * Registers four energy scorers (world consistency, goal alignment,
 * constraint compliance, epistemic uncertainty) and exposes two entry
 * points: anx_ebm_score_proposal() for single-proposal scoring and
 * anx_ebm_run_iteration() for the full belief→propose→score→arbitrate
 * pipeline that drives one IBAL iteration.
 */

#include <anx/ebm.h>
#include <anx/loop.h>
#include <anx/jepa.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Scorer registry                                                     */
/* ------------------------------------------------------------------ */

typedef float (*ebm_score_fn)(anx_oid_t session_oid, anx_oid_t proposal_oid);

struct ebm_scorer_entry {
	char         id[32];
	ebm_score_fn fn;
	float        weight;
};

/* Implemented in ebm_world.c, ebm_goal.c, ebm_constraint.c, ebm_uncertainty.c */
float ebm_world_consistency(anx_oid_t, anx_oid_t);
float ebm_goal_alignment(anx_oid_t, anx_oid_t);
float ebm_constraint_compliance(anx_oid_t, anx_oid_t);
float ebm_epistemic_uncertainty(anx_oid_t, anx_oid_t);

static const struct ebm_scorer_entry g_scorers[] = {
	{ "world_consistency",     ebm_world_consistency,     0.40f },
	{ "goal_alignment",        ebm_goal_alignment,        0.30f },
	{ "constraint_compliance", ebm_constraint_compliance, 0.20f },
	{ "epistemic_uncertainty", ebm_epistemic_uncertainty, 0.10f },
};

#define EBM_SCORER_COUNT  ((uint32_t)(sizeof(g_scorers) / sizeof(g_scorers[0])))

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

int anx_ebm_init(void)
{
	kprintf("[ebm] initialized: %u scorers (world/goal/constraint/uncertainty)\n",
		EBM_SCORER_COUNT);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Single-proposal scoring                                             */
/* ------------------------------------------------------------------ */

int anx_ebm_score_proposal(anx_oid_t session_oid, anx_oid_t proposal_oid,
			    float *energy_out)
{
	float    aggregate = 0.0f;
	uint32_t i;

	if (!energy_out)
		return ANX_EINVAL;

	for (i = 0; i < EBM_SCORER_COUNT; i++) {
		float e = g_scorers[i].fn(session_oid, proposal_oid);
		anx_oid_t score_oid;
		enum anx_loop_score_class cls;

		/* Clamp to [0, 1] */
		if (e < 0.0f) e = 0.0f;
		if (e > 1.0f) e = 1.0f;

		cls = (e < 0.40f) ? ANX_LOOP_SCORE_ACCEPT  :
		      (e < 0.70f) ? ANX_LOOP_SCORE_MARGINAL :
				    ANX_LOOP_SCORE_REJECT;

		(void)anx_loop_score_create(session_oid, proposal_oid,
					    g_scorers[i].id, e, cls,
					    1.0f - e, &score_oid);

		aggregate += e * g_scorers[i].weight;
	}

	*energy_out = aggregate;

	if (aggregate >= ANX_EBM_REJECT_THRESH) {
		(void)anx_loop_counterexample_record(session_oid, proposal_oid,
			ANX_LOOP_REJECT_ENERGY_TOO_HIGH, aggregate,
			"ebm: aggregate energy above reject threshold");
	}

	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Full iteration driver                                               */
/* ------------------------------------------------------------------ */

int anx_ebm_run_iteration(anx_oid_t session_oid,
			   struct anx_loop_session_action_stats *stats,
			   uint32_t stats_count)
{
	struct anx_loop_session *s;
	anx_oid_t  belief_oid;
	anx_oid_t  pred_oid;
	/* +1 for the LLM proposal added after the JEPA candidates */
	anx_oid_t  prop_oids[ANX_EBM_PROPOSALS_PER_ITER + 1];
	uint32_t   act_ids[ANX_EBM_PROPOSALS_PER_ITER + 1];
	float      energies[ANX_EBM_PROPOSALS_PER_ITER + 1];
	float      best_energy  = 1.0f;
	anx_oid_t  best_prop;
	uint32_t   best_act_id  = 0;
	uint32_t   n_props      = 0;
	uint32_t   i;
	int        rc;
	enum anx_loop_arb_decision decision;

	anx_memset(&best_prop, 0, sizeof(best_prop));
	anx_memset(prop_oids,  0, sizeof(prop_oids));
	anx_memset(energies,   0, sizeof(energies));

	s = anx_loop_session_get(session_oid);
	if (!s || s->status != ANX_LOOP_RUNNING)
		return ANX_EINVAL;

	/* Step 1: create belief state for this iteration */
	rc = anx_loop_belief_create(session_oid, s->iteration,
				    s->active_belief, &belief_oid);
	if (rc == ANX_OK)
		(void)anx_loop_session_set_belief(session_oid, belief_oid);

	/* Step 2: generate ANX_EBM_PROPOSALS_PER_ITER JEPA proposals,
	 * using PAL-biased action ordering so the best prior comes first */
	{
		uint32_t preferred = anx_loop_select_action_by_prior(
			s->world_uri, (uint32_t)ANX_JEPA_ACT_COUNT);
		uint32_t offset = 0;

		for (i = 0; i < ANX_EBM_PROPOSALS_PER_ITER; i++) {
			uint32_t action;

			/* First slot: PAL-preferred action; rest: round-robin */
			if (i == 0)
				action = preferred;
			else {
				action = offset % (uint32_t)ANX_JEPA_ACT_COUNT;
				if (action == preferred)
					action = (action + 1) %
						 (uint32_t)ANX_JEPA_ACT_COUNT;
				offset++;
			}

			anx_memset(&pred_oid, 0, sizeof(pred_oid));
			(void)anx_jepa_predict(&s->active_belief, action,
					       &pred_oid);

			rc = anx_loop_proposal_create_jepa(
				session_oid, s->iteration,
				pred_oid, action,
				&prop_oids[n_props]);
			if (rc == ANX_OK) {
				act_ids[n_props] = action;
				n_props++;
			}
		}
	}

	/* Step 2b: add one LLM proposal alongside the JEPA candidates */
	if (n_props < ANX_EBM_PROPOSALS_PER_ITER + 1) {
		anx_oid_t llm_prop = {0};
		uint32_t  llm_act  = 0;

		if (anx_loop_llm_propose(session_oid, s->iteration, &llm_prop)
		    == ANX_OK) {
			if (anx_loop_proposal_get_action_id(llm_prop, &llm_act)
			    != ANX_OK)
				llm_act = 0;
			prop_oids[n_props] = llm_prop;
			act_ids[n_props]   = llm_act;
			n_props++;
		}
	}

	/* Step 3: score each proposal */
	for (i = 0; i < n_props; i++) {
		rc = anx_ebm_score_proposal(session_oid, prop_oids[i],
					    &energies[i]);
		if (rc != ANX_OK)
			energies[i] = 0.5f;

		if (energies[i] < best_energy) {
			best_energy = energies[i];
			best_prop   = prop_oids[i];
			best_act_id = act_ids[i];
		}

		/* Accumulate per-action stats */
		if (stats && act_ids[i] < stats_count) {
			struct anx_loop_session_action_stats *a =
				&stats[act_ids[i]];

			a->total_proposals++;
			a->energy_sum += energies[i];
			if (energies[i] < a->min_energy)
				a->min_energy = energies[i];
		}
	}

	/* Mark winner in per-action stats */
	if (stats && best_act_id < stats_count)
		stats[best_act_id].win_count++;

	/* Step 4: record best energy and pick arbitration decision */
	(void)anx_loop_session_record_score(session_oid, best_energy, best_prop);

	/* Re-read status — record_score may have triggered convergence halt */
	s = anx_loop_session_get(session_oid);
	if (!s || s->status != ANX_LOOP_RUNNING)
		return ANX_OK;

	if (best_energy <= ANX_EBM_CONVERGE_THRESH) {
		decision = ANX_LOOP_ARB_HALT;
	} else if (best_energy >= ANX_EBM_REJECT_THRESH &&
		   s->branch_budget > 0) {
		decision = ANX_LOOP_ARB_BRANCH;
	} else {
		decision = ANX_LOOP_ARB_CONTINUE;
	}

	(void)anx_loop_session_apply_decision(session_oid, decision);

	{
		unsigned int ei = (unsigned int)best_energy;
		unsigned int ef = (unsigned int)((best_energy - (float)ei)
					         * 10000.0f + 0.5f);

		kprintf("[ebm] iter %u: best_energy=%u.%04u act=%u decision=%d\n",
			s->iteration, ei, ef, best_act_id, (int)decision);
	}

	return ANX_OK;
}
