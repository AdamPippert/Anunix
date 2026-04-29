/*
 * anx/rlm.h — Reasoning Language Model Harness.
 *
 * Drives multi-step model rollouts on top of Execution Cells, plans,
 * traces, and the scheduler. A rollout is a parent cell whose plan
 * advances one inference step at a time; each step produces a
 * response State Object and appends to the trace. Tool callouts
 * spawn child cells so lineage and provenance are preserved.
 *
 * The inference call is routed through a pluggable adapter so the
 * same harness drives local model servers, remote Claude API calls,
 * or test doubles without modification.
 */

#ifndef ANX_RLM_H
#define ANX_RLM_H

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/sched.h>

#define ANX_RLM_MAX_STEPS	32
#define ANX_RLM_MAX_BATCH	32
#define ANX_RLM_MAX_CONTENT	4096
#define ANX_RLM_MODEL_NAME	64
#define ANX_RLM_LABEL_LEN	64

/* --- Rollout status --- */

enum anx_rlm_status {
	ANX_RLM_PENDING,
	ANX_RLM_RUNNING,
	ANX_RLM_COMPLETED,
	ANX_RLM_FAILED,
	ANX_RLM_CANCELLED,
};

/* --- Step kinds within a rollout --- */

enum anx_rlm_step_kind {
	ANX_RLM_STEP_INFER,		/* model inference */
	ANX_RLM_STEP_TOOL,		/* tool result injection */
};

/* --- Step record (bounded, no dynamic alloc) --- */

struct anx_rlm_step {
	enum anx_rlm_step_kind kind;
	anx_cid_t cell_id;		/* child cell that produced the step */
	anx_oid_t input_oid;		/* message fed in */
	anx_oid_t output_oid;		/* response/result object */
	int32_t input_tokens;
	int32_t output_tokens;
	bool stop;			/* true => end_turn */
	int status;
	char label[ANX_RLM_LABEL_LEN];
};

/* --- Configuration --- */

struct anx_rlm_config {
	char model[ANX_RLM_MODEL_NAME];
	char system[ANX_RLM_MAX_CONTENT];
	uint32_t max_steps;		/* hard cap on inference iterations */
	uint32_t max_tokens;		/* per call (step 0 baseline) */
	uint32_t min_tokens;		/* lower bound after lambda decay */
	uint16_t lambda_decay;		/* lambda-RLM token decay, Q8: 256=1.0, e.g. 210≈0.82 */
	bool persist_trace;		/* finalize trace as State Object */
	bool admit_responses;		/* admit responses to memplane L1 */
	enum anx_sched_priority priority;
	enum anx_queue_class queue;
};

/* --- Inference adapter --- */

struct anx_rlm_infer_req {
	const char *model;
	const char *system;
	const char *user_message;
	uint32_t max_tokens;
};

struct anx_rlm_infer_resp {
	char content[ANX_RLM_MAX_CONTENT];
	uint32_t content_len;
	int32_t input_tokens;
	int32_t output_tokens;
	bool stop;			/* set when stop_reason == "end_turn" */
	int status;
};

typedef int (*anx_rlm_infer_fn)(const struct anx_rlm_infer_req *req,
				struct anx_rlm_infer_resp *resp);

/* Install an inference adapter. NULL restores the default (model_client). */
void anx_rlm_set_infer(anx_rlm_infer_fn fn);

/* --- The rollout --- */

struct anx_rlm_rollout {
	anx_cid_t root_cid;		/* parent execution cell */
	anx_oid_t prompt_oid;		/* user prompt State Object */
	anx_oid_t response_oid;		/* final assistant response */
	anx_oid_t trace_oid;		/* materialized trace (if persisted) */

	struct anx_rlm_step steps[ANX_RLM_MAX_STEPS];
	uint32_t step_count;

	struct anx_rlm_config config;
	enum anx_rlm_status status;

	int32_t score;			/* trajectory score (caller-set) */
	int32_t total_input_tokens;
	int32_t total_output_tokens;

	anx_time_t started_at;
	anx_time_t completed_at;
	int error_code;
};

/* Global init (idempotent). */
void anx_rlm_init(void);

/* Fill config with sensible defaults. */
void anx_rlm_config_default(struct anx_rlm_config *cfg);

/*
 * Create a rollout bound to a prompt State Object. The prompt must
 * already exist in the object store and be readable.
 */
int anx_rlm_rollout_create(const anx_oid_t *prompt_oid,
			   const struct anx_rlm_config *config,
			   struct anx_rlm_rollout **out);

/*
 * Advance one inference step. Builds a request from the prompt plus
 * accumulated responses, calls the adapter, records a step, and
 * marks the rollout COMPLETED if the adapter signals stop.
 */
int anx_rlm_rollout_step(struct anx_rlm_rollout *r);

/* Loop step() until completed, failed, or max_steps reached. */
int anx_rlm_rollout_run(struct anx_rlm_rollout *r);

/*
 * Inject an externally-produced tool result into the trajectory.
 * The caller is responsible for creating tool_result_oid (typically
 * in a child cell). The result becomes the user_message for the
 * next inference step.
 */
int anx_rlm_rollout_inject(struct anx_rlm_rollout *r,
			   const char *label,
			   const anx_oid_t *tool_result_oid);

/* Set trajectory score (used by external preference/reward models). */
void anx_rlm_rollout_set_score(struct anx_rlm_rollout *r, int32_t score);

/* Release all resources associated with a rollout. */
void anx_rlm_rollout_destroy(struct anx_rlm_rollout *r);

/* --- Batch runner (parallel rollouts via the scheduler) --- */

struct anx_rlm_batch {
	struct anx_rlm_rollout *rollouts[ANX_RLM_MAX_BATCH];
	uint32_t count;
	uint32_t completed;
	uint32_t failed;
	anx_time_t started_at;
	anx_time_t completed_at;
};

/*
 * Create a batch of rollouts from an array of prompt OIDs. Each
 * rollout uses the same config. Rollouts are created but not yet
 * advanced.
 */
int anx_rlm_batch_create(const anx_oid_t *prompt_oids, uint32_t count,
			 const struct anx_rlm_config *config,
			 struct anx_rlm_batch **out);

/*
 * Enqueue every rollout's root cell and drain the queue by
 * advancing rollouts round-robin until all are terminal. Returns
 * ANX_OK when every rollout reached COMPLETED; ANX_EIO if any
 * failed (individual failures are recorded per-rollout).
 */
int anx_rlm_batch_run(struct anx_rlm_batch *batch);

void anx_rlm_batch_destroy(struct anx_rlm_batch *batch);

/* Aggregate throughput since started_at. Returns 0 if batch not yet run. */
uint32_t anx_rlm_batch_tokens_per_second(const struct anx_rlm_batch *batch);

/* --- PAL feedback bridge (Phase 16) --- */

/*
 * Feed a completed rollout's score into the PAL cross-session accumulator.
 * action_id (0..ANX_MEMORY_ACT_COUNT-1) selects which action slot to update.
 * Low-scoring rollouts (score < 30) also emit a counterexample State Object
 * so the CEXL critic can refine per-action priors in the next IBAL session.
 * Returns ANX_EPERM if the rollout is not COMPLETED.
 */
int anx_rlm_pal_feedback(struct anx_rlm_rollout *r,
			  const char *world_uri,
			  uint32_t action_id);

#endif /* ANX_RLM_H */
