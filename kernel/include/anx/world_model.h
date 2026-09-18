/*
 * anx/world_model.h — The world model behind Anunix-world.
 *
 * The OS loop (IBAL), the EBM scorer and the `world` shell command need
 * a model that observes the system, encodes the observation into a
 * latent, predicts the latent after an action, and learns from what
 * happened. They must not care how the model is built: JEPA backs it
 * today, and another architecture (Arboris) may replace it. So they call
 * this interface, and one backend registers an ops table behind it.
 *
 * The observation and the action vocabulary describe the OS, not the
 * model, so they live here and every backend shares them. Latents,
 * checkpoints and trajectories are State Objects whose format belongs to
 * the backend; callers only pass their OIDs around.
 *
 * With no backend registered every call is safe: queries report
 * "unavailable" and operations return ANX_ENOSYS.
 */

#ifndef ANX_WORLD_MODEL_H
#define ANX_WORLD_MODEL_H

#include <anx/types.h>

#define ANX_WORLD_OBS_SCHED_CLASSES	6	/* mirrors ANX_QUEUE_CLASS_COUNT */
#define ANX_WORLD_OBS_MEM_TIERS		6	/* mirrors ANX_MEM_TIER_COUNT */
#define ANX_WORLD_MAX			8	/* registered world profiles */

/* Lifecycle of the world model backend. */
enum anx_world_status {
	ANX_WORLD_UNINITIALIZED,
	ANX_WORLD_INITIALIZING,
	ANX_WORLD_READY,	/* can encode and predict */
	ANX_WORLD_TRAINING,	/* training loop active */
	ANX_WORLD_DEGRADED,	/* CPU fallback only (no NPU/GPU) */
	ANX_WORLD_UNAVAILABLE,	/* no compute for a model at all */
};

/*
 * Action vocabulary of the os-default world: the actions available to
 * any Anunix OS agent. Other worlds may define their own sets.
 */
enum anx_world_action {
	ANX_WORLD_ACT_IDLE = 0,
	ANX_WORLD_ACT_ROUTE_LOCAL,
	ANX_WORLD_ACT_ROUTE_REMOTE,
	ANX_WORLD_ACT_ROUTE_FALLBACK,
	ANX_WORLD_ACT_MEM_PROMOTE,
	ANX_WORLD_ACT_MEM_DEMOTE,
	ANX_WORLD_ACT_MEM_FORGET,
	ANX_WORLD_ACT_CELL_SPAWN,
	ANX_WORLD_ACT_CELL_CANCEL,
	ANX_WORLD_ACT_CAP_VALIDATE,
	ANX_WORLD_ACT_CAP_SUSPEND,
	ANX_WORLD_ACT_SECURITY_ALERT,
	ANX_WORLD_ACT_COUNT,
};

/*
 * Snapshot of Anunix runtime state collected from kernel subsystems:
 * the observation of "anx:world/os-default".
 */
struct anx_world_obs {
	uint64_t timestamp_ns;

	/* Scheduler (one depth counter per queue class) */
	uint32_t sched_queue_depths[ANX_WORLD_OBS_SCHED_CLASSES];
	uint32_t active_cell_count;

	/* Memory control plane (one entry per tier) */
	uint32_t mem_decay_score_avg[ANX_WORLD_OBS_MEM_TIERS];
	uint32_t mem_entry_counts[ANX_WORLD_OBS_MEM_TIERS];

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

/* What a caller may know about a world, whatever model backs it. */
struct anx_world_info {
	const char *uri;
	const char *display_name;
	uint32_t    obs_dim;
	uint32_t    latent_dim;	/* 0 when the model has no fixed latent */
	uint32_t    action_count;
	bool        collects_obs;	/* false: world not implemented yet */
};

/* The operations a world model backend provides. */
struct anx_world_model_ops {
	const char *name;		/* "jepa", "arboris", ... */

	bool     (*available)(void);
	enum anx_world_status (*status)(void);
	uint32_t (*train_steps)(void);

	/* Observe, persist, encode, predict */
	int   (*observe)(struct anx_world_obs *obs_out);
	int   (*observe_store)(const struct anx_world_obs *obs,
			       anx_oid_t *obs_oid_out);
	int   (*encode)(const anx_oid_t *obs_oid, anx_oid_t *latent_oid_out);
	int   (*predict)(const anx_oid_t *latent_oid, uint32_t action_id,
			 anx_oid_t *predicted_oid_out);
	/* Distance between two latents in [0, 1]; negative on error */
	float (*divergence)(const anx_oid_t *a, const anx_oid_t *b);

	/* Learning from loop outcomes */
	int   (*record_winner)(uint32_t action_id);
	int   (*traj_ingest)(const struct anx_world_obs *obs,
			     uint32_t action_id, const char *world_uri);
	int   (*traj_count)(uint32_t *entries_out, uint32_t *bytes_out);
	int   (*traj_dump)(anx_oid_t *oid_out, uint32_t *bytes_out);
	void  (*traj_reset)(void);

	/* Worlds */
	int   (*world_list)(const char **uris_out, uint32_t max,
			    uint32_t *count_out);
	int   (*world_info)(const char *uri, struct anx_world_info *out);
	int   (*world_set_active)(const char *uri);
	/* Name of action id in the active world, NULL past its vocabulary */
	const char *(*action_name)(uint32_t action_id);
	int   (*world_rebuild)(const char *uri, uint32_t max_steps,
			       anx_oid_t *checkpoint_oid_out);
	int   (*world_activate)(const char *uri,
				const anx_oid_t *checkpoint_oid);
};

/*
 * Install the backend. A second registration replaces the first, so a
 * newer model can take over from the default one. ops must outlive the
 * kernel (a static table).
 */
void anx_world_model_register(const struct anx_world_model_ops *ops);

/* Name of the registered backend, or "none". */
const char *anx_world_model_name(void);

bool     anx_world_available(void);
enum anx_world_status anx_world_status_get(void);
const char *anx_world_status_name(enum anx_world_status st);
uint32_t anx_world_train_steps(void);

int anx_world_observe(struct anx_world_obs *obs_out);
int anx_world_observe_store(const struct anx_world_obs *obs,
			    anx_oid_t *obs_oid_out);
int anx_world_encode(const anx_oid_t *obs_oid, anx_oid_t *latent_oid_out);
int anx_world_predict(const anx_oid_t *latent_oid, uint32_t action_id,
		      anx_oid_t *predicted_oid_out);
/* Distance between two latents in [0, 1]; negative on error. */
float anx_world_divergence(const anx_oid_t *a, const anx_oid_t *b);

int  anx_world_record_winner(uint32_t action_id);
int  anx_world_traj_ingest(const struct anx_world_obs *obs,
			   uint32_t action_id, const char *world_uri);
/* ANX_ENOENT when the trajectory buffer is empty. */
int  anx_world_traj_count(uint32_t *entries_out, uint32_t *bytes_out);
/* Store the trajectory buffer as a State Object; ANX_ENOENT if empty. */
int  anx_world_traj_dump(anx_oid_t *oid_out, uint32_t *bytes_out);
void anx_world_traj_reset(void);

int anx_world_list(const char **uris_out, uint32_t max, uint32_t *count_out);
/* uri NULL means the active world; ANX_ENOENT when there is none. */
int anx_world_info_get(const char *uri, struct anx_world_info *out);
int anx_world_set_active(const char *uri);
/* Action count of the active world, or ANX_WORLD_ACT_COUNT without one. */
uint32_t anx_world_action_count(void);
/* Name of an action in the active world, or NULL. */
const char *anx_world_action_name(uint32_t action_id);
int anx_world_rebuild(const char *uri, uint32_t max_steps,
		      anx_oid_t *checkpoint_oid_out);
int anx_world_activate(const char *uri, const anx_oid_t *checkpoint_oid);

#endif /* ANX_WORLD_MODEL_H */
