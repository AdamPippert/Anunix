/* Model request admission and reservation release in the live kernel. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/model_server.h>
#include <anx/engine_lease.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/arch.h>
#include <anx/route.h>

int anx_research_day003(void)
{
	struct anx_engine *engine = NULL;
	struct anx_model_server *srv = NULL;
	struct anx_engine_lease *extra = NULL;
	struct anx_model_desc desc = {0};
	struct anx_infer_request req = {0};
	struct anx_cell *backing;
	struct anx_cell *task = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_route_result route;
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

	/* Exercise propagation through the actual cell-to-model dispatch path. */
	engine->quality_score = 100;
	engine->supports_private_data = true;
	anx_engine_set_topology(engine, 0xffff000000000003ULL, 0xffff000000000003ULL);
	anx_strlcpy(intent.name, "research-day-003-task", sizeof(intent.name));
	rc = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &task);
	if (rc != ANX_OK)
		goto out;
	task->constraints.topology_bk_set = true;
	task->constraints.topology_bk_lo = task->constraints.topology_bk_hi = 0xffff000000000003ULL;
	task->constraints.max_latency_ms = 10000;
	anx_cell_set_cognitive_envelope(task, 129, 0);
	if (anx_route_plan(task, &route) != ANX_OK ||
	    anx_uuid_compare(&route.candidates[route.selected_index].engine_id, &engine->eid) != 0) {
		rc = -307;
		goto out;
	}
	if (anx_cell_run(task) != ANX_EINVAL || task->status != ANX_CELL_FAILED ||
	    srv->requests_served != 1) {
		rc = -308;
		goto out;
	}
	anx_cell_destroy(task);
	task = NULL;
	rc = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &task);
	if (rc != ANX_OK)
		goto out;
	task->constraints.topology_bk_set = true;
	task->constraints.topology_bk_lo = task->constraints.topology_bk_hi = 0xffff000000000003ULL;
	task->constraints.max_latency_ms = ~(uint64_t)0;
	anx_cell_set_cognitive_envelope(task, 64, 0);
	if (anx_cell_run(task) != ANX_OK || task->status != ANX_CELL_COMPLETED ||
	    srv->requests_served != 2) {
		rc = -309;
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
	if (task)
		anx_cell_destroy(task);
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
