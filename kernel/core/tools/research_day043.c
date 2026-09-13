/* Baseline resumes without checking an out-of-band semantic dependency. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workflow.h>
#include <anx/state_object.h>
#include <anx/uuid.h>

int anx_research_day043(void)
{
	struct anx_state_object *policy = NULL;
	struct anx_so_create_params p = {0};
	struct anx_object_handle h = {0};
	struct anx_wf_node node = { .kind = ANX_WF_NODE_CAP_PROMOTION };
	anx_oid_t oid = ANX_UUID_NIL;
	uint16_t id;
	int ret;
	p.object_type = ANX_OBJ_BYTE_DATA; p.payload = "policy-v1"; p.payload_size = 9;
	ret = anx_so_create(&p, &policy);
	if (ret == ANX_OK) ret = anx_so_open(&policy->oid, ANX_OPEN_READWRITE, &h);
	if (ret == ANX_OK) ret = anx_wf_create("research-day-043", NULL, &oid);
	if (ret == ANX_OK) ret = anx_wf_node_add(&oid, &node, &id);
	if (ret != ANX_OK) goto out;
	ret = -4300;
	if (anx_wf_run(&oid, NULL) != ANX_EPERM) goto out;
	ret = anx_so_replace_payload(&h, "policy-v2", 9);
	if (ret != ANX_OK) goto out;
	ret = -4301;
	if (anx_wf_resume(&oid, ANX_WF_RESUME_SKIP, NULL) != ANX_EBUSY) goto out;
	ret = ANX_OK;
out:
	if (!anx_uuid_is_nil(&oid)) anx_wf_destroy(&oid);
	anx_so_close(&h);
	if (policy) { anx_so_delete(&policy->oid, false); anx_objstore_release(policy); }
	return ret;
}
#endif
