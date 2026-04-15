/*
 * model_server.c — Model server cell management.
 *
 * The kernel creates and supervises model-server cells. Each
 * server hosts one model engine. The kernel manages lifecycle
 * (load, serve, drain, unload), routes inference requests,
 * monitors health, and restarts on failure.
 *
 * Actual inference is a stub until userland model runtimes exist.
 */

#include <anx/types.h>
#include <anx/model_server.h>
#include <anx/engine.h>
#include <anx/engine_lease.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/arch.h>

/* Registry of active model servers (flat list — few servers) */
static struct anx_list_head msrv_list;
static struct anx_spinlock msrv_lock;

void anx_msrv_init(void)
{
	anx_list_init(&msrv_list);
	anx_spin_init(&msrv_lock);
}

int anx_msrv_create(struct anx_engine *engine,
		    struct anx_model_server **out)
{
	struct anx_model_server *srv;
	struct anx_cell *cell;
	struct anx_cell_intent intent;
	int ret;

	if (!engine || !out)
		return ANX_EINVAL;

	/* Only model engines can have servers */
	if (engine->engine_class != ANX_ENGINE_LOCAL_MODEL &&
	    engine->engine_class != ANX_ENGINE_REMOTE_MODEL)
		return ANX_EINVAL;

	srv = anx_zalloc(sizeof(*srv));
	if (!srv)
		return ANX_ENOMEM;

	/* Create the model-server cell */
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, engine->name, sizeof(intent.name));
	anx_strlcpy(intent.objective, "model server",
		     sizeof(intent.objective));
	intent.priority = 100;	/* high priority */

	ret = anx_cell_create(ANX_CELL_MODEL_SERVER, &intent, &cell);
	if (ret != ANX_OK) {
		anx_free(srv);
		return ret;
	}

	srv->cell_id = cell->cid;
	srv->engine_id = engine->eid;
	srv->msrv_status = ANX_MSRV_STOPPED;
	srv->max_pending = ANX_MSRV_MAX_PENDING;
	srv->max_restarts = 3;

	anx_list_init(&srv->pending_requests);
	anx_spin_init(&srv->lock);
	anx_list_init(&srv->registry_link);

	anx_spin_lock(&msrv_lock);
	anx_list_add_tail(&srv->registry_link, &msrv_list);
	anx_spin_unlock(&msrv_lock);

	anx_cell_store_release(cell);

	*out = srv;
	return ANX_OK;
}

int anx_msrv_start(struct anx_model_server *srv)
{
	struct anx_engine *engine;
	struct anx_engine_lease *lease;
	int ret;

	if (!srv)
		return ANX_EINVAL;

	anx_spin_lock(&srv->lock);

	if (srv->msrv_status != ANX_MSRV_STOPPED &&
	    srv->msrv_status != ANX_MSRV_FAILED) {
		anx_spin_unlock(&srv->lock);
		return ANX_EPERM;
	}

	srv->msrv_status = ANX_MSRV_STARTING;
	anx_spin_unlock(&srv->lock);

	engine = anx_engine_lookup(&srv->engine_id);
	if (!engine)
		return ANX_ENOENT;

	/* Transition engine: REGISTERED -> LOADING */
	ret = anx_engine_transition(engine, ANX_ENGINE_LOADING);
	if (ret != ANX_OK)
		goto fail;

	/* Grant resource lease for model weights */
	srv->msrv_status = ANX_MSRV_LOADING_WEIGHTS;
	ret = anx_lease_grant(&engine->eid, ANX_MEM_L1,
			      engine->model.mem_footprint_bytes,
			      ANX_ACCEL_NONE, 0, &lease);
	if (ret != ANX_OK) {
		anx_engine_transition(engine, ANX_ENGINE_OFFLINE);
		goto fail;
	}

	engine->lease = lease;

	/* Transition: LOADING -> READY -> AVAILABLE */
	ret = anx_engine_transition(engine, ANX_ENGINE_READY);
	if (ret != ANX_OK)
		goto fail_release;

	ret = anx_engine_transition(engine, ANX_ENGINE_AVAILABLE);
	if (ret != ANX_OK)
		goto fail_release;

	anx_spin_lock(&srv->lock);
	srv->msrv_status = ANX_MSRV_SERVING;
	srv->consecutive_failures = 0;
	anx_spin_unlock(&srv->lock);

	return ANX_OK;

fail_release:
	anx_lease_release(lease);
	engine->lease = NULL;
fail:
	anx_spin_lock(&srv->lock);
	srv->msrv_status = ANX_MSRV_FAILED;
	anx_spin_unlock(&srv->lock);
	return ret;
}

int anx_msrv_stop(struct anx_model_server *srv)
{
	struct anx_engine *engine;

	if (!srv)
		return ANX_EINVAL;

	anx_spin_lock(&srv->lock);
	srv->msrv_status = ANX_MSRV_DRAINING;
	anx_spin_unlock(&srv->lock);

	engine = anx_engine_lookup(&srv->engine_id);
	if (engine) {
		anx_engine_transition(engine, ANX_ENGINE_DRAINING);
		anx_engine_transition(engine, ANX_ENGINE_UNLOADING);

		if (engine->lease) {
			anx_lease_release(engine->lease);
			engine->lease = NULL;
		}

		anx_engine_transition(engine, ANX_ENGINE_OFFLINE);
	}

	anx_spin_lock(&srv->lock);
	srv->msrv_status = ANX_MSRV_STOPPED;
	anx_spin_unlock(&srv->lock);

	return ANX_OK;
}

int anx_msrv_submit(struct anx_model_server *srv,
		    struct anx_infer_request *req)
{
	if (!srv || !req)
		return ANX_EINVAL;

	anx_spin_lock(&srv->lock);

	if (srv->msrv_status != ANX_MSRV_SERVING) {
		anx_spin_unlock(&srv->lock);
		return ANX_EPERM;
	}

	/* Backpressure check */
	if (srv->pending_count >= srv->max_pending) {
		anx_spin_unlock(&srv->lock);
		return ANX_EBUSY;
	}

	/*
	 * Stub: complete the request immediately.
	 * Real implementation would queue and dispatch to the
	 * model-server cell's inference runtime.
	 */
	req->status = ANX_OK;
	req->tokens_generated = 0;
	req->latency_ns = 0;

	srv->requests_served++;
	srv->consecutive_failures = 0;

	anx_spin_unlock(&srv->lock);
	return ANX_OK;
}

int anx_msrv_health_check(struct anx_model_server *srv)
{
	struct anx_engine *engine;

	if (!srv)
		return ANX_EINVAL;

	anx_spin_lock(&srv->lock);
	srv->last_health_check = arch_time_now();

	if (srv->consecutive_failures >= 3 &&
	    srv->msrv_status == ANX_MSRV_SERVING) {
		srv->msrv_status = ANX_MSRV_FAILED;
		anx_spin_unlock(&srv->lock);

		engine = anx_engine_lookup(&srv->engine_id);
		if (engine)
			anx_engine_set_status(engine, ANX_ENGINE_DEGRADED);

		return ANX_EIO;
	}

	anx_spin_unlock(&srv->lock);
	return ANX_OK;
}

int anx_msrv_restart(struct anx_model_server *srv)
{
	if (!srv)
		return ANX_EINVAL;

	anx_spin_lock(&srv->lock);

	if (srv->restart_count >= srv->max_restarts) {
		anx_spin_unlock(&srv->lock);
		return ANX_EPERM;
	}

	srv->restart_count++;
	anx_spin_unlock(&srv->lock);

	anx_msrv_stop(srv);
	return anx_msrv_start(srv);
}

struct anx_model_server *anx_msrv_lookup(const anx_eid_t *engine_id)
{
	struct anx_list_head *pos;

	if (!engine_id)
		return NULL;

	anx_spin_lock(&msrv_lock);

	ANX_LIST_FOR_EACH(pos, &msrv_list) {
		struct anx_model_server *srv;

		srv = ANX_LIST_ENTRY(pos, struct anx_model_server,
				     registry_link);
		if (anx_uuid_compare(&srv->engine_id, engine_id) == 0) {
			anx_spin_unlock(&msrv_lock);
			return srv;
		}
	}

	anx_spin_unlock(&msrv_lock);
	return NULL;
}

int anx_msrv_destroy(struct anx_model_server *srv)
{
	if (!srv)
		return ANX_EINVAL;

	if (srv->msrv_status == ANX_MSRV_SERVING)
		anx_msrv_stop(srv);

	anx_spin_lock(&msrv_lock);
	anx_list_del(&srv->registry_link);
	anx_spin_unlock(&msrv_lock);

	anx_free(srv);
	return ANX_OK;
}
