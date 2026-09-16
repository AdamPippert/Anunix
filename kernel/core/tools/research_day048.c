/* A paused workflow retains its declared semantic dependency chain. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workflow_semantic.h>
#include <anx/memplane.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/external_call.h>
#include <anx/crypto.h>

static int self_validate(struct anx_external_call *call, void *arg)
{
	(void)call;
	struct anx_mem_entry *entry = arg;
	uint64_t generation = entry->validation_generation;
	return anx_memplane_set_validation(entry, ANX_MEMVAL_VALIDATED) == ANX_EPERM &&
		entry->validation == ANX_MEMVAL_CONTESTED && entry->validation_generation == generation ? ANX_OK : -4809;
}

int anx_research_day048(void)
{
	struct anx_state_object *objects[4] = {0};
	struct anx_mem_entry *entries[4] = {0};
	struct anx_semantic_spec *spec = anx_zalloc(sizeof(*spec));
	struct anx_wf_node node = { .kind = ANX_WF_NODE_CAP_PROMOTION };
	anx_oid_t workflow = ANX_UUID_NIL, manifest = ANX_UUID_NIL, second = ANX_UUID_NIL;
	anx_oid_t checkpoint = ANX_UUID_NIL, resolved = ANX_UUID_NIL, victim = ANX_UUID_NIL;
	const char *names[] = {"evidence", "embedding", "index", "unrelated"};
	struct anx_cell *caller = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call *call = NULL;
	uint8_t before[32], after[32];
	uint16_t id;
	int ret = ANX_ENOMEM;
	if (!spec) goto out;
	spec->resource_count = 3; spec->requires_count = 2;
	for (uint32_t i = 0; i < 4; i++) {
		struct anx_so_create_params p = { .object_type = ANX_OBJ_BYTE_DATA, .payload = names[i], .payload_size = anx_strlen(names[i]) };
		ret = anx_so_create(&p, &objects[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&objects[i]->oid);
		if (ret == ANX_OK) ret = anx_memplane_admit(&objects[i]->oid, ANX_ADMIT_CACHEABLE, &entries[i]);
		if (ret == ANX_OK) ret = anx_memplane_set_validation(entries[i], ANX_MEMVAL_VALIDATED);
		if (ret != ANX_OK) goto out;
		if (i == 3) continue;
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
	struct anx_wf_object *wf = anx_wf_object_get(&workflow);
	struct anx_wf_continuation *saved = wf->continuation;
	struct anx_wf_trace_entry *trace = wf->trace_entries;
	uint32_t trace_count = wf->trace_entry_count;
	anx_sha256(saved, sizeof(*saved), before);
	ret = -4802;
	for (uint32_t i = 0; i < 3; i++) {
		entries[i]->decay_score = 1000;
		if (!anx_wf_cache_needed(&objects[i]->oid) || anx_memplane_demote(entries[i], ANX_MEM_L1) != ANX_EBUSY ||
		    anx_memplane_forget(entries[i], ANX_FORGET_HARD_DELETE) != ANX_EBUSY ||
		    anx_memplane_forget(entries[i], ANX_FORGET_ARCHIVE) != ANX_EBUSY) goto out;
	}
	anx_oid_t pool[] = {objects[0]->oid, objects[1]->oid, objects[2]->oid, objects[3]->oid};
	if (anx_memplane_evict(pool, 4, ANX_MEM_L0, &victim) != ANX_OK || anx_uuid_compare(&victim, &objects[3]->oid)) goto out;
	uint64_t original_generation = entries[0]->validation_generation;
	ret = anx_memplane_add_contradiction(entries[0]);
	if (ret == ANX_OK) ret = anx_memplane_add_contradiction(entries[0]);
	if (ret != ANX_OK) goto out;
	ret = -4803;
	if (entries[0]->validation != ANX_MEMVAL_CONTESTED ||
	    anx_wf_semantic_resolve(&workflow, "index", &resolved) != ANX_EPERM || !anx_uuid_is_nil(&resolved) ||
	    anx_wf_resume(&workflow, ANX_WF_RESUME_SKIP, NULL) != ANX_EPERM ||
	    anx_wf_checkpoint_save(&workflow, &checkpoint) != ANX_EPERM || !anx_uuid_is_nil(&checkpoint)) goto out;
	anx_sha256(wf->continuation, sizeof(*saved), after);
	ret = -4804;
	if (wf->continuation != saved || anx_memcmp(before, after, sizeof(before)) ||
	    wf->trace_entries != trace || wf->trace_entry_count != trace_count || wf->run_state != ANX_WF_RUN_SUSPENDED ||
	    anx_memplane_forget(entries[0], ANX_FORGET_HARD_DELETE) != ANX_EBUSY) goto out;
	anx_strlcpy(intent.name, "research-day-048-validator", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch048", self_validate, entries[0]);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	if (!call) { ret = ANX_ENOMEM; goto out; }
	anx_strlcpy(call->endpoint, "anxresearch048://validate", sizeof(call->endpoint));
	caller->ext_call = call; caller->execution.allow_side_effects = true;
	ret = anx_cell_run(caller);
	if (ret == ANX_OK) ret = anx_memplane_set_validation(entries[0], ANX_MEMVAL_VALIDATED);
	if (ret != ANX_OK) goto out;
	ret = -4805;
	if (entries[0]->validation_generation <= original_generation ||
	    anx_wf_semantic_resolve(&workflow, "index", &resolved) != ANX_EBUSY || !anx_uuid_is_nil(&resolved) ||
	    anx_wf_resume(&workflow, ANX_WF_RESUME_SKIP, NULL) != ANX_EBUSY ||
	    anx_wf_semantic_bind(&workflow, spec, &second) != ANX_EBUSY || !anx_uuid_is_nil(&second)) goto out;
	ret = anx_wf_resume(&workflow, ANX_WF_RESUME_ABORT, NULL);
	if (ret != ANX_OK) goto out;
	ret = -4806;
	for (uint32_t i = 0; i < 3; i++)
		if (anx_wf_cache_needed(&objects[i]->oid) || anx_memplane_demote(entries[i], ANX_MEM_L0) != ANX_OK) goto out;
	ret = anx_wf_semantic_bind(&workflow, spec, &second);
	if (ret != ANX_OK) goto out;
	ret = -4807;
	if (anx_wf_run(&workflow, NULL) != ANX_EPERM ||
	    anx_wf_resume(&workflow, ANX_WF_RESUME_SKIP, NULL) != ANX_OK || wf->run_state != ANX_WF_RUN_COMPLETED) goto out;
	/* Forget/readmit cannot recreate an old validation identity. */
	uint64_t prior = entries[0]->validation_generation;
	ret = anx_memplane_forget(entries[0], ANX_FORGET_HARD_DELETE);
	if (ret != ANX_OK) goto out;
	entries[0] = NULL;
	ret = anx_memplane_admit(&objects[0]->oid, ANX_ADMIT_CACHEABLE, &entries[0]);
	if (ret == ANX_OK) ret = anx_memplane_set_validation(entries[0], ANX_MEMVAL_VALIDATED);
	if (ret != ANX_OK) goto out;
	ret = -4808;
	if (entries[0]->validation_generation <= prior || anx_wf_semantic_check(wf) != ANX_EBUSY ||
	    anx_memplane_set_validation(entries[0], (enum anx_mem_validation_state)99) != ANX_EINVAL) goto out;
	uint64_t epoch = entries[0]->validation_generation;
	if (anx_memplane_set_validation(entries[0], (enum anx_mem_validation_state)-1) != ANX_EINVAL ||
	    entries[0]->validation_generation != epoch) goto out;
	ret = ANX_OK;
out:
	anx_external_unregister_handler("anxresearch048");
	if (caller) anx_cell_destroy(caller);
	anx_free(call);
	if (!anx_uuid_is_nil(&workflow)) anx_wf_destroy(&workflow);
	if (!anx_uuid_is_nil(&manifest)) anx_so_delete(&manifest, false);
	if (!anx_uuid_is_nil(&second)) anx_so_delete(&second, false);
	if (!anx_uuid_is_nil(&checkpoint)) anx_so_delete(&checkpoint, false);
	for (uint32_t i = 0; i < 4; i++) {
		if (entries[i]) anx_memplane_forget(entries[i], ANX_FORGET_HARD_DELETE);
		if (objects[i]) { anx_so_delete(&objects[i]->oid, false); anx_objstore_release(objects[i]); }
	}
	anx_free(spec);
	return ret;
}
#endif
