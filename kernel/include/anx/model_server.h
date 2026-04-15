/*
 * anx/model_server.h — Model server cell management.
 *
 * A model server is a privileged cell that hosts a running model
 * engine. The kernel manages its lifecycle, routes inference
 * requests to it, monitors health, and restarts on failure.
 *
 * Actual inference runs inside the model-server cell, not in
 * the kernel. The kernel is the control plane.
 */

#ifndef ANX_MODEL_SERVER_H
#define ANX_MODEL_SERVER_H

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/engine.h>
#include <anx/list.h>
#include <anx/spinlock.h>

/* --- Model server status --- */

enum anx_msrv_status {
	ANX_MSRV_STARTING,
	ANX_MSRV_LOADING_WEIGHTS,
	ANX_MSRV_READY,
	ANX_MSRV_SERVING,
	ANX_MSRV_DRAINING,
	ANX_MSRV_STOPPED,
	ANX_MSRV_FAILED,
};

/* --- Inference request (kernel-side descriptor) --- */

struct anx_infer_request {
	anx_cid_t requestor_cid;
	anx_eid_t engine_id;

	/* Input/output as State Object references */
	anx_oid_t input_oid;
	anx_oid_t output_oid;

	/* Constraints */
	uint32_t max_tokens;
	uint64_t deadline_ns;		/* 0 = no deadline */

	/* Result (filled on completion) */
	int status;
	uint32_t tokens_generated;
	uint64_t latency_ns;

	struct anx_list_head queue_link;
};

/* --- Model server descriptor --- */

#define ANX_MSRV_MAX_PENDING	32

struct anx_model_server {
	anx_cid_t cell_id;
	anx_eid_t engine_id;
	enum anx_msrv_status msrv_status;

	/* Health */
	uint32_t requests_served;
	uint32_t requests_failed;
	uint32_t consecutive_failures;
	anx_time_t last_health_check;

	/* Request queue with backpressure */
	struct anx_list_head pending_requests;
	uint32_t pending_count;
	uint32_t max_pending;

	/* Restart policy */
	uint32_t max_restarts;
	uint32_t restart_count;

	/* Bookkeeping */
	struct anx_spinlock lock;
	struct anx_list_head registry_link;
};

/* --- Model Server API --- */

/* Initialize the model server subsystem */
void anx_msrv_init(void);

/* Create a model server cell for an engine */
int anx_msrv_create(struct anx_engine *engine,
		    struct anx_model_server **out);

/* Start a model server (load weights, begin serving) */
int anx_msrv_start(struct anx_model_server *srv);

/* Stop a model server (drain, unload) */
int anx_msrv_stop(struct anx_model_server *srv);

/* Submit an inference request */
int anx_msrv_submit(struct anx_model_server *srv,
		    struct anx_infer_request *req);

/* Check health of a model server */
int anx_msrv_health_check(struct anx_model_server *srv);

/* Restart a failed model server */
int anx_msrv_restart(struct anx_model_server *srv);

/* Look up a model server by engine ID */
struct anx_model_server *anx_msrv_lookup(const anx_eid_t *engine_id);

/* Destroy a model server */
int anx_msrv_destroy(struct anx_model_server *srv);

#endif /* ANX_MODEL_SERVER_H */
