/* Keep live workflow intermediates despite a stronger ordinary eviction score. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workflow.h>
#include <anx/memplane.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

static int waiting_workflow(const char *name, const anx_oid_t *needed, const anx_oid_t *unused,
			    enum anx_wf_node_kind consumer, anx_oid_t *out)
{
	struct anx_wf_node node = {0};
	uint16_t source, spare, target;
	int ret = anx_wf_create(name, "Workflow cache liveness regression", out);
	if (ret != ANX_OK) return ret;
	node.kind = ANX_WF_NODE_STATE_REF;
	node.params.state_ref.obj_oid = *needed;
	node.port_count = 1;
	node.ports[0].dir = ANX_WF_PORT_OUT;
	ret = anx_wf_node_add(out, &node, &source);
	node.params.state_ref.obj_oid = *unused;
	if (ret == ANX_OK) ret = anx_wf_node_add(out, &node, &spare);
	anx_memset(&node, 0, sizeof(node));
	node.kind = consumer;
	node.port_count = 1;
	node.ports[0].dir = ANX_WF_PORT_IN;
	if (consumer == ANX_WF_NODE_CAP_PROMOTION) {
		node.port_count = 2;
		node.ports[1].dir = ANX_WF_PORT_OUT;
	}
	if (ret == ANX_OK) ret = anx_wf_node_add(out, &node, &target);
	if (ret == ANX_OK) ret = anx_wf_edge_add(out, source, 0, target, 0);
	if (ret != ANX_OK) return ret;
	ret = anx_wf_run(out, NULL);
	if (consumer == ANX_WF_NODE_CAP_PROMOTION)
		return ret == ANX_EPERM && anx_wf_object_get(out)->run_state == ANX_WF_RUN_SUSPENDED ? ANX_OK : ANX_EIO;
	return ret == ANX_OK && anx_wf_object_get(out)->run_state == ANX_WF_RUN_WAITING_HUMAN ? ANX_OK : ANX_EIO;
}

int anx_research_day029(void)
{
	struct anx_state_object *objects[2] = {0};
	struct anx_mem_entry *entries[2] = {0};
	struct anx_so_create_params params = {0};
	anx_oid_t pool[2], workflows[3] = {0}, victim, sentinel;
	int ret;
	for (uint32_t i = 0; i < 2; i++) {
		params.object_type = ANX_OBJ_BYTE_DATA;
		params.payload = "cached";
		params.payload_size = 6;
		ret = anx_so_create(&params, &objects[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&objects[i]->oid);
		if (ret == ANX_OK) ret = anx_memplane_admit(&objects[i]->oid, ANX_ADMIT_CACHEABLE, &entries[i]);
		if (ret != ANX_OK) goto out;
		pool[i] = objects[i]->oid;
		entries[i]->decay_score = i == 0 ? 1000 : 0;
	}
	ret = waiting_workflow("research-day-029-first", &pool[0], &pool[1], ANX_WF_NODE_CAP_PROMOTION, &workflows[0]);
	if (ret == ANX_OK)
		ret = waiting_workflow("research-day-029-shared", &pool[0], &pool[1], ANX_WF_NODE_CAP_PROMOTION, &workflows[1]);
	if (ret != ANX_OK) goto out;
	ret = -2901;
	if (anx_memplane_evict(pool, 2, ANX_MEM_L0, &victim) != ANX_OK ||
	    anx_uuid_compare(&victim, &pool[1]) || !anx_mem_in_tier(entries[0], ANX_MEM_L0) ||
	    anx_mem_in_tier(entries[1], ANX_MEM_L0))
		goto out;
	anx_uuid_generate(&sentinel); victim = sentinel;
	ret = -2902;
	if (anx_memplane_evict(&pool[0], 1, ANX_MEM_L0, &victim) != ANX_ENOENT ||
	    anx_uuid_compare(&victim, &sentinel) || anx_memplane_demote(entries[0], ANX_MEM_L1) != ANX_EBUSY)
		goto out;
	for (int mode = ANX_FORGET_HARD_DELETE; mode <= ANX_FORGET_REDERIVE; mode++) {
		int result = anx_memplane_forget(entries[0], (enum anx_forget_mode)mode);
		if (result != ANX_EBUSY) {
			if (result == ANX_OK && mode == ANX_FORGET_HARD_DELETE) entries[0] = NULL;
			goto out;
		}
	}
	ret = anx_wf_resume(&workflows[0], ANX_WF_RESUME_SKIP, NULL);
	if (ret != ANX_OK) goto out;
	ret = -2903;
	if (anx_wf_object_get(&workflows[0])->run_state != ANX_WF_RUN_COMPLETED ||
	    anx_memplane_demote(entries[0], ANX_MEM_L0) != ANX_EBUSY)
		goto out;
	ret = anx_wf_resume(&workflows[1], ANX_WF_RESUME_ABORT, NULL);
	if (ret != ANX_OK) goto out;
	ret = -2904;
	if (anx_memplane_evict(&pool[0], 1, ANX_MEM_L0, &victim) != ANX_OK ||
	    anx_uuid_compare(&victim, &pool[0]) || anx_mem_in_tier(entries[0], ANX_MEM_L0))
		goto out;
	ret = anx_memplane_promote(entries[0], ANX_MEM_L0);
	if (ret == ANX_OK)
		ret = waiting_workflow("research-day-029-review", &pool[0], &pool[1], ANX_WF_NODE_HUMAN_REVIEW, &workflows[2]);
	if (ret != ANX_OK) goto out;
	ret = -2905;
	if (anx_memplane_demote(entries[0], ANX_MEM_L0) != ANX_EBUSY ||
	    anx_wf_destroy(&workflows[2]) != ANX_OK)
		goto out;
	workflows[2] = ANX_UUID_NIL;
	ret = anx_memplane_demote(entries[0], ANX_MEM_L0);
out:
	for (uint32_t i = 0; i < 3; i++)
		if (!anx_uuid_is_nil(&workflows[i])) anx_wf_destroy(&workflows[i]);
	for (uint32_t i = 0; i < 2; i++) {
		if (entries[i]) anx_memplane_forget(entries[i], ANX_FORGET_HARD_DELETE);
		if (objects[i]) { anx_so_delete(&objects[i]->oid, false); anx_objstore_release(objects[i]); }
	}
	return ret;
}
#endif
