/* A paused workflow retains its declared semantic dependency chain. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workflow_semantic.h>
#include <anx/memplane.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/string.h>

int anx_research_day048(void)
{
	struct anx_state_object *objects[3] = {0};
	struct anx_mem_entry *entries[3] = {0};
	struct anx_semantic_spec *spec = anx_zalloc(sizeof(*spec));
	struct anx_wf_node node = { .kind = ANX_WF_NODE_CAP_PROMOTION };
	anx_oid_t workflow = ANX_UUID_NIL, manifest = ANX_UUID_NIL;
	const char *names[] = {"evidence", "embedding", "index"};
	uint16_t id;
	int ret = ANX_ENOMEM;
	if (!spec) goto out;
	spec->resource_count = 3; spec->requires_count = 2;
	for (uint32_t i = 0; i < 3; i++) {
		struct anx_so_create_params p = { .object_type = ANX_OBJ_BYTE_DATA, .payload = names[i], .payload_size = anx_strlen(names[i]) };
		ret = anx_so_create(&p, &objects[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&objects[i]->oid);
		if (ret == ANX_OK) ret = anx_memplane_admit(&objects[i]->oid, ANX_ADMIT_CACHEABLE, &entries[i]);
		if (ret == ANX_OK) ret = anx_memplane_set_validation(entries[i], ANX_MEMVAL_VALIDATED);
		if (ret != ANX_OK) goto out;
		anx_strlcpy(spec->resources[i].name, names[i], ANX_SEMANTIC_NAME_MAX);
		spec->resources[i].kind = i == 0 ? ANX_SEMANTIC_POLICY : (i == 1 ? ANX_SEMANTIC_EMBEDDING : ANX_SEMANTIC_INDEX);
		spec->resources[i].oid = objects[i]->oid; spec->resources[i].version = objects[i]->version;
		if (i) spec->requires[i - 1] = (struct anx_semantic_requires){i, i - 1, objects[i - 1]->oid, objects[i - 1]->version};
	}
	ret = anx_wf_create("research-day-048", NULL, &workflow);
	if (ret == ANX_OK) ret = anx_wf_node_add(&workflow, &node, &id);
	if (ret == ANX_OK) ret = anx_wf_semantic_bind(&workflow, spec, &manifest);
	if (ret != ANX_OK) goto out;
	ret = -4800;
	if (anx_wf_run(&workflow, NULL) != ANX_EPERM || anx_wf_object_get(&workflow)->run_state != ANX_WF_RUN_SUSPENDED) goto out;
	ret = -4801;
	if (anx_memplane_demote(entries[0], ANX_MEM_L0) != ANX_EBUSY || !anx_mem_in_tier(entries[0], ANX_MEM_L0)) goto out;
	ret = ANX_OK;
out:
	if (!anx_uuid_is_nil(&workflow)) anx_wf_destroy(&workflow);
	if (!anx_uuid_is_nil(&manifest)) anx_so_delete(&manifest, false);
	for (uint32_t i = 0; i < 3; i++) {
		if (entries[i]) anx_memplane_forget(entries[i], ANX_FORGET_HARD_DELETE);
		if (objects[i]) { anx_so_delete(&objects[i]->oid, false); anx_objstore_release(objects[i]); }
	}
	anx_free(spec);
	return ret;
}
#endif
