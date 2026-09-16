/* A reusable workflow stops before execution when a pinned condition changes. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workflow_reuse.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>

int anx_research_day038(void)
{
	struct anx_state_object *obj = NULL;
	struct anx_so_create_params params = {0};
	struct anx_object_handle write = {0};
	struct anx_wf_node node = {0};
	struct anx_wf_reuse_view view;
	anx_oid_t oid = ANX_UUID_NIL, conditions[2];
	uint16_t source, output;
	int ret;
	uint32_t checkpoint = 1;
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = "condition-v1"; params.payload_size = 12;
	ret = anx_so_create(&params, &obj);
	if (ret == ANX_OK) ret = anx_so_open(&obj->oid, ANX_OPEN_READWRITE, &write);
	if (ret == ANX_OK) ret = anx_wf_create("research-day-038", NULL, &oid);
	if (ret != ANX_OK) goto out;
	conditions[0] = conditions[1] = obj->oid;
	node.kind = ANX_WF_NODE_STATE_REF;
	node.params.state_ref.obj_oid = obj->oid;
	node.port_count = 1; node.ports[0].dir = ANX_WF_PORT_OUT;
	ret = anx_wf_node_add(&oid, &node, &source);
	anx_memset(&node, 0, sizeof(node));
	node.kind = ANX_WF_NODE_OUTPUT;
	node.port_count = 1; node.ports[0].dir = ANX_WF_PORT_IN;
	if (ret == ANX_OK) ret = anx_wf_node_add(&oid, &node, &output);
	if (ret == ANX_OK) ret = anx_wf_edge_add(&oid, source, 0, output, 0);
	if (ret == ANX_OK) ret = anx_wf_reuse_bind(&oid, ANX_WF_DETERMINISTIC, conditions, 1);
	if (ret == ANX_OK) ret = anx_wf_run(&oid, NULL);
	if (ret != ANX_OK) goto out;
	struct anx_wf_object *wf = anx_wf_object_get(&oid);
	if (wf->output_count != 1 || anx_uuid_compare(&wf->output_oids[0], &obj->oid)) { ret = -3800; goto out; }
	anx_time_t previous_run = wf->last_run;
	ret = anx_so_replace_payload(&write, "condition-v2", 12);
	checkpoint = 2;
	if (ret != ANX_OK) goto out;
	ret = -3801;
	if (anx_wf_run(&oid, NULL) != ANX_EBUSY || wf->last_run != previous_run || wf->output_count) goto out;
	ret = -3802;
	if (anx_wf_reuse_status(&oid, &view) != ANX_OK || !view.demoted || view.form != ANX_WF_EXPLORATORY ||
	    view.reason != ANX_EBUSY || view.successful_runs != 1 || anx_wf_run(&oid, NULL) != ANX_EBUSY) goto out;
	if (anx_wf_reuse_bind(&oid, ANX_WF_DETERMINISTIC, conditions, 2) != ANX_EINVAL ||
	    anx_wf_reuse_bind(&oid, ANX_WF_DETERMINISTIC, NULL, 0) != ANX_EINVAL) goto out;
	ret = anx_wf_reuse_bind(&oid, ANX_WF_DETERMINISTIC, conditions, 1);
	checkpoint = 3;
	if (ret == ANX_OK) { checkpoint = 4; ret = anx_wf_run(&oid, NULL); }
	if (ret != ANX_OK) goto out;
	/* Changing the graph requires a fresh validation, even with unchanged inputs. */
	anx_strlcpy(wf->nodes[output - 1].params.output.dest_name, "changed", ANX_WF_NAME_MAX);
	ret = -3803;
	if (anx_wf_run(&oid, NULL) != ANX_EBUSY || wf->output_count) goto out;
	wf->nodes[output - 1].kind = ANX_WF_NODE_MODEL_CALL;
	if (anx_wf_reuse_bind(&oid, ANX_WF_DETERMINISTIC, conditions, 1) != ANX_EPERM) goto out;
	wf->nodes[output - 1].kind = ANX_WF_NODE_OUTPUT;
	ret = anx_wf_reuse_bind(&oid, ANX_WF_DETERMINISTIC, conditions, 1);
	if (ret != ANX_OK) goto out;
	obj->access_policy.rule_count = 1;
	obj->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	obj->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	ret = -3805;
	if (anx_wf_run(&oid, NULL) != ANX_EPERM || wf->output_count) goto out;
	obj->access_policy.rule_count = 0;
	/* Restoring access does not silently reactivate a demoted form. */
	if (anx_wf_run(&oid, NULL) != ANX_EPERM) goto out;
	wf->nodes[output - 1].kind = ANX_WF_NODE_CAP_PROMOTION;
	wf->nodes[output - 1].port_count = 2;
	wf->nodes[output - 1].ports[1].dir = ANX_WF_PORT_OUT;
	ret = anx_wf_reuse_bind(&oid, ANX_WF_HYBRID, conditions, 1);
	if (ret != ANX_OK) goto out;
	ret = -3806;
	if (anx_wf_run(&oid, NULL) != ANX_EPERM ||
	    anx_wf_reuse_status(&oid, &view) != ANX_OK || !view.demoted || view.successful_runs ||
	    anx_wf_resume(&oid, ANX_WF_RESUME_RETRY, NULL) != ANX_EPERM ||
	    anx_wf_resume(&oid, ANX_WF_RESUME_ABORT, NULL) != ANX_OK) goto out;
	wf->nodes[output - 1].kind = ANX_WF_NODE_OUTPUT;
	wf->nodes[output - 1].port_count = 1;
	ret = anx_wf_reuse_bind(&oid, ANX_WF_DETERMINISTIC, conditions, 1);
	if (ret != ANX_OK) goto out;
	ret = anx_so_delete(&obj->oid, false);
	if (ret != ANX_OK) goto out;
	ret = -3804;
	if (anx_wf_run(&oid, NULL) != ANX_ENOENT || wf->output_count ||
	    anx_wf_reuse_status(&oid, &view) != ANX_OK || !view.demoted) goto out;
	ret = ANX_OK;
out:
	if (ret != ANX_OK) kprintf("day-038: checkpoint %u rc=%d\n", checkpoint, ret);
	if (!anx_uuid_is_nil(&oid)) anx_wf_destroy(&oid);
	anx_so_close(&write);
	if (obj) { anx_so_delete(&obj->oid, false); anx_objstore_release(obj); }
	return ret;
}
#endif
