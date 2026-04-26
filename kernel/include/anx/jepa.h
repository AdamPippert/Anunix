/*
 * anx/jepa.h — JEPA Latent-State Subsystem.
 *
 * A native C implementation of a Joint Embedding Predictive Architecture
 * (JEPA) world model for Anunix.  The subsystem learns compressed
 * representations of runtime system state and predicts future states in
 * latent space — never in observation space — to inform routing, memory,
 * and validation decisions.
 *
 * Symbolic governance (Capabilities, Cells, State Objects, policy gates)
 * remains authoritative.  JEPA contributes a bounded learned signal.
 *
 * The subsystem is optional: if no tensor-capable hardware is present,
 * anx_jepa_available() returns false and all hooks are no-ops.
 *
 * World profiles allow the observation schema, action vocabulary, and
 * model architecture to be tailored per deployment target.  The built-in
 * "anx:world/os-default" profile covers AI researcher and OS developer
 * workloads.  Stubs for cellular, robotics, and enterprise-IT targets
 * are registered at init time.
 */

#ifndef ANX_JEPA_H
#define ANX_JEPA_H

#include <anx/types.h>
#include <anx/spinlock.h>
#include <anx/list.h>
#include <anx/state_object.h>
#include <anx/engine.h>
#include <anx/route.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define ANX_JEPA_LATENT_DIM_MAX		512	/* hard cap on embedding size */
#define ANX_JEPA_LATENT_DIM_DEFAULT	256	/* os-default arch */
#define ANX_JEPA_MAX_DATASETS		16	/* training datasets per world */
#define ANX_JEPA_MAX_WORLDS		8	/* registered world profiles */
#define ANX_JEPA_WORLD_URI_MAX		128
#define ANX_JEPA_WORLD_NAME_MAX		64
#define ANX_JEPA_WORLD_DESC_MAX		256
#define ANX_JEPA_FIELD_NAME_MAX		32
#define ANX_JEPA_MAX_OBS_FIELDS		64	/* observation schema entries */
#define ANX_JEPA_MAX_ACTIONS		32	/* action vocabulary entries */
#define ANX_JEPA_ROUTE_DELTA_MAX	20	/* max ±score added to route planner */
#define ANX_JEPA_MEM_UTILITY_MAX	100	/* 0-100 utility hint scale */
#define ANX_JEPA_CONTEXT_BUF_MAX	1024	/* RLM context injection buffer */

/* Observation dimensions for the os-default profile.
 * Build-time asserts in jepa.c verify these match the scheduler and
 * memory-plane constants they mirror. */
#define ANX_JEPA_OBS_SCHED_CLASSES	6	/* mirrors ANX_QUEUE_CLASS_COUNT */
#define ANX_JEPA_OBS_MEM_TIERS		6	/* mirrors ANX_MEM_TIER_COUNT */

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */

/* Subsystem lifecycle state. */
enum anx_jepa_status {
	ANX_JEPA_UNINITIALIZED,
	ANX_JEPA_INITIALIZING,
	ANX_JEPA_READY,		/* encoder loaded, can encode + predict */
	ANX_JEPA_TRAINING,	/* training loop active */
	ANX_JEPA_DEGRADED,	/* CPU fallback only (no NPU/GPU) */
	ANX_JEPA_UNAVAILABLE,	/* no tensor capability at all */
};

/* Operating mode. */
enum anx_jepa_mode {
	ANX_JEPA_MODE_INFERENCE,	/* encode + predict, no trace recording */
	ANX_JEPA_MODE_ONLINE,		/* inference + live trace accumulation */
	ANX_JEPA_MODE_TRAINING,		/* full training loop active */
};

/*
 * Core action vocabulary for the os-default world profile.
 * World profiles may define their own action sets; this enum covers
 * the actions available to any Anunix OS agent.
 */
enum anx_jepa_action {
	ANX_JEPA_ACT_IDLE = 0,
	ANX_JEPA_ACT_ROUTE_LOCAL,
	ANX_JEPA_ACT_ROUTE_REMOTE,
	ANX_JEPA_ACT_ROUTE_FALLBACK,
	ANX_JEPA_ACT_MEM_PROMOTE,
	ANX_JEPA_ACT_MEM_DEMOTE,
	ANX_JEPA_ACT_MEM_FORGET,
	ANX_JEPA_ACT_CELL_SPAWN,
	ANX_JEPA_ACT_CELL_CANCEL,
	ANX_JEPA_ACT_CAP_VALIDATE,
	ANX_JEPA_ACT_CAP_SUSPEND,
	ANX_JEPA_ACT_SECURITY_ALERT,
	ANX_JEPA_ACT_COUNT,
};

/* ------------------------------------------------------------------ */
/* Observation struct — os-default world profile                       */
/* ------------------------------------------------------------------ */

/*
 * Snapshot of Anunix runtime state collected from kernel subsystems.
 * This is the observation type for "anx:world/os-default".  Other world
 * profiles define their own observation structs; the collect_obs callback
 * in anx_jepa_world_profile casts obs_buf to the appropriate type.
 */
struct anx_jepa_obs {
	uint64_t timestamp_ns;

	/* Scheduler (one depth counter per queue class) */
	uint32_t sched_queue_depths[ANX_JEPA_OBS_SCHED_CLASSES];
	uint32_t active_cell_count;

	/* Memory control plane (one entry per tier) */
	uint32_t mem_decay_score_avg[ANX_JEPA_OBS_MEM_TIERS];
	uint32_t mem_entry_counts[ANX_JEPA_OBS_MEM_TIERS];

	/* Routing plane */
	uint32_t route_fallback_count;	/* since last observation */
	float    route_avg_score;	/* average planner score (0-100) */

	/* Tensor compute utilization (0.0-1.0) */
	float    tensor_cpu_util;
	float    tensor_npu_util;	/* 0 if no NPU present */

	/* Capability validation */
	float    cap_validation_avg;	/* average validation score (0-100) */
	uint32_t cap_failures;		/* since last observation */

	/* Error and security counters since last observation */
	uint32_t error_count;
	uint32_t security_event_count;
};

/* ------------------------------------------------------------------ */
/* State Object payloads                                               */
/* ------------------------------------------------------------------ */

/*
 * Payload for ANX_OBJ_JEPA_LATENT.
 * The first `dim` elements of vec[] are valid; the rest are unused.
 */
struct anx_jepa_latent_payload {
	uint32_t  dim;			/* actual embedding dimension */
	uint64_t  encoded_at_ns;
	anx_oid_t source_obs_oid;	/* ANX_OBJ_JEPA_OBS that produced this */
	float     vec[ANX_JEPA_LATENT_DIM_MAX];
};

/*
 * Payload for ANX_OBJ_JEPA_TRACE.
 * One training sample: (observation at t, action, observation at t+1,
 * context latent, predicted latent, target latent, VICReg loss).
 */
struct anx_jepa_trace_payload {
	anx_oid_t obs_t_oid;		/* ANX_OBJ_JEPA_OBS at time t */
	anx_oid_t obs_tp1_oid;		/* ANX_OBJ_JEPA_OBS at time t+1 */
	anx_oid_t latent_t_oid;		/* z_t: context encoder output */
	anx_oid_t latent_pred_oid;	/* ẑ_{t+1}: predictor output */
	anx_oid_t latent_tp1_oid;	/* z_{t+1}: target encoder output */
	uint32_t  action_id;
	float     loss;			/* VICReg loss for this step */
	uint64_t  timestamp_ns;
};

/* ------------------------------------------------------------------ */
/* Architecture and training configuration                             */
/* ------------------------------------------------------------------ */

struct anx_jepa_arch_config {
	uint32_t obs_dim;		/* flattened observation vector length */
	uint32_t latent_dim;		/* embedding dimension */
	uint32_t encoder_layers;
	uint32_t encoder_heads;
	uint32_t predictor_layers;
	uint32_t predictor_heads;
	uint32_t action_count;
	uint32_t action_embed_dim;
};

struct anx_jepa_train_config {
	float    lr;			/* learning rate */
	float    ema_decay;		/* target encoder EMA decay (e.g. 0.996) */
	float    vicreg_lambda_inv;	/* invariance term weight */
	float    vicreg_lambda_var;	/* variance term weight */
	float    vicreg_lambda_cov;	/* covariance term weight */
	uint32_t batch_size;
	uint32_t warmup_steps;
};

/* ------------------------------------------------------------------ */
/* World profile                                                       */
/* ------------------------------------------------------------------ */

/*
 * A world profile defines the observation schema, action vocabulary,
 * model architecture, and training configuration for a specific
 * deployment target.  Profiles are registered at init time; custom
 * profiles can be registered by platform code before anx_jepa_init().
 *
 * collect_obs: called by anx_jepa_observe() when this profile is active.
 * obs_buf receives a world-specific observation struct (e.g.
 * anx_jepa_obs for os-default); obs_buf_size is the buffer capacity.
 * Returns ANX_OK on success, negative on error.
 *
 * Encoder and predictor weights for this world are stored as
 * ANX_OBJ_TENSOR State Objects referenced by active_checkpoint.
 */
struct anx_jepa_world_profile {
	char uri[ANX_JEPA_WORLD_URI_MAX];
	char display_name[ANX_JEPA_WORLD_NAME_MAX];
	char description[ANX_JEPA_WORLD_DESC_MAX];

	struct anx_jepa_arch_config  arch;
	struct anx_jepa_train_config train;

	/* Observation schema (for introspection and tooling) */
	uint32_t obs_field_count;
	char     obs_field_names[ANX_JEPA_MAX_OBS_FIELDS][ANX_JEPA_FIELD_NAME_MAX];

	/* Action vocabulary */
	uint32_t action_count;
	char     action_names[ANX_JEPA_MAX_ACTIONS][ANX_JEPA_FIELD_NAME_MAX];

	/* World-specific observation collector (NULL = world not yet implemented) */
	int (*collect_obs)(void *obs_buf, uint32_t obs_buf_size);

	/* Registered training datasets (ANX_OBJ_STRUCTURED_DATA OIDs) */
	anx_oid_t datasets[ANX_JEPA_MAX_DATASETS];
	uint32_t  dataset_count;

	/* Active trained checkpoint (ANX_OBJ_TENSOR, null OID if untrained) */
	anx_oid_t active_checkpoint;
	bool      checkpoint_loaded;

	struct anx_spinlock   lock;
	struct anx_list_head  registry_link;
};

/* ------------------------------------------------------------------ */
/* Core subsystem context (opaque to callers)                          */
/* ------------------------------------------------------------------ */

struct anx_jepa_ctx {
	enum anx_jepa_status status;
	enum anx_jepa_mode   mode;

	struct anx_engine *engine;		/* LOCAL_MODEL engine handle */

	/* Weight OIDs — ANX_OBJ_TENSOR, null until trained or loaded */
	anx_oid_t encoder_weights_oid;
	anx_oid_t target_weights_oid;		/* EMA copy of encoder */
	anx_oid_t predictor_weights_oid;

	struct anx_jepa_world_profile *active_world;

	/* Runtime statistics */
	uint64_t encode_count;
	uint64_t predict_count;
	uint64_t train_steps;
	float    last_loss;
	float    last_divergence;

	struct anx_spinlock lock;
};

/* ------------------------------------------------------------------ */
/* Subsystem lifecycle                                                 */
/* ------------------------------------------------------------------ */

/*
 * Initialize the JEPA subsystem.  Non-fatal: if no tensor-capable
 * engine is available, status is set to ANX_JEPA_UNAVAILABLE and all
 * integration hooks become no-ops.  Registers four built-in world
 * profiles and activates "anx:world/os-default".
 */
int  anx_jepa_init(void);
void anx_jepa_shutdown(void);
bool anx_jepa_available(void);
enum anx_jepa_status anx_jepa_status_get(void);

/* ------------------------------------------------------------------ */
/* Observation                                                         */
/* ------------------------------------------------------------------ */

/* Collect a snapshot from kernel subsystems into obs (os-default). */
int anx_jepa_observe(struct anx_jepa_obs *obs_out);

/* Collect + persist as ANX_OBJ_JEPA_OBS, return OID. */
int anx_jepa_observe_store(const struct anx_jepa_obs *obs,
			   anx_oid_t *oid_out);

/* ------------------------------------------------------------------ */
/* Encoding                                                            */
/* ------------------------------------------------------------------ */

/* Encode a stored observation → ANX_OBJ_JEPA_LATENT. */
int anx_jepa_encode(const anx_oid_t *obs_oid, anx_oid_t *latent_oid_out);

/* Convenience: observe + encode in one call. */
int anx_jepa_encode_obs(const struct anx_jepa_obs *obs,
			anx_oid_t *latent_oid_out);

/* ------------------------------------------------------------------ */
/* Prediction                                                          */
/* ------------------------------------------------------------------ */

/*
 * Given a context latent z_t and an action_id, produce a predicted
 * future latent ẑ_{t+1} stored as ANX_OBJ_JEPA_LATENT.
 */
int anx_jepa_predict(const anx_oid_t *latent_oid, uint32_t action_id,
		     anx_oid_t *pred_latent_oid_out);

/* ------------------------------------------------------------------ */
/* Anomaly detection                                                   */
/* ------------------------------------------------------------------ */

/*
 * Cosine distance between two latent vectors.
 * High values (near 1.0) indicate unexpected system behaviour.
 * Returns -1.0 on error (OIDs not found or JEPA unavailable).
 */
float anx_jepa_divergence(const anx_oid_t *predicted_oid,
			  const anx_oid_t *actual_oid);

/* ------------------------------------------------------------------ */
/* Integration hooks                                                   */
/* ------------------------------------------------------------------ */

/*
 * Route planner hook.  Returns a score delta in [-ANX_JEPA_ROUTE_DELTA_MAX,
 * +ANX_JEPA_ROUTE_DELTA_MAX] representing JEPA's prediction of how the
 * system state will evolve after committing to this candidate.  Returns 0
 * if JEPA is unavailable or no latent state has been encoded yet.
 */
int32_t anx_jepa_route_score_delta(const struct anx_route_candidate *candidate);

/*
 * Memory plane hook.  Returns a utility score in [0, ANX_JEPA_MEM_UTILITY_MAX]
 * representing the predicted future relevance of obj_oid.  The memory
 * plane's decay sweep suppresses demotion when this exceeds its threshold.
 * Returns 0 if JEPA is unavailable.
 */
uint32_t anx_jepa_mem_utility_hint(const anx_oid_t *obj_oid);

/* ------------------------------------------------------------------ */
/* Training                                                            */
/* ------------------------------------------------------------------ */

/* Run one VICReg training step over n_traces trace OIDs. */
int anx_jepa_train_step(const anx_oid_t *trace_oids, uint32_t n_traces);

/* Perform an EMA update of the target encoder from the context encoder. */
int anx_jepa_update_target_encoder(void);

/* ------------------------------------------------------------------ */
/* World profile registry                                              */
/* ------------------------------------------------------------------ */

/* Register a world profile.  profile must remain valid for the subsystem
 * lifetime (typically static storage in the registering module). */
int anx_jepa_world_register(struct anx_jepa_world_profile *profile);

/* Look up a profile by URI.  Returns NULL if not found. */
struct anx_jepa_world_profile *anx_jepa_world_lookup(const char *uri);

/* Fill uris_out with up to max_count URI strings; set *found_out. */
int anx_jepa_world_list(const char **uris_out, uint32_t max_count,
			uint32_t *found_out);

/* Switch the active world.  Swaps observation collector and arch config;
 * reloads weights from active_checkpoint if present. */
int anx_jepa_world_set_active(const char *uri);

/* Return the currently active world profile (never NULL after init). */
const struct anx_jepa_world_profile *anx_jepa_world_get_active(void);

/* ------------------------------------------------------------------ */
/* Data management                                                     */
/* ------------------------------------------------------------------ */

/*
 * Ingest training data into a world's dataset store.
 * data is a flat array of obs_count serialized observation structs.
 * Stored as ANX_OBJ_STRUCTURED_DATA; OID written to *dataset_oid_out.
 */
int anx_jepa_world_ingest(const char *uri,
			  const void *data, uint64_t size,
			  uint32_t obs_count,
			  anx_oid_t *dataset_oid_out);

/*
 * Retrain encoder + predictor from the world's stored datasets.
 * max_steps bounds the training budget.  Checkpoint OID written to
 * *checkpoint_oid_out; call anx_jepa_world_activate() to deploy it.
 */
int anx_jepa_world_rebuild(const char *uri, uint32_t max_steps,
			   anx_oid_t *checkpoint_oid_out);

/* Load and activate a previously trained checkpoint for this world. */
int anx_jepa_world_activate(const char *uri,
			    const anx_oid_t *checkpoint_oid);

/* ------------------------------------------------------------------ */
/* RLM integration                                                     */
/* ------------------------------------------------------------------ */

/*
 * Data reference mode: serialise the current JEPA world state as a
 * human-readable block and append it to system_buf (null-terminated).
 * buf_size includes the existing content; the block is appended in-place.
 * Returns ANX_OK, ANX_ENOMEM if buf_size exceeded, or ANX_ENOENT if no
 * latent state is available.
 */
int anx_jepa_rlm_inject_context(char *system_buf, uint32_t buf_size);

/*
 * Plugin mode: register JEPA tool descriptors in an RLM config so the
 * LLM can call jepa_world_state, jepa_predict, and jepa_anomaly_score
 * as tool_use entries during rollouts.
 */
struct anx_rlm_config;		/* forward declaration */
int anx_jepa_rlm_install_tools(struct anx_rlm_config *config);

/* ------------------------------------------------------------------ */
/* Training trace recording                                            */
/* ------------------------------------------------------------------ */

/*
 * Record a training trace for the transition (obs_t, action, obs_t+1).
 * Encodes both observations, runs the predictor, stores the resulting
 * ANX_OBJ_JEPA_TRACE, and admits it to memplane L2.
 * Called by the workflow's "record trace" cell after each agent action.
 */
/* trace_oid_out receives the created ANX_OBJ_JEPA_TRACE OID (may be NULL). */
int anx_jepa_record_trace(const anx_oid_t *obs_t_oid,
			  uint32_t action_id,
			  const anx_oid_t *obs_tp1_oid,
			  anx_oid_t *trace_oid_out);

/*
 * Ingest a sealed ANX_OBJ_EXECUTION_TRACE into the JEPA training pipeline.
 *
 * obs_before_oid and obs_after_oid are ANX_OBJ_JEPA_OBS snapshots taken
 * immediately before and after the workflow ran.  The dominant node kind
 * in the trace is mapped to a JEPA action_id; anx_jepa_record_trace() is
 * called to produce a training sample.  When enough samples accumulate
 * (one batch_size worth), anx_jepa_train_step() fires automatically.
 *
 * Safe to call when JEPA is unavailable — returns ANX_ENOENT as a no-op.
 */
int anx_jepa_ingest_wf_trace(const anx_oid_t *exec_trace_oid,
			     const anx_oid_t *obs_before_oid,
			     const anx_oid_t *obs_after_oid);

/* ------------------------------------------------------------------ */
/* Workflow helpers                                                    */
/* ------------------------------------------------------------------ */

/* Return the OID of a named JEPA workflow (populated at jepa_init). */
int anx_jepa_workflow_oid(const char *name, anx_oid_t *oid_out);

/* ------------------------------------------------------------------ */
/* Tool dispatch (called by JEPA tool engine)                          */
/* ------------------------------------------------------------------ */

/* Serialise current world state summary as JSON into out_buf. */
int anx_jepa_tool_world_state(char *out_buf, uint32_t buf_size);

/* Predict outcome of action_name and serialise result as JSON. */
int anx_jepa_tool_predict(const char *action_name,
			  char *out_buf, uint32_t buf_size);

/* Compute and serialise current anomaly score as JSON. */
int anx_jepa_tool_anomaly_score(char *out_buf, uint32_t buf_size);

/* ------------------------------------------------------------------ */
/* Online learning integration (Phase 10)                              */
/* ------------------------------------------------------------------ */

/* Record which action won a loop iteration (gates train-step batching). */
int anx_jepa_record_winner(uint32_t action_id);

/* Return the number of online training steps completed this session. */
uint32_t anx_jepa_get_train_step_count(void);

/*
 * Compute cosine divergence for each action in the os-default vocabulary.
 * Observes current system state, encodes it, then for each action predicts
 * a future latent and measures divergence from the context latent.
 * Fills out[0..min(max, ANX_JEPA_ACT_COUNT)-1] with values in [0, 1].
 * Returns the number of entries written.  All zeros when JEPA unavailable.
 */
uint32_t anx_jepa_get_action_divergences(float *out, uint32_t max);

#endif /* ANX_JEPA_H */
