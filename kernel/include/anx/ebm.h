/*
 * anx/ebm.h — Energy-Based Model scoring subsystem (RFC-0020 Phase 3).
 *
 * Four energy functions evaluate each world proposal: world consistency
 * (JEPA divergence), goal alignment (keyword matching), constraint
 * compliance (budget ratio), and epistemic uncertainty (belief state).
 *
 * anx_ebm_run_iteration() is the primary entry point — it drives one
 * full IBAL iteration: observe → propose → score → arbitrate.
 */

#ifndef ANX_EBM_H
#define ANX_EBM_H

#include <anx/types.h>
#include <anx/loop.h>

/* Energy above which a proposal is classified REJECT and counterexample stored */
#define ANX_EBM_REJECT_THRESH    0.70f
/* Energy below which the arbitration cell halts (converged) */
#define ANX_EBM_CONVERGE_THRESH  0.20f
/* Number of JEPA proposals generated per iteration */
#define ANX_EBM_PROPOSALS_PER_ITER  4

int anx_ebm_init(void);

/*
 * Score one world proposal against all registered energy functions.
 * Creates one ANX_OBJ_SCORE per scorer.  Writes the weighted aggregate
 * energy to *energy_out (0.0 = optimal, 1.0 = worst).  Proposals with
 * aggregate >= ANX_EBM_REJECT_THRESH are recorded as counterexamples.
 */
int anx_ebm_score_proposal(anx_oid_t session_oid, anx_oid_t proposal_oid,
			    float *energy_out);

/*
 * Run one full EBM iteration for the given session:
 *   1. Create an ANX_OBJ_BELIEF_STATE (JEPA observe + encode)
 *   2. Generate ANX_EBM_PROPOSALS_PER_ITER JEPA world proposals
 *   3. Score each with anx_ebm_score_proposal()
 *   4. Record best energy, apply arbitration decision to session status
 *   5. Accumulate per-action win/energy stats into stats[] (if non-NULL)
 *
 * Non-fatal when JEPA is unavailable: proceeds with neutral energies.
 * The session must be in ANX_LOOP_RUNNING status.
 */
int anx_ebm_run_iteration(anx_oid_t session_oid,
			   struct anx_loop_session_action_stats *stats,
			   uint32_t stats_count);

#endif /* ANX_EBM_H */
