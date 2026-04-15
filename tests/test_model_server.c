/*
 * test_model_server.c — Tests for model server cells.
 */

#include <anx/types.h>
#include <anx/model_server.h>
#include <anx/engine.h>
#include <anx/engine_lease.h>
#include <anx/string.h>

int test_model_server(void)
{
	struct anx_engine *engine;
	struct anx_model_server *srv;
	struct anx_model_desc desc;
	struct anx_infer_request req;
	int ret;

	anx_engine_registry_init();
	anx_lease_init();
	anx_msrv_init();
	anx_cell_store_init();

	/* Register a local model */
	anx_memset(&desc, 0, sizeof(desc));
	desc.param_count = 3000000000ULL;
	desc.quant = ANX_QUANT_Q4;
	desc.context_window = 8192;
	desc.mem_footprint_bytes = 2ULL * 1024 * 1024 * 1024;
	desc.offline_capable = true;

	ret = anx_engine_register_model("phi-3-mini-q4",
					ANX_ENGINE_LOCAL_MODEL,
					ANX_CAP_QUESTION_ANSWERING,
					&desc, &engine);
	if (ret != ANX_OK)
		return -1;

	/* Create a model server */
	ret = anx_msrv_create(engine, &srv);
	if (ret != ANX_OK)
		return -2;

	if (srv->msrv_status != ANX_MSRV_STOPPED)
		return -3;

	/* Cannot submit to stopped server */
	anx_memset(&req, 0, sizeof(req));
	ret = anx_msrv_submit(srv, &req);
	if (ret != ANX_EPERM)
		return -4;

	/* Start the server */
	ret = anx_msrv_start(srv);
	if (ret != ANX_OK)
		return -5;

	if (srv->msrv_status != ANX_MSRV_SERVING)
		return -6;

	/* Engine should be available */
	if (engine->status != ANX_ENGINE_AVAILABLE)
		return -7;

	/* Engine should have a lease */
	if (!engine->lease)
		return -8;

	/* Submit an inference request */
	anx_memset(&req, 0, sizeof(req));
	req.max_tokens = 256;
	ret = anx_msrv_submit(srv, &req);
	if (ret != ANX_OK)
		return -9;

	if (srv->requests_served != 1)
		return -10;

	/* Health check should pass */
	ret = anx_msrv_health_check(srv);
	if (ret != ANX_OK)
		return -11;

	/* Lookup by engine ID */
	{
		struct anx_model_server *found;

		found = anx_msrv_lookup(&engine->eid);
		if (!found || found != srv)
			return -12;
	}

	/* Stop the server */
	ret = anx_msrv_stop(srv);
	if (ret != ANX_OK)
		return -13;

	if (srv->msrv_status != ANX_MSRV_STOPPED)
		return -14;

	/* Engine should be offline, lease released */
	if (engine->status != ANX_ENGINE_OFFLINE)
		return -15;
	if (engine->lease)
		return -16;

	/* Restart */
	ret = anx_msrv_start(srv);
	if (ret != ANX_OK)
		return -17;

	if (srv->msrv_status != ANX_MSRV_SERVING)
		return -18;

	/* Destroy */
	anx_msrv_destroy(srv);
	anx_engine_unregister(engine);
	return 0;
}
