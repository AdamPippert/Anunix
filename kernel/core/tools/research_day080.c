#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/state_object.h>
#include <anx/epistemic.h>
#include <anx/effect_fence.h>
#include <anx/external_call.h>
#include <anx/cell.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
struct fixture080 {
	struct anx_object_handle handles[4], *batch[4];
	struct anx_hash hashes[4];
	uint64_t versions[4];
	struct anx_external_call call;
	bool foreign;
};
static const char *replacement080[4] = { "new0", "new1", "new2", "new3" };
static bool unchanged080(struct fixture080 *f, bool staged)
{
	for (uint32_t i = 0; i < 4; i++) {
		struct anx_state_object *o = f->handles[i].obj;
		if (!!o->staged != staged || o->version != f->versions[i] || o->payload_size != 4 ||
		    anx_memcmp(o->payload, "old!", 4) || anx_memcmp(&o->content_hash, &f->hashes[i], sizeof(f->hashes[i]))) return false;
	}
	return true;
}
static int stage080(struct fixture080 *f, anx_cid_t owner)
{
	for (uint32_t i = 0; i < 4; i++) {
		int ret = anx_object_stage(&f->handles[i], owner);
		if (ret == ANX_OK) ret = anx_so_replace_payload(&f->handles[i], replacement080[i], 4);
		if (ret != ANX_OK) return ret;
	}
	return ANX_OK;
}
static int active080(struct anx_external_call *call, void *arg)
{
	struct fixture080 *f = arg; (void)call;
	if (f->foreign) return anx_object_commit_batch(f->batch, 4) == ANX_EPERM &&
		anx_object_abort_batch(f->batch, 4) == ANX_EPERM && unchanged080(f, true) ? ANX_OK : -8010;
	if (anx_object_commit_batch(f->batch, 4) != ANX_OK) return -8011;
	for (uint32_t i = 0; i < 4; i++) {
		struct anx_state_object *o = f->handles[i].obj;
		if (o->staged || o->version != f->versions[i] + 1 || o->payload_size != 4 ||
		    anx_memcmp(o->payload, replacement080[i], 4) ||
		    !anx_memcmp(&o->content_hash, &f->hashes[i], sizeof(f->hashes[i])) ||
		    o->provenance->events[o->provenance->count - 1].event_type != ANX_PROV_MUTATED) return -8012;
	}
	return anx_object_commit_batch(f->batch, 4) == ANX_EINVAL &&
		anx_object_abort_batch(f->batch, 4) == ANX_EINVAL ? ANX_OK : -8013;
}
int anx_research_day080(void)
{
	struct fixture080 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *objects[4] = {0};
	struct anx_effect_fence_view fence;
	struct anx_epistemic_view review = {0};
	struct anx_so_create_params params = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "old!", .payload_size = 4 };
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-080", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	ret = anx_effect_fence_create(&fence);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(owner, &fence.id);
	if (ret != ANX_OK) goto out;
	for (uint32_t i = 0; i < 4; i++) {
		ret = anx_so_create(&params, &objects[i]);
		if (ret == ANX_OK) ret = anx_so_open(&objects[i]->oid, ANX_OPEN_READWRITE, &f->handles[i]);
		if (ret != ANX_OK) goto out;
		f->versions[i] = objects[i]->version; f->hashes[i] = objects[i]->content_hash;
		f->batch[3-i] = &f->handles[i];
	}
	ret = stage080(f, owner->cid);
	if (ret != ANX_OK) goto out;
	ret = -8001;
	if (anx_object_abort_batch(f->batch, 4) != ANX_OK) goto out;
	if (!unchanged080(f, false)) { ret = -8002; goto out; }
	for (uint32_t i = 0; i < 4; i++) if (objects[i]->provenance->events[objects[i]->provenance->count - 1].event_type != ANX_PROV_STAGE_ABORTED) {
		ret = -8002; goto out;
	}
	ret = stage080(f, owner->cid);
	if (ret != ANX_OK) goto out;
	if (anx_object_commit_batch(f->batch, 0) != ANX_EINVAL || anx_object_abort_batch(f->batch, 5) != ANX_EINVAL ||
	    anx_object_commit_batch(NULL, 4) != ANX_EINVAL || !unchanged080(f, true)) { ret = -8003; goto out; }
	struct anx_object_handle read = f->handles[0]; read.mode = ANX_OPEN_READ;
	f->batch[3] = &read;
	ret = anx_object_commit_batch(f->batch, 4);
	int abort_ret = anx_object_abort_batch(f->batch, 4);
	f->batch[3] = &f->handles[0];
	if (ret != ANX_EPERM || abort_ret != ANX_EPERM || !unchanged080(f, true)) { ret = -8003; goto out; }
	f->batch[3] = f->batch[0]; ret = anx_object_commit_batch(f->batch, 4);
	abort_ret = anx_object_abort_batch(f->batch, 4); f->batch[3] = &f->handles[0];
	if (ret != ANX_EEXIST || abort_ret != ANX_EEXIST || !unchanged080(f, true)) { ret = -8003; goto out; }
	ret = anx_object_abort(&f->handles[3]);
	if (ret == ANX_OK) ret = anx_object_stage(&f->handles[3], foreign->cid);
	if (ret != ANX_OK) goto out;
	if (anx_object_commit_batch(f->batch, 4) != ANX_EPERM || anx_object_abort_batch(f->batch, 4) != ANX_EPERM ||
	    !unchanged080(f, true)) { ret = -8004; goto out; }
	ret = anx_object_abort(&f->handles[3]);
	if (ret == ANX_OK) ret = anx_object_stage(&f->handles[3], owner->cid);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&f->handles[3], replacement080[3], 4);
	if (ret != ANX_OK) goto out;
	objects[3]->version++; ret = anx_object_commit_batch(f->batch, 4); objects[3]->version--;
	if (ret != ANX_EBUSY || !unchanged080(f, true)) { ret = -8005; goto out; }
	objects[3]->access_policy.rule_count = 1;
	objects[3]->access_policy.rules[0] = (struct anx_access_rule){ .operations = ANX_ACCESS_WRITE_PAYLOAD, .effect = ANX_EFFECT_DENY };
	ret = anx_object_commit_batch(f->batch, 4); objects[3]->access_policy.rule_count = 0;
	if (ret != ANX_EPERM || !unchanged080(f, true)) { ret = -8006; goto out; }
	owner->execution.allow_side_effects = false; ret = anx_object_commit_batch(f->batch, 4); owner->execution.allow_side_effects = true;
	if (ret != ANX_EPERM || !unchanged080(f, true)) { ret = -8007; goto out; }
	ret = anx_effect_fence_hold(owner);
	if (ret != ANX_OK) goto out;
	if (anx_object_commit_batch(f->batch, 4) != ANX_EBUSY || !unchanged080(f, true)) { ret = -8008; goto out; }
	ret = anx_effect_fence_get(&fence.id, &fence);
	if (ret == ANX_OK) ret = anx_effect_fence_transition(&fence.id, fence.generation, ANX_FENCE_RUNNING);
	if (ret != ANX_OK) goto out;
	struct anx_epistemic_spec spec = { .count = 1, .threshold = 1, .minimum_cut = 1 };
	spec.reviewers[0] = foreign->cid;
	ret = anx_epistemic_begin(&f->handles[3], &spec, &review);
	if (ret != ANX_OK) goto out;
	if (anx_object_commit_batch(f->batch, 4) != ANX_EAUDIT || !unchanged080(f, true)) { ret = -8009; goto out; }
	ret = anx_object_abort_batch(f->batch, 4);
	if (ret == ANX_OK) ret = anx_epistemic_get(review.id, &review);
	if (ret != ANX_OK || review.state != ANX_EPISTEMIC_ABORTED || !unchanged080(f, false)) { ret = -8009; goto out; }
	ret = anx_epistemic_destroy(review.id); review.id = 0;
	if (ret == ANX_OK) ret = stage080(f, owner->cid);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch080", active080, f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint, "anxresearch080://commit", sizeof(f->call.endpoint));
	f->foreign = true; foreign->ext_call = &f->call; ret = anx_cell_run(foreign);
	if (ret == ANX_OK) { f->foreign = false; owner->ext_call = &f->call; ret = anx_cell_run(owner); }
	if (ret == ANX_OK) kprintf("day080 objects=4 abort_preserved=4 rejected_commits_preserved=4 published=4 versions=once\n");
out:
	anx_external_unregister_handler("anxresearch080");
	for (uint32_t i = 0; i < 4; i++) if (f->handles[i].obj) {
		if (f->handles[i].obj->staged) anx_object_abort(&f->handles[i]);
		anx_so_close(&f->handles[i]);
	}
	if (review.id) anx_epistemic_destroy(review.id);
	for (uint32_t i = 0; i < 4; i++) if (objects[i]) { anx_so_delete(&objects[i]->oid, false); anx_objstore_release(objects[i]); }
	if (foreign) anx_cell_destroy(foreign);
	if (owner) anx_cell_destroy(owner);
	if (ret != ANX_OK) kprintf("day080 native failure rc=%d\n", ret);
	anx_free(f); return ret;
}
#endif
