/* Model request admission and reservation release in the live kernel. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/model_server.h>
#include <anx/engine_lease.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/arch.h>

int anx_research_day003(void)
{
	struct anx_engine *engine = NULL;
	struct anx_model_server *srv = NULL;
	struct anx_engine_lease *extra = NULL;
	struct anx_model_desc desc = {0};
	struct anx_infer_request req = {0};
	struct anx_cell *backing;
	anx_cid_t server_cid = {0};
	anx_eid_t extra_id;
	uint64_t before, during, after;
	int rc;

	rc = anx_lease_avail_mem(ANX_MEM_L1, &before);
	if (rc != ANX_OK)
		return rc;
	if (before < 4096)
		return ANX_ENOMEM;
	desc.context_window = 128;
	desc.mem_footprint_bytes = 4096;
	desc.offline_capable = true;
	rc = anx_engine_register_model("research-day-003", ANX_ENGINE_LOCAL_MODEL,
				       ANX_CAP_QUESTION_ANSWERING, &desc, &engine);
	if (rc != ANX_OK)
		goto out;
	rc = anx_msrv_create(engine, &srv);
	if (rc != ANX_OK)
		goto out;
	server_cid = srv->cell_id;
	rc = anx_msrv_start(srv);
	if (rc != ANX_OK)
		goto out;
	if (anx_lease_avail_mem(ANX_MEM_L1, &during) != ANX_OK || during != before - 4096) {
		rc = -300;
		goto out;
	}

	/* Expired work must not consume a serving slot or increment usage. */
	req.engine_id = engine->eid;
	req.max_tokens = 32;
	req.deadline_ns = 1;
	if (anx_msrv_submit(srv, &req) != ANX_ETIMEDOUT || srv->requests_served != 0) {
		rc = -301;
		goto out;
	}
	req.deadline_ns = 0;
	req.max_tokens = 129;
	if (anx_msrv_submit(srv, &req) != ANX_EINVAL || srv->requests_served != 0) {
		rc = -302;
		goto out;
	}
	req.max_tokens = 128;
	req.deadline_ns = arch_time_now() + 10000000000ULL;
	if (anx_msrv_submit(srv, &req) != ANX_OK || srv->requests_served != 1) {
		rc = -303;
		goto out;
	}

	/* An over-capacity reservation fails without consuming the remainder. */
	anx_uuid_generate(&extra_id);
	if (anx_lease_grant(&extra_id, ANX_MEM_L1, during + 1,
			    ANX_ACCEL_NONE, 0, &extra) != ANX_ENOMEM || extra) {
		rc = -304;
		goto out;
	}
	if (anx_lease_avail_mem(ANX_MEM_L1, &after) != ANX_OK || after != during) {
		rc = -305;
		goto out;
	}
	rc = anx_msrv_stop(srv);
	if (rc != ANX_OK)
		goto out;
	if (engine->lease || anx_lease_avail_mem(ANX_MEM_L1, &after) != ANX_OK || after != before) {
		rc = -306;
		goto out;
	}
	rc = ANX_OK;
out:
	if (extra)
		anx_lease_release(extra);
	if (srv)
		anx_msrv_destroy(srv);
	/* The server API currently leaves its cell in the registry. */
	if (!anx_uuid_is_nil(&server_cid)) {
		backing = anx_cell_store_lookup(&server_cid);
		if (backing) {
			if (anx_cell_destroy(backing) != ANX_OK)
				anx_cell_store_release(backing);
		}
	}
	if (engine)
		anx_engine_unregister(engine);
	return rc;
}
#endif
