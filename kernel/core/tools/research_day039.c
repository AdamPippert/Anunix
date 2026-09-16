/* Restoring saved progress does not execute the completed result creation again. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workflow.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

int anx_research_day039(void)
{
	struct anx_state_object *objects[2] = {0};
	struct anx_so_create_params p = {0};
	struct anx_object_handle handle = {0};
	struct anx_wf_node node = {0};
	anx_oid_t oid = ANX_UUID_NIL, checkpoint = ANX_UUID_NIL, result_oid = ANX_UUID_NIL;
	uint16_t source[2], editor, gate, output;
	const char *payloads[] = {"original", "(+ 20 22)"};
	int ret;
	for (uint32_t i = 0; i < 2; i++) {
		p.object_type = ANX_OBJ_BYTE_DATA; p.payload = payloads[i]; p.payload_size = anx_strlen(payloads[i]);
		ret = anx_so_create(&p, &objects[i]);
		if (ret != ANX_OK) goto out;
	}
	ret = anx_wf_create("research-day-039", NULL, &oid);
	if (ret != ANX_OK) goto out;
	for (uint32_t i = 0; i < 2; i++) {
		node.kind = ANX_WF_NODE_STATE_REF;
		node.params.state_ref.obj_oid = objects[i]->oid;
		node.port_count = 1; node.ports[0].dir = ANX_WF_PORT_OUT;
		ret = anx_wf_node_add(&oid, &node, &source[i]);
		if (ret != ANX_OK) goto out;
	}
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_CELL_CALL;
	anx_strlcpy(node.params.cell_call.intent, "editor-eval", sizeof(node.params.cell_call.intent));
	node.port_count = 3;
	node.ports[0].dir = node.ports[1].dir = ANX_WF_PORT_IN;
	node.ports[2].dir = ANX_WF_PORT_OUT;
	ret = anx_wf_node_add(&oid, &node, &editor);
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_CAP_PROMOTION;
	node.port_count = 2; node.ports[0].dir = ANX_WF_PORT_IN; node.ports[1].dir = ANX_WF_PORT_OUT;
	if (ret == ANX_OK) ret = anx_wf_node_add(&oid, &node, &gate);
	node.kind = ANX_WF_NODE_OUTPUT; node.ports[1].dir = ANX_WF_PORT_IN;
	if (ret == ANX_OK) ret = anx_wf_node_add(&oid, &node, &output);
	if (ret == ANX_OK) ret = anx_wf_edge_add(&oid, source[0], 0, editor, 0);
	if (ret == ANX_OK) ret = anx_wf_edge_add(&oid, source[1], 0, editor, 1);
	if (ret == ANX_OK) ret = anx_wf_edge_add(&oid, editor, 2, gate, 0);
	if (ret == ANX_OK) ret = anx_wf_edge_add(&oid, editor, 2, output, 0);
	if (ret == ANX_OK) ret = anx_wf_edge_add(&oid, gate, 1, output, 1);
	if (ret != ANX_OK) goto out;
	struct anx_wf_object *wf = anx_wf_object_get(&oid);
	ret = -3900;
	if (anx_wf_run(&oid, NULL) != ANX_EPERM || wf->run_state != ANX_WF_RUN_SUSPENDED || !wf->continuation)
		goto out;
	result_oid = wf->continuation->port_oid[editor - 1][2];
	ret = anx_wf_checkpoint_save(&oid, &checkpoint);
	if (ret != ANX_OK) goto out;
	ret = -3901;
	if (anx_wf_checkpoint_restore(&oid, &checkpoint) != ANX_OK) goto out;
	ret = -3902;
	if (!wf->continuation || !wf->continuation->completed[editor - 1] ||
	    anx_uuid_compare(&wf->continuation->port_oid[editor - 1][2], &result_oid) ||
	    anx_wf_checkpoint_restore(&oid, &checkpoint) != ANX_EBUSY) goto out;
	/* A second issued image unloads progress until every dependency is valid. */
	anx_so_delete(&checkpoint, false);
	ret = anx_wf_checkpoint_save(&oid, &checkpoint);
	if (ret != ANX_OK) goto out;
	ret = -3906;
	if (wf->continuation || wf->trace_entries || anx_wf_run(&oid, NULL) != ANX_EBUSY ||
	    anx_wf_checkpoint_restore(&oid, &result_oid) != ANX_EPERM) goto out;
	wf->nodes[editor - 1].label[0] = 'X';
	if (anx_wf_checkpoint_restore(&oid, &checkpoint) != ANX_EBUSY || wf->continuation) goto out;
	wf->nodes[editor - 1].label[0] = '\0';
	ret = anx_so_open(&objects[0]->oid, ANX_OPEN_READWRITE, &handle);
	if (ret == ANX_OK) ret = anx_object_stage(&handle, ANX_UUID_NIL);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&handle, "changed", 7);
	if (ret != ANX_OK) goto out;
	ret = -3907;
	if (anx_wf_checkpoint_restore(&oid, &checkpoint) != ANX_EBUSY || wf->continuation) goto out;
	ret = anx_object_abort(&handle);
	anx_so_close(&handle);
	if (ret != ANX_OK) goto out;
	objects[0]->access_policy.rule_count = 1;
	objects[0]->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	objects[0]->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	ret = -3908;
	if (anx_wf_checkpoint_restore(&oid, &checkpoint) != ANX_EPERM || wf->continuation) goto out;
	objects[0]->access_policy.rule_count = 0;
	ret = anx_wf_checkpoint_restore(&oid, &checkpoint);
	if (ret != ANX_OK) goto out;
	ret = anx_wf_resume(&oid, ANX_WF_RESUME_SKIP, NULL);
	if (ret != ANX_OK) goto out;
	ret = -3903;
	if (wf->run_state != ANX_WF_RUN_COMPLETED || wf->output_count != 1 ||
	    anx_uuid_compare(&wf->output_oids[0], &result_oid) ||
	    anx_wf_checkpoint_restore(&oid, &checkpoint) != ANX_EBUSY) goto out;
	ret = anx_so_open(&result_oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) goto out;
	ret = -3904;
	char bytes[3] = {0};
	if (anx_so_read_payload(&handle, 0, bytes, 2) != 2 || anx_strcmp(bytes, "42")) goto out;
	anx_so_close(&handle);
	ret = anx_so_open(&wf->trace_oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) goto out;
	struct anx_wf_trace_entry entries[5];
	ret = -3905;
	if (handle.obj->payload_size != sizeof(entries) ||
	    anx_so_read_payload(&handle, 0, entries, sizeof(entries)) != sizeof(entries) ||
	    entries[2].node_id != editor || entries[4].node_id != output) goto out;
	ret = ANX_OK;
out:
	if (handle.obj && handle.obj->staged) anx_object_abort(&handle);
	anx_so_close(&handle);
	if (!anx_uuid_is_nil(&oid)) anx_wf_destroy(&oid);
	if (!anx_uuid_is_nil(&checkpoint)) anx_so_delete(&checkpoint, false);
	if (!anx_uuid_is_nil(&result_oid)) anx_so_delete(&result_oid, false);
	for (uint32_t i = 0; i < 2; i++) if (objects[i]) {
		anx_so_delete(&objects[i]->oid, false); anx_objstore_release(objects[i]);
	}
	return ret;
}
#endif
