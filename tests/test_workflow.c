/*
 * test_workflow.c — Host-native tests for the Workflow engine (RFC-0018).
 *
 * Exercises create, node/edge manipulation, run, list, and destroy.
 * No hardware dependencies — kprintf side effects are acceptable.
 */

#include <anx/types.h>
#include <anx/workflow.h>
#include <anx/string.h>
#include <anx/state_object.h>

int test_workflow(void)
{
	anx_oid_t		wf_oid, wf2_oid;
	uint16_t		node_id, node_id2;
	enum anx_wf_run_state	state;
	struct anx_wf_object	*wf;
	struct anx_wf_node	node_spec;
	int			ret;

	anx_wf_init();
	anx_objstore_init();	/* needed by some subsystems */

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
	ret = anx_wf_node_add(&wf_oid, &node_spec, &node_id);
	if (ret != ANX_OK) return -8;
	if (node_id == 0) return -9;

	/* Test 7: add a model call node */
	anx_memset(&node_spec, 0, sizeof(node_spec));
	node_spec.kind = ANX_WF_NODE_MODEL_CALL;
	anx_strlcpy(node_spec.label, "infer", ANX_WF_LABEL_MAX);
	anx_strlcpy(node_spec.params.model_call.model_id, "claude-3", 64);
	node_spec.canvas_x = 200; node_spec.canvas_y = 40;
	node_spec.canvas_w = 120; node_spec.canvas_h = 48;
	ret = anx_wf_node_add(&wf_oid, &node_spec, &node_id2);
	if (ret != ANX_OK) return -10;

	/* Test 8: add an edge */
	ret = anx_wf_edge_add(&wf_oid, node_id, 0, node_id2, 0);
	if (ret != ANX_OK) return -11;
	wf = anx_wf_object_get(&wf_oid);
	if (wf->edge_count != 1) return -12;

	/* Test 9: self-loop rejected */
	ret = anx_wf_edge_add(&wf_oid, node_id, 0, node_id, 0);
	if (ret == ANX_OK) return -13;

	/* Test 10: run workflow */
	ret = anx_wf_run(&wf_oid, NULL);
	if (ret != ANX_OK) return -14;
	ret = anx_wf_run_state_get(&wf_oid, &state);
	if (ret != ANX_OK) return -15;
	if (state != ANX_WF_RUN_COMPLETED) return -16;

	/* Test 11: list includes our workflow */
	{
		anx_oid_t	list[16];
		uint32_t	count;

		ret = anx_wf_list(list, 16, &count);
		if (ret != ANX_OK) return -17;
		if (count == 0) return -18;
	}

	/* Test 12: remove a node cleans up edges */
	ret = anx_wf_node_remove(&wf_oid, node_id);
	if (ret != ANX_OK) return -19;
	wf = anx_wf_object_get(&wf_oid);
	if (wf->edge_count != 0) return -20;	/* edge should be gone too */

	/* Test 13: destroy */
	ret = anx_wf_destroy(&wf_oid);
	if (ret != ANX_OK) return -21;
	if (anx_wf_object_get(&wf_oid) != NULL) return -22;

	return 0;
}
