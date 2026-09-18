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
#include <anx/research_test.h>
#include <anx/namespace.h>
#include <anx/tools.h>
#include <anx/uuid.h>
#include <anx/anxml.h>

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
	anx_ns_init();

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

	/* Graph boxes must contain labels, with padding and bounded truncation. */
	{
		char graph[1024];
		if (anx_wf_render_ascii(&wf_oid, graph, sizeof(graph)) != ANX_OK)
			return -100;
		if (!anx_strstr(graph, "[trigger:start   ]") ||
		    !anx_strstr(graph, "[model:infer     ]"))
			return -101;
		anx_strlcpy(wf->nodes[model_id - 1].label,
			    "long-label-truncated", ANX_WF_LABEL_MAX);
		if (anx_wf_render_ascii(&wf_oid, graph, sizeof(graph)) != ANX_OK ||
		    !anx_strstr(graph, "[model:long-label]"))
			return -102;
		anx_strlcpy(wf->nodes[model_id - 1].label, "infer", ANX_WF_LABEL_MAX);
	}

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

	/* Shell-created state bindings must reach native workflow execution. */
	{
		struct anx_so_create_params params;
		struct anx_state_object *input;
		char serialized[1024];
		char oid_text[37];
		char *create_argv[] = { "workflow", "create", "native-bind" };
		char *state_argv[] = { "workflow", "add-node", "native-bind",
			"state_ref", "input", "default:pluralea/input" };
		char *output_argv[] = { "workflow", "add-node", "native-bind",
			"output", "result" };
		char *edge_argv[] = { "workflow", "add-edge", "native-bind", "1", "2" };
		char *run_argv[] = { "workflow", "run", "native-bind" };
		char *bad_ref_argv[] = { "workflow", "add-node", "native-bind",
			"state_ref", "missing", "default:pluralea/missing" };
		char *write_mode_argv[] = { "workflow", "add-node", "native-bind",
			"state_ref", "write-mode", "default:pluralea/input", "write" };
		char *legacy_state_argv[] = { "workflow", "add-node", "native-bind",
			"state_ref", "unbound" };
		char *legacy_cell_argv[] = { "workflow", "add-node", "native-bind",
			"cell_call", "compute" };
		char *cell_argv[] = { "workflow", "add-node", "native-bind",
			"cell_call", "sum", "pluralea-sum" };
		anx_oid_t workflows[ANX_WF_MAX_WFS];
		uint32_t workflow_count, workflow_i;
		uint16_t before;

		anx_memset(&params, 0, sizeof(params));
		params.object_type = ANX_OBJ_BYTE_DATA;
		params.payload = "36";
		params.payload_size = 2;
		if (anx_so_create(&params, &input) != ANX_OK) return -103;
		if (anx_ns_bind("default", "pluralea/input", &input->oid) != ANX_OK)
			return -104;

		if (cmd_workflow(3, create_argv) != ANX_OK) return -105;
		if (cmd_workflow(6, state_argv) != ANX_OK) return -106;
		if (cmd_workflow(5, output_argv) != ANX_OK) return -107;
		if (cmd_workflow(6, edge_argv) != ANX_OK) return -108;
		if (anx_wf_list(workflows, ANX_WF_MAX_WFS, &workflow_count) != ANX_OK)
			return -109;
		for (workflow_i = 0; workflow_i < workflow_count; workflow_i++) {
			wf = anx_wf_object_get(&workflows[workflow_i]);
			if (wf && anx_strcmp(wf->name, "native-bind") == 0) {
				wf2_oid = workflows[workflow_i];
				break;
			}
		}
		if (workflow_i == workflow_count) return -109;
		wf = anx_wf_object_get(&wf2_oid);
		if (!wf || wf->node_count != 2 ||
		    anx_uuid_compare(&wf->nodes[0].params.state_ref.obj_oid,
				     &input->oid) != 0 ||
		    wf->nodes[0].params.state_ref.write_mode)
			return -110;
		if (wf->nodes[0].port_count != 1 ||
		    wf->nodes[0].ports[0].dir != ANX_WF_PORT_OUT ||
		    wf->nodes[1].port_count != 1 ||
		    wf->nodes[1].ports[0].dir != ANX_WF_PORT_IN)
			return -120;

		anx_uuid_to_string(&input->oid, oid_text, sizeof(oid_text));
		if (anx_wf_serialize(&wf2_oid, serialized, sizeof(serialized)) != ANX_OK ||
		    !anx_strstr(serialized, oid_text) ||
		    !anx_strstr(serialized, "mode read"))
			return -111;

		if (cmd_workflow(4, run_argv) != ANX_OK ||
		    wf->run_state != ANX_WF_RUN_COMPLETED || wf->output_count != 1 ||
		    anx_uuid_compare(&wf->output_oids[0], &input->oid) != 0)
			return -112;

		before = wf->node_count;
		if (cmd_workflow(6, bad_ref_argv) == ANX_OK || wf->node_count != before)
			return -113;
		if (cmd_workflow(7, write_mode_argv) == ANX_OK || wf->node_count != before)
			return -114;
		if (cmd_workflow(5, legacy_state_argv) != ANX_OK ||
		    wf->node_count != before + 1 ||
		    !anx_uuid_is_nil(&wf->nodes[2].params.state_ref.obj_oid))
			return -115;
		if (cmd_workflow(5, legacy_cell_argv) != ANX_OK ||
		    wf->node_count != before + 2 ||
		    wf->nodes[3].params.cell_call.intent[0] != '\0')
			return -116;
		if (cmd_workflow(6, cell_argv) != ANX_OK || wf->node_count != before + 3 ||
		    anx_strcmp(wf->nodes[4].params.cell_call.intent, "pluralea-sum") != 0)
			return -117;
		if (anx_wf_serialize(&wf2_oid, serialized, sizeof(serialized)) != ANX_OK ||
		    !anx_strstr(serialized, "intent pluralea-sum"))
			return -118;

		if (anx_wf_destroy(&wf2_oid) != ANX_OK) return -119;

		/* The live showcase shape must deliver one input to local anxml. */
		{
			struct anx_state_object *model_input, *model_output;
			anx_oid_t model_wf_oid;
			char *model_create[] = { "workflow", "create", "native-anxml" };
			char *model_state[] = { "workflow", "add-node", "native-anxml",
				"state_ref", "prompt", "default:showcase/prompt" };
			char *model_cell[] = { "workflow", "add-node", "native-anxml",
				"cell_call", "local-model", "anxml-generate" };
			char *model_output_argv[] = { "workflow", "add-node", "native-anxml",
				"output", "generated" };
			char *model_edge_a[] = { "workflow", "add-edge", "native-anxml", "1", "2" };
			char *model_edge_b[] = { "workflow", "add-edge", "native-anxml", "2", "3" };
			char *model_run[] = { "workflow", "run", "native-anxml" };

			anx_memset(&params, 0, sizeof(params));
			params.object_type = ANX_OBJ_BYTE_DATA;
			params.payload = "anunix is ";
			params.payload_size = 10;
			if (anx_so_create(&params, &model_input) != ANX_OK) return -121;
			if (anx_ns_bind("default", "showcase/prompt", &model_input->oid) != ANX_OK)
				return -122;
			anx_anxml_init();
			if (cmd_workflow(3, model_create) != ANX_OK ||
			    cmd_workflow(6, model_state) != ANX_OK ||
			    cmd_workflow(6, model_cell) != ANX_OK ||
			    cmd_workflow(5, model_output_argv) != ANX_OK ||
			    cmd_workflow(6, model_edge_a) != ANX_OK ||
			    cmd_workflow(6, model_edge_b) != ANX_OK)
				return -123;
			if (anx_wf_list(workflows, ANX_WF_MAX_WFS, &workflow_count) != ANX_OK)
				return -124;
			for (workflow_i = 0; workflow_i < workflow_count; workflow_i++) {
				wf = anx_wf_object_get(&workflows[workflow_i]);
				if (wf && anx_strcmp(wf->name, "native-anxml") == 0) {
					model_wf_oid = workflows[workflow_i];
					break;
				}
			}
			if (workflow_i == workflow_count) return -125;
			wf = anx_wf_object_get(&model_wf_oid);
			if (!wf || wf->nodes[1].port_count != 2 ||
			    wf->nodes[1].ports[0].dir != ANX_WF_PORT_IN ||
			    wf->nodes[1].ports[1].dir != ANX_WF_PORT_OUT ||
			    wf->edges[0].from_port != 0 || wf->edges[0].to_port != 0 ||
			    wf->edges[1].from_port != 1 || wf->edges[1].to_port != 0)
				return -126;
			if (cmd_workflow(4, model_run) != ANX_OK ||
			    wf->run_state != ANX_WF_RUN_COMPLETED || wf->output_count != 1)
				return -127;
			model_output = anx_objstore_lookup(&wf->output_oids[0]);
			if (!model_output || model_output->object_type != ANX_OBJ_MODEL_OUTPUT ||
			    !model_output->payload_size)
				return -128;
			anx_objstore_release(model_output);
			anx_objstore_release(model_input);
			if (anx_wf_destroy(&model_wf_oid) != ANX_OK) return -129;
		}

		anx_objstore_release(input);
	}

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

		/* Test 23: traj_ingest with JEPA available records an entry. */
		{
			struct anx_world_obs obs;

			anx_memset(&obs, 0, sizeof(obs));
			obs.active_cell_count = 3;

			anx_jepa_traj_reset();
			ret = anx_jepa_traj_ingest(&obs, 1 /* route_local */,
						   "anx:world/os-default");
			if (ret != ANX_OK) return -43;
		}

		/* Test 24: export produces a valid self-describing header. */
		{
			static uint8_t  buf[8192];
			uint32_t written = 0;
			const struct anx_jepa_traj_header *hdr;

			ret = anx_jepa_export_trajectory(buf, sizeof(buf),
							  &written);
			if (ret != ANX_OK) return -44;
			if (written < sizeof(struct anx_jepa_traj_header))
				return -45;

			hdr = (const struct anx_jepa_traj_header *)buf;
			if (hdr->magic       != ANX_JEPA_TRAJ_MAGIC) return -45;
			if (hdr->entry_count != 1)                   return -45;
		}
	}

	ret = anx_research_day002();
	if (ret != ANX_OK)
		return ret;
	ret = anx_research_day005();
	if (ret == ANX_OK) ret = anx_research_day029();
	if (ret == ANX_OK) ret = anx_research_day037();
	if (ret == ANX_OK) ret = anx_research_day038();
	if (ret == ANX_OK) ret = anx_research_day039();
	return ret == ANX_OK ? anx_research_day043() : ret;
}
