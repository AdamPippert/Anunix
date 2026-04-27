/*
 * test_workflow.c — Host-native tests for the Workflow engine (RFC-0018).
 *
 * Phase 1 tests: create, node/edge manipulation, list, destroy.
 * Phase 2 tests: parallel dispatch, suspend on cell failure, resume.
 *
 * In the test environment no model engines are registered, so any
 * CELL_CALL/MODEL_CALL/AGENT_CALL node will fail routing and cause the
 * workflow to suspend.  Tests verify the suspend/resume cycle explicitly.
 */

#include <anx/types.h>
#include <anx/workflow.h>
#include <anx/string.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/jepa.h>
#include <anx/jepa_cell.h>
#include <anx/workflow_library.h>
#include <anx/tensor_ops.h>

int test_workflow(void)
{
	anx_oid_t		wf_oid, wf2_oid;
	uint16_t		trigger_id, model_id;
	enum anx_wf_run_state	state;
	struct anx_wf_object	*wf;
	struct anx_wf_node	node_spec;
	const struct anx_wf_continuation *cont;
	int			ret;

	anx_wf_init();
	anx_objstore_init();
	anx_cell_store_init();

	/* Test 1: create a workflow */
	ret = anx_wf_create("test-flow", "a test workflow", &wf_oid);
	if (ret != ANX_OK) return -1;

	/* Test 2: duplicate name rejected */
	ret = anx_wf_create("test-flow", "dup", &wf2_oid);
	if (ret == ANX_OK) return -2;

	/* Test 3: empty name rejected */
	ret = anx_wf_create("", "bad", &wf2_oid);
	if (ret == ANX_OK) return -3;

	/* Test 4: object retrievable */
	wf = anx_wf_object_get(&wf_oid);
	if (!wf) return -4;
	if (anx_strcmp(wf->name, "test-flow") != 0) return -5;

	/* Test 5: initial state is idle */
	ret = anx_wf_run_state_get(&wf_oid, &state);
	if (ret != ANX_OK) return -6;
	if (state != ANX_WF_RUN_IDLE) return -7;

	/* Test 6: add a trigger node */
	anx_memset(&node_spec, 0, sizeof(node_spec));
	node_spec.kind = ANX_WF_NODE_TRIGGER;
	anx_strlcpy(node_spec.label, "start", ANX_WF_LABEL_MAX);
	anx_strlcpy(node_spec.params.trigger.schedule, "manual", 64);
	node_spec.canvas_x = 20; node_spec.canvas_y = 40;
	node_spec.canvas_w = 120; node_spec.canvas_h = 48;
	ret = anx_wf_node_add(&wf_oid, &node_spec, &trigger_id);
	if (ret != ANX_OK) return -8;
	if (trigger_id == 0) return -9;

	/* Test 7: add a model call node */
	anx_memset(&node_spec, 0, sizeof(node_spec));
	node_spec.kind = ANX_WF_NODE_MODEL_CALL;
	anx_strlcpy(node_spec.label, "infer", ANX_WF_LABEL_MAX);
	anx_strlcpy(node_spec.params.model_call.model_id, "claude-3", 64);
	node_spec.canvas_x = 200; node_spec.canvas_y = 40;
	node_spec.canvas_w = 120; node_spec.canvas_h = 48;
	ret = anx_wf_node_add(&wf_oid, &node_spec, &model_id);
	if (ret != ANX_OK) return -10;

	/* Test 8: add an edge */
	ret = anx_wf_edge_add(&wf_oid, trigger_id, 0, model_id, 0);
	if (ret != ANX_OK) return -11;
	wf = anx_wf_object_get(&wf_oid);
	if (wf->edge_count != 1) return -12;

	/* Test 9: self-loop rejected */
	ret = anx_wf_edge_add(&wf_oid, trigger_id, 0, trigger_id, 0);
	if (ret == ANX_OK) return -13;

	/* Test 10: cap is derived from host hardware (4 CPUs, 1 GPU in mock) */
	wf = anx_wf_object_get(&wf_oid);
	if (wf->computed_cap == 0) return -14;

	/*
	 * Test 11: run workflow.
	 *
	 * The TRIGGER node dispatches as a no-op.  The MODEL_CALL node attempts
	 * to create and route a cell, which fails in the test environment because
	 * no model engine is registered.  The workflow transitions to SUSPENDED
	 * and returns a non-zero error.  This is the expected Phase 2 behaviour.
	 */
	anx_wf_run(&wf_oid, NULL);	/* return value intentionally ignored */

	ret = anx_wf_run_state_get(&wf_oid, &state);
	if (ret != ANX_OK) return -15;
	if (state != ANX_WF_RUN_SUSPENDED && state != ANX_WF_RUN_COMPLETED)
		return -16;

	/*
	 * Test 12: if suspended, inspect the continuation and verify it
	 * identifies the MODEL_CALL node as the failure site.
	 */
	if (state == ANX_WF_RUN_SUSPENDED) {
		cont = anx_wf_continuation_get(&wf_oid);
		if (!cont) return -17;
		if (cont->failed_node_id != model_id) return -18;
		if (cont->error_code == ANX_OK) return -19;

		/* continuation_get returns NULL for non-suspended workflows */
		{
			anx_oid_t other;
			anx_wf_create("other", NULL, &other);
			if (anx_wf_continuation_get(&other) != NULL) return -20;
			anx_wf_destroy(&other);
		}
	}

	/*
	 * Test 13: resume with SKIP — the failed node is marked complete with
	 * null outputs and execution continues.  With no remaining unfinished
	 * nodes the workflow reaches COMPLETED.
	 */
	if (state == ANX_WF_RUN_SUSPENDED) {
		ret = anx_wf_resume(&wf_oid, ANX_WF_RESUME_SKIP, NULL);
		if (ret != ANX_OK) return -21;

		ret = anx_wf_run_state_get(&wf_oid, &state);
		if (ret != ANX_OK) return -22;
		if (state != ANX_WF_RUN_COMPLETED) return -23;
	}

	/* Test 14: resume on a non-suspended workflow is rejected */
	ret = anx_wf_resume(&wf_oid, ANX_WF_RESUME_SKIP, NULL);
	if (ret == ANX_OK) return -24;

	/* Test 15: list includes our workflow */
	{
		anx_oid_t	list[16];
		uint32_t	count;

		ret = anx_wf_list(list, 16, &count);
		if (ret != ANX_OK) return -25;
		if (count == 0) return -26;
	}

	/* Test 16: remove a node cleans up edges */
	ret = anx_wf_node_remove(&wf_oid, trigger_id);
	if (ret != ANX_OK) return -27;
	wf = anx_wf_object_get(&wf_oid);
	if (wf->edge_count != 0) return -28;	/* edge must be cleaned up */

	/* Test 17: destroy */
	ret = anx_wf_destroy(&wf_oid);
	if (ret != ANX_OK) return -29;
	if (anx_wf_object_get(&wf_oid) != NULL) return -30;

	/* ---------------------------------------------------------------- */
	/* Tests 18-22: JEPA cell dispatch (via anx_jepa_cell_dispatch)     */
	/* ---------------------------------------------------------------- */

	/* Ensure tensor engine is registered so JEPA enters DEGRADED mode */
	anx_tensor_cpu_engine_init();
	anx_jepa_init();

	/* Tests 18-22 require JEPA to be available (tensor engine present).
	 * In minimal test environments without a tensor engine, skip gracefully. */
	if (anx_jepa_available()) {
		anx_oid_t obs_oid    = {0};
		anx_oid_t latent_oid = {0};
		anx_oid_t pred_oid   = {0};

		/* Test 18: jepa-observe returns a non-zero OID */
		ret = anx_jepa_cell_dispatch("jepa-observe", NULL, 0, &obs_oid);
		if (ret != ANX_OK) return -31;
		if (obs_oid.hi == 0 && obs_oid.lo == 0) return -32;

		/* Test 19: jepa-encode returns a LATENT OID from an OBS OID */
		ret = anx_jepa_cell_dispatch("jepa-encode", &obs_oid, 1, &latent_oid);
		if (ret != ANX_OK) return -33;
		if (latent_oid.hi == 0 && latent_oid.lo == 0) return -34;

		/* Test 20: jepa-observe-encode shortcut produces a LATENT OID */
		ret = anx_jepa_cell_dispatch("jepa-observe-encode",
					     NULL, 0, &latent_oid);
		if (ret != ANX_OK) return -35;
		if (latent_oid.hi == 0 && latent_oid.lo == 0) return -36;

		/* Test 21: jepa-predict:route_local predicts next latent */
		ret = anx_jepa_cell_dispatch("jepa-predict:route_local",
					     &latent_oid, 1, &pred_oid);
		if (ret != ANX_OK) return -37;
		if (pred_oid.hi == 0 && pred_oid.lo == 0) return -38;

		/* Test 22: observe-encode workflow template completes */
		{
			anx_oid_t         tmpl_oid = {0};
			anx_cid_t         run_cid;
			enum anx_wf_run_state s;

			anx_wf_lib_init();
			ret = anx_wf_lib_instantiate(
				"anx:workflow/jepa/observe-encode/v1",
				"test-jepa-oe", &tmpl_oid);
			if (ret != ANX_OK) return -39;

			ret = anx_wf_run(&tmpl_oid, &run_cid);
			if (ret != ANX_OK) return -40;

			ret = anx_wf_run_state_get(&tmpl_oid, &s);
			if (ret != ANX_OK) return -41;
			if (s != ANX_WF_RUN_COMPLETED) return -42;
		}
	}

	return 0;
}
