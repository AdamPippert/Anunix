/*
 * anx/loop.h — Iterative Belief-Action Loop (IBAL) session primitive (RFC-0020).
 *
 * Defines the loop session object and public API for the kernel loop
 * subsystem.  Phase 1: session CRUD, iteration advance, halt.
 * EBM and JEPA integration wired in Phase 2+.
 */

#ifndef ANX_LOOP_H
#define ANX_LOOP_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define ANX_LOOP_MAX_SESSIONS      32
#define ANX_LOOP_MAX_ITERATIONS   256
#define ANX_LOOP_MAX_CANDIDATES    16
#define ANX_LOOP_MAX_SCORE_HIST    64
#define ANX_LOOP_MAX_BRANCHES       8   /* max parallel branch sessions */

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */

enum anx_loop_halt_policy {
	ANX_LOOP_HALT_ON_CONVERGENCE,	/* halt when |energy_delta| < threshold */
	ANX_LOOP_HALT_ON_BUDGET,	/* halt when iteration >= max_iterations */
	ANX_LOOP_HALT_ON_CONFIDENCE,	/* halt when best candidate confidence >= min */
	ANX_LOOP_HALT_MANUAL,		/* only via explicit loop halt command */
	ANX_LOOP_HALT_POLICY_COUNT,
};

enum anx_loop_session_status {
	ANX_LOOP_PENDING,
	ANX_LOOP_RUNNING,
	ANX_LOOP_HALTED,
	ANX_LOOP_COMMITTED,
	ANX_LOOP_ABORTED,
	ANX_LOOP_STATUS_COUNT,
};

/* Decision produced by the arbitration cell each iteration */
enum anx_loop_arb_decision {
	ANX_LOOP_ARB_CONTINUE,
	ANX_LOOP_ARB_HALT,
	ANX_LOOP_ARB_BRANCH,	/* Phase 4: parallel hypothesis branches */
	ANX_LOOP_ARB_ABORT,
};

/* ------------------------------------------------------------------ */
/* Per-iteration score record (stored in session history ring)        */
/* ------------------------------------------------------------------ */

struct anx_loop_iter_score {
	uint32_t  iteration;
	float     best_energy;		/* lowest energy seen this iteration */
	float     delta;		/* change from previous iteration */
	uint32_t  candidate_count;
	anx_oid_t best_candidate;
};

/* ------------------------------------------------------------------ */
/* Runtime session object                                              */
/* ------------------------------------------------------------------ */

struct anx_loop_session {
	bool      in_use;

	/* Identity */
	anx_oid_t session_oid;
	anx_oid_t parent_task_oid;

	/* Configuration (immutable after create) */
	char                       world_uri[128];
	char                       goal_text[512];	/* natural-language goal (Phase 3) */
	uint32_t                   max_iterations;
	enum anx_loop_halt_policy  halt_policy;
	float                      halt_threshold;
	float                      confidence_min;
	uint32_t                   branch_budget;
	anx_oid_t                  capability_scope;

	/* Dynamic state */
	enum anx_loop_session_status  status;
	uint32_t                      iteration;

	/* Live object references */
	anx_oid_t  active_belief;
	anx_oid_t  best_candidate;

	/* Per-iteration candidate list (reset on advance) */
	anx_oid_t  candidates[ANX_LOOP_MAX_CANDIDATES];
	uint32_t   candidate_count;

	/* Score history ring */
	struct anx_loop_iter_score  score_history[ANX_LOOP_MAX_SCORE_HIST];
	uint32_t                    score_hist_count;

	/* Timestamps (nanoseconds since boot) */
	uint64_t   started_at_ns;
	uint64_t   halted_at_ns;

	/* Phase 4: branch/merge tracking */
	anx_oid_t  branch_from_oid;			/* parent OID; zero for root */
	uint32_t   branch_depth;			/* 0=root, 1=branch */
	uint32_t   branch_id;				/* index within parent's list */
	anx_oid_t  branch_child_oids[ANX_LOOP_MAX_BRANCHES];
	uint32_t   branch_child_count;
};

/* ------------------------------------------------------------------ */
/* Object payload structs (RFC-0020 Phase 2)                          */
/*                                                                     */
/* These are the in-object-store representations of IBAL objects.     */
/* Each is stored as a sealed ANX_OBJ_* payload via anx_so_create().  */
/* ------------------------------------------------------------------ */

/* Source of a world proposal */
enum anx_loop_proposal_source {
	ANX_LOOP_PROPOSAL_JEPA,		/* anx_jepa_predict() */
	ANX_LOOP_PROPOSAL_LLM,		/* LLM inference (Phase 3) */
	ANX_LOOP_PROPOSAL_RETRIEVAL,	/* semantic search (Phase 3) */
	ANX_LOOP_PROPOSAL_SYMBOLIC,	/* graph/rule transition (Phase 3) */
};

/* Proposal lifecycle status */
enum anx_loop_proposal_status {
	ANX_LOOP_PROPOSAL_CANDIDATE,
	ANX_LOOP_PROPOSAL_SELECTED,
	ANX_LOOP_PROPOSAL_REJECTED,
	ANX_LOOP_PROPOSAL_COMMITTED,
};

/* EBM threshold classification (matches anx_ebm_threshold_class values) */
enum anx_loop_score_class {
	ANX_LOOP_SCORE_ACCEPT,
	ANX_LOOP_SCORE_MARGINAL,
	ANX_LOOP_SCORE_REJECT,
};

#define ANX_LOOP_BELIEF_MAX_CONTEXT	32
#define ANX_LOOP_PROPOSAL_MAX_SCORES	8

/*
 * Payload stored as ANX_OBJ_BELIEF_STATE.
 * Represents the system's belief about the world at a single iteration.
 */
struct anx_loop_belief_payload {
	anx_oid_t  session_oid;
	uint32_t   iteration;
	anx_oid_t  latent_oid;		/* ANX_OBJ_JEPA_LATENT encoded this iter */
	anx_oid_t  obs_oid;		/* ANX_OBJ_JEPA_OBS observation used */
	anx_oid_t  parent_belief_oid;	/* prior iter's belief (null if iter 0) */
	float      uncertainty;		/* [0,1] epistemic uncertainty estimate */
	anx_oid_t  context_oids[ANX_LOOP_BELIEF_MAX_CONTEXT];
	uint32_t   context_count;
};

/*
 * Payload stored as ANX_OBJ_WORLD_PROPOSAL.
 * Represents one candidate world hypothesis at a single iteration.
 */
struct anx_loop_proposal_payload {
	anx_oid_t                      session_oid;
	uint32_t                       iteration;
	enum anx_loop_proposal_source  source;
	anx_oid_t                      latent_oid;	/* predicted ANX_OBJ_JEPA_LATENT */
	uint32_t                       action_id;	/* JEPA action (if source==JEPA) */
	float                          aggregate_score;	/* filled after EBM scoring */
	enum anx_loop_proposal_status  status;
	anx_oid_t                      score_oids[ANX_LOOP_PROPOSAL_MAX_SCORES];
	uint32_t                       score_count;
};

/*
 * Payload stored as ANX_OBJ_SCORE.
 * One energy function's evaluation of one proposal.
 */
struct anx_loop_score_payload {
	anx_oid_t  session_oid;
	anx_oid_t  target_oid;			/* the proposal being scored */
	char       scorer_id[64];
	float      scalar;			/* aggregate (lower = better fit) */
	float      components[8];
	uint32_t   component_count;
	enum anx_loop_score_class  threshold_class;
	float      confidence;
};

/*
 * Payload stored as ANX_OBJ_COUNTEREXAMPLE.
 * Rejected hypothesis stored as negative knowledge for future training.
 */
struct anx_loop_counterexample_payload {
	anx_oid_t  session_oid;
	anx_oid_t  rejected_oid;		/* the rejected proposal */
	uint32_t   reason;			/* ANX_LOOP_REJECT_* */
	float      rejection_score;
	char       context_summary[512];
};

/* Rejection reason codes for counterexamples */
#define ANX_LOOP_REJECT_ENERGY_TOO_HIGH		0
#define ANX_LOOP_REJECT_CONSTRAINT_VIOLATED	1
#define ANX_LOOP_REJECT_CAPABILITY_DENIED	2
#define ANX_LOOP_REJECT_DIVERGENCE_TOO_HIGH	3
#define ANX_LOOP_REJECT_EXECUTION_FAILED	4
#define ANX_LOOP_REJECT_HUMAN_REJECTED		5

/* ------------------------------------------------------------------ */
/* Belief state API (Phase 2: loop_belief.c)                          */
/* ------------------------------------------------------------------ */

/*
 * Collect current system state, encode via JEPA, and store as a new
 * ANX_OBJ_BELIEF_STATE object linked to the session.  On success,
 * updates session->active_belief and writes *belief_oid_out.
 */
int anx_loop_belief_create(anx_oid_t session_oid, uint32_t iteration,
			   anx_oid_t parent_belief_oid,
			   anx_oid_t *belief_oid_out);

/* Read the JEPA latent OID from a stored belief state. */
int anx_loop_belief_get_latent(anx_oid_t belief_oid,
			       anx_oid_t *latent_oid_out);

/* Read the epistemic uncertainty [0,1] from a stored belief state. */
int anx_loop_belief_get_uncertainty(anx_oid_t belief_oid,
				    float *uncertainty_out);

/* ------------------------------------------------------------------ */
/* World proposal API (Phase 2: loop_proposal.c)                      */
/* ------------------------------------------------------------------ */

/*
 * Store a JEPA-predicted latent as an ANX_OBJ_WORLD_PROPOSAL and add
 * it to the session's candidate list.
 */
int anx_loop_proposal_create_jepa(anx_oid_t session_oid, uint32_t iteration,
				  anx_oid_t predicted_latent_oid,
				  uint32_t action_id,
				  anx_oid_t *proposal_oid_out);

/* Read the JEPA latent OID from a stored proposal. */
int anx_loop_proposal_get_latent(anx_oid_t proposal_oid,
				 anx_oid_t *latent_oid_out);

/* Read the action_id from a stored proposal. */
int anx_loop_proposal_get_action_id(anx_oid_t proposal_oid,
				    uint32_t *action_id_out);

/* Update the aggregate energy score on a proposal (called after EBM). */
int anx_loop_proposal_set_score(anx_oid_t proposal_oid, float aggregate);

/* ------------------------------------------------------------------ */
/* Score object API (Phase 2: loop_score.c)                           */
/* ------------------------------------------------------------------ */

int anx_loop_score_create(anx_oid_t session_oid, anx_oid_t target_oid,
			  const char *scorer_id, float scalar,
			  enum anx_loop_score_class threshold_class,
			  float confidence, anx_oid_t *score_oid_out);

/* Append a score OID to a proposal's score list. */
int anx_loop_proposal_add_score(anx_oid_t proposal_oid,
				anx_oid_t score_oid);

/* ------------------------------------------------------------------ */
/* Counterexample recording (Phase 2: loop_proposal.c)                */
/* ------------------------------------------------------------------ */

int anx_loop_counterexample_record(anx_oid_t session_oid,
				   anx_oid_t rejected_proposal_oid,
				   uint32_t reason, float rejection_score,
				   const char *context_summary);

/* ------------------------------------------------------------------ */
/* Session creation parameters                                         */
/* ------------------------------------------------------------------ */

struct anx_loop_create_params {
	char                       world_uri[128];
	char                       goal_text[512];	/* natural-language goal (Phase 3) */
	uint32_t                   max_iterations;	/* 0 → ANX_LOOP_MAX_ITERATIONS */
	enum anx_loop_halt_policy  halt_policy;
	float                      halt_threshold;	/* for HALT_ON_CONVERGENCE */
	float                      confidence_min;	/* for HALT_ON_CONFIDENCE */
	uint32_t                   branch_budget;	/* 0 → 1 */
	anx_oid_t                  capability_scope;
	anx_oid_t                  parent_task_oid;
};

/* ------------------------------------------------------------------ */
/* Session info (introspection)                                        */
/* ------------------------------------------------------------------ */

struct anx_loop_session_info {
	anx_oid_t                     session_oid;
	enum anx_loop_session_status  status;
	uint32_t                      iteration;
	uint32_t                      max_iterations;
	float                         last_best_energy;
	float                         last_delta;
	anx_oid_t                     active_belief;
	anx_oid_t                     best_candidate;
	anx_oid_t                     capability_scope;
	char                          goal_text[512];	/* Phase 3 */
	uint64_t                      started_at_ns;
	uint64_t                      halted_at_ns;
	uint32_t   branch_depth;
	uint32_t   branch_child_count;
};

/* ------------------------------------------------------------------ */
/* Goal alignment energy (Phase 3: loop_goal.c)                       */
/* ------------------------------------------------------------------ */

/*
 * Compute how well action_id aligns with goal_text using keyword-to-
 * action-category matching.  Returns an energy in [0, 1] where 0.0
 * means the action is ideal for the stated goal and 1.0 means it is
 * entirely misaligned.  Returns 0.5 when goal_text is empty (neutral).
 */
float anx_loop_goal_alignment_energy(const char *goal_text, uint32_t action_id);

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Subsystem lifecycle */
int anx_loop_init(void);

/* Session CRUD */
int anx_loop_session_create(const struct anx_loop_create_params *params,
			     anx_oid_t *session_oid_out);
int anx_loop_session_advance(anx_oid_t session_oid);
int anx_loop_session_halt(anx_oid_t session_oid);
int anx_loop_session_abort(anx_oid_t session_oid);
int anx_loop_session_commit(anx_oid_t session_oid, anx_oid_t plan_oid);

/* State updates (called by loop cells) */
int anx_loop_session_set_belief(anx_oid_t session_oid, anx_oid_t belief_oid);
int anx_loop_session_add_candidate(anx_oid_t session_oid,
				   anx_oid_t candidate_oid);
int anx_loop_session_record_score(anx_oid_t session_oid, float best_energy,
				  anx_oid_t best_candidate_oid);
int anx_loop_session_apply_decision(anx_oid_t session_oid,
				    enum anx_loop_arb_decision decision);

/* Introspection */
int anx_loop_session_status_get(anx_oid_t session_oid,
				struct anx_loop_session_info *info_out);
int anx_loop_session_candidates(anx_oid_t session_oid,
				anx_oid_t *oids_out, uint32_t max_oids,
				uint32_t *found_out);
int anx_loop_session_score_history(anx_oid_t session_oid,
				   struct anx_loop_iter_score *out,
				   uint32_t max_entries,
				   uint32_t *found_out);

/* Internal accessor used by loop_*.c subsystems */
struct anx_loop_session *anx_loop_session_get(anx_oid_t session_oid);

/* Shell command entry point */
int anx_loop_shell_dispatch(int argc, const char *const *argv);

/* ------------------------------------------------------------------ */
/* Branch/merge scheduler (Phase 4: loop_branch.c)                    */
/* ------------------------------------------------------------------ */

/*
 * Create a child branch session that inherits parent's goal + world.
 * branch_id is a caller-assigned index [0, ANX_LOOP_MAX_BRANCHES).
 * branch_max_iterations sets the child's iteration budget.
 */
int anx_loop_branch_create(anx_oid_t parent_oid, uint32_t branch_id,
			   uint32_t branch_max_iterations,
			   anx_oid_t *child_oid_out);

/*
 * If child found a lower-energy candidate than parent's current best,
 * adopt it into the parent session.
 */
int anx_loop_branch_merge(anx_oid_t parent_oid, anx_oid_t child_oid);

/* List the OIDs of all child branches registered for parent. */
int anx_loop_branch_list(anx_oid_t parent_oid,
			 anx_oid_t *oids_out, uint32_t max_count,
			 uint32_t *count_out);

#endif /* ANX_LOOP_H */
