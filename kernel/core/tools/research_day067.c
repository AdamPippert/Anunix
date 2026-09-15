/* Replace native capability workers while preserving recorded completions. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/continuation.h>
#include <anx/external_operation.h>
#include <anx/effect_fence.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
struct fixture067 {
	struct anx_continuation_view view, output, sentinel;
	struct anx_continuation_result result, result_sentinel;
	struct anx_external_call request, active_call;
	anx_oid_t source;
	uint32_t calls;
	bool foreign, fail_provider;
};
static int provider067(struct anx_external_call *call, void *arg)
{
	struct fixture067 *f = arg;
	f->calls++;
	if (call->request_size != 4 || anx_memcmp(call->request_body, "data", 4)) return -6702;
	if (anx_continuation_get(f->view.id, &f->output) != ANX_EPERM ||
	    anx_continuation_dispatch(f->view.id, f->view.epoch, 0, 999, &f->request, "research-day-067", &f->source, &f->output) != ANX_EPERM ||
	    anx_continuation_bind(f->view.id, f->view.epoch, 0, anx_cell_current_id(), &f->output) != ANX_EPERM ||
	    anx_continuation_test_drop_reply(true) != ANX_EPERM) return -6703;
	anx_strlcpy(f->active_call.endpoint, "anxcontinuation://dispatch", sizeof(f->active_call.endpoint));
	if (anx_external_invoke(&f->active_call) != ANX_EBUSY) return -6703;
	if (f->fail_provider) return ANX_ETIMEDOUT;
	anx_memcpy(call->response_buf, "done", 4); call->response_size = 4; call->status_code = 200;
	return ANX_OK;
}
static void sentinel067(struct fixture067 *f)
{
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	anx_memset(&f->result, 0x55, sizeof(f->result)); f->result_sentinel = f->result;
}
static int active067(struct anx_external_call *call, void *arg)
{
	struct fixture067 *f = arg;
	(void)call;
	sentinel067(f);
	if (anx_continuation_create(&f->view.owner, &f->output) != ANX_EPERM ||
	    anx_continuation_destroy(f->view.id) != ANX_EPERM) return -6704;
	if (f->foreign) {
		struct anx_object_handle handle = {0};
		if (anx_so_open(&f->view.head, ANX_OPEN_READ, &handle) != ANX_EPERM) { anx_so_close(&handle); return -6705; }
		return anx_continuation_get(f->view.id, &f->output) == ANX_EPERM &&
			anx_continuation_read(f->view.id, 1, &f->result) == ANX_EPERM &&
			!anx_memcmp(&f->output, &f->sentinel, sizeof(f->output)) &&
			!anx_memcmp(&f->result, &f->result_sentinel, sizeof(f->result)) ? ANX_OK : -6705;
	}
	return anx_continuation_get(f->view.id, &f->output) == ANX_OK && f->output.completed == 3 &&
		anx_continuation_read(f->view.id, 1, &f->result) == ANX_OK && f->result.size == 4 &&
		!anx_memcmp(f->result.bytes, "done", 4) ? ANX_OK : -6706;
}
static int dispatch067(struct fixture067 *f, uint32_t slot, uint64_t key)
{
	return anx_continuation_dispatch(f->view.id, f->view.epoch, slot, key, &f->request,
		"research-day-067", &f->source, &f->view);
}
/* An uncertain operation deliberately retains its key and authority references. */
static int uncertain067(struct fixture067 *f)
{
	struct anx_cell *owner = NULL, *worker = NULL, *replacement = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_continuation_view healthy = f->view, failed = {0};
	uint32_t before = f->calls;
	int ret;
	anx_strlcpy(intent.name, "research-day-067-uncertain", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret != ANX_OK) return ret;
	owner->execution.allow_recursive_cells = owner->execution.allow_side_effects = true;
	ret = anx_cell_derive_child(owner, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &worker);
	if (ret == ANX_OK) ret = anx_cell_derive_child(owner, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &replacement);
	if (ret == ANX_OK) ret = anx_continuation_create(&owner->cid, &failed);
	if (ret == ANX_OK) ret = anx_continuation_bind(failed.id, failed.epoch, 0, &worker->cid, &failed);
	if (ret != ANX_OK) goto out;
	f->view = failed; f->fail_provider = true;
	ret = dispatch067(f, 0, 1); f->fail_provider = false;
	failed = f->view;
	if (ret != ANX_ETIMEDOUT || f->calls != before + 1 || failed.uncertain != 1 || failed.completed || worker->status != ANX_CELL_FAILED) { ret = -6718; goto out; }
	ret = anx_continuation_bind(failed.id, failed.epoch, 0, &replacement->cid, &failed);
	if (ret != ANX_OK) goto out;
	f->view = failed;
	ret = -6719;
	if (dispatch067(f, 0, 1) != ANX_EEXIST || f->calls != before + 1 ||
	    anx_continuation_read(failed.id, 1, &f->result) != ANX_EBUSY ||
	    anx_continuation_destroy(failed.id) != ANX_EBUSY || replacement->status != ANX_CELL_CREATED) goto out;
	if (anx_continuation_get(healthy.id, &f->output) != ANX_OK || f->output.completed != healthy.completed ||
	    anx_continuation_read(healthy.id, 1, &f->result) != ANX_OK || anx_memcmp(f->result.bytes, "done", 4)) goto out;
	ret = ANX_OK;
out:
	f->view = healthy; f->fail_provider = false;
	/* Uncertain records remain inspectable; completed local workers need no pin. */
	if (worker) { if (anx_cell_destroy(worker) != ANX_OK) anx_cell_store_release(worker); }
	if (replacement) anx_cell_store_release(replacement);
	if (owner) anx_cell_store_release(owner);
	return ret;
}
int anx_research_day067(void)
{
	struct fixture067 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL, *workers[4] = {0};
	struct anx_cell_intent intent = {0};
	struct anx_state_object *source = NULL, *object = NULL;
	struct anx_continuation_event event;
	struct anx_effect_fence_view fence;
	struct anx_so_create_params params = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "data", .payload_size = 4 };
	anx_oid_t retained[ANX_CONTINUATION_EVENTS * 2]; uint32_t retained_count = 0;
	uint64_t original_epoch;
	bool keep_source = false;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-067", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret == ANX_OK) ret = anx_so_create(&params, &source);
	if (ret == ANX_OK) ret = anx_so_seal(&source->oid);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_recursive_cells = owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	ret = anx_effect_fence_create(&fence);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(owner, &fence.id);
	if (ret != ANX_OK) goto out;
	ret = -6701;
	if (anx_continuation_create(&owner->cid, &f->view) != ANX_OK) goto out;
	f->source = source->oid;
	for (uint32_t i = 0; i < 4; i++) {
		ret = anx_cell_derive_child(owner, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &workers[i]);
		if (ret != ANX_OK) goto out;
	}
	ret = anx_sink_register("research-day-067", ANX_SENSITIVITY_PUBLIC, NULL);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch067", provider067, f);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch067active", active067, f);
	if (ret == ANX_OK) ret = anx_continuation_bind(f->view.id, f->view.epoch, 0, &workers[0]->cid, &f->view);
	if (ret == ANX_OK) ret = anx_continuation_bind(f->view.id, f->view.epoch, 1, &workers[1]->cid, &f->view);
	if (ret != ANX_OK) goto out;
	original_epoch = f->view.epoch;
	sentinel067(f); ret = -6707;
	if (anx_continuation_bind(f->view.id, f->view.epoch + 1, 0, &workers[2]->cid, &f->output) != ANX_EBUSY ||
	    anx_continuation_bind(f->view.id, f->view.epoch, 0, &workers[2]->cid, &f->output) != ANX_EBUSY ||
	    anx_continuation_bind(f->view.id, f->view.epoch, 2, &workers[0]->cid, &f->output) != ANX_EEXIST ||
	    anx_continuation_bind(f->view.id, f->view.epoch, 2, &foreign->cid, &f->output) != ANX_EPERM ||
	    anx_memcmp(&f->output, &f->sentinel, sizeof(f->output)) || anx_cell_destroy(workers[0]) != ANX_EBUSY) goto out;
	anx_strlcpy(f->request.endpoint, "anxresearch067://effect", sizeof(f->request.endpoint));
	f->request.request_body = "data"; f->request.request_size = 4;
	/* A widened parent policy does not change the continuation's captured authority. */
	owner->execution.allow_network = true;
	ret = dispatch067(f, 0, 1); owner->execution.allow_network = false;
	if (ret != ANX_EBUSY || f->calls) { ret = -6708; goto out; }
	workers[0]->execution.allow_network = true;
	ret = dispatch067(f, 0, 1); workers[0]->execution.allow_network = false;
	if (ret != ANX_EPERM || f->calls) { ret = -6708; goto out; }
	f->request.request_body = "evil";
	ret = dispatch067(f, 0, 1); f->request.request_body = "data";
	if (ret != ANX_EINVAL || f->calls) { ret = -6708; goto out; }
	ret = anx_continuation_test_drop_reply(true);
	if (ret != ANX_OK) goto out;
	sentinel067(f);
	ret = anx_continuation_dispatch(f->view.id, f->view.epoch, 0, 1, &f->request, "research-day-067", &f->source, &f->output);
	if (ret != ANX_ETIMEDOUT || f->calls != 1 || workers[0]->status != ANX_CELL_COMPLETED ||
	    anx_memcmp(&f->output, &f->sentinel, sizeof(f->output))) { ret = -6709; goto out; }
	ret = anx_continuation_get(f->view.id, &f->view);
	if (ret != ANX_OK) goto out;
	ret = -6710;
	if (f->view.completed != 1 || f->view.events != 4 || f->view.operations != 1 || f->view.uncertain ||
	    anx_continuation_event_get(f->view.id, 3, &event) != ANX_OK || event.kind != ANX_CONT_COMMITTED || event.key != 1) goto out;
	anx_cid_t departed = workers[0]->cid;
	ret = anx_continuation_bind(f->view.id, f->view.epoch, 0, &workers[2]->cid, &f->view);
	if (ret == ANX_OK) ret = anx_cell_destroy(workers[0]);
	if (ret != ANX_OK) goto out;
	workers[0] = NULL;
	struct anx_cell *gone = anx_cell_store_lookup(&departed);
	if (gone) { anx_cell_store_release(gone); ret = -6711; goto out; }
	ret = -6711;
	if (f->view.generations[0] != 2 || anx_uuid_compare(&f->view.owner, &owner->cid) ||
	    anx_continuation_read(f->view.id, 1, &f->result) != ANX_OK || f->result.size != 4 ||
	    f->result.status_code != 200 || anx_memcmp(f->result.bytes, "done", 4)) goto out;
	if (dispatch067(f, 0, 1) != ANX_EEXIST || f->calls != 1 || workers[2]->status != ANX_CELL_CREATED ||
	    anx_continuation_dispatch(f->view.id, original_epoch, 0, 2, &f->request, "research-day-067", &f->source, &f->output) != ANX_EBUSY) goto out;
	/* A failed idle peer can be replaced without disturbing the completed call. */
	ret = anx_cell_transition(workers[1], ANX_CELL_FAILED);
	if (ret == ANX_OK) ret = anx_continuation_bind(f->view.id, f->view.epoch, 1, &workers[3]->cid, &f->view);
	if (ret == ANX_OK) ret = anx_cell_destroy(workers[1]);
	if (ret != ANX_OK) goto out;
	workers[1] = NULL;
	ret = dispatch067(f, 0, 2);
	if (ret == ANX_OK) ret = dispatch067(f, 1, 3);
	if (ret != ANX_OK) goto out;
	if (f->calls != 3 || f->view.completed != 3 || owner->execution.allow_network || owner->execution.allow_remote_models) { ret = -6712; goto out; }
	/* Changed evidence or access blocks projection and output without modifying the caller. */
	object = anx_objstore_lookup(&f->view.head);
	if (!object) { ret = ANX_ENOENT; goto out; }
	sentinel067(f);
	object->version++; ret = anx_continuation_get(f->view.id, &f->output); object->version--;
	if (ret != ANX_EBUSY || anx_memcmp(&f->output, &f->sentinel, sizeof(f->output))) { ret = -6713; goto out; }
	((uint8_t *)object->payload)[0] ^= 1; ret = anx_continuation_get(f->view.id, &f->output); ((uint8_t *)object->payload)[0] ^= 1;
	if (ret != ANX_EBUSY) { ret = -6713; goto out; }
	object->access_policy.rules[0].effect = ANX_EFFECT_DENY; ret = anx_continuation_get(f->view.id, &f->output);
	object->access_policy.rules[0].effect = ANX_EFFECT_ALLOW;
	if (ret != ANX_EPERM) { ret = -6713; goto out; }
	anx_objstore_release(object); object = NULL;
	for (uint32_t mode = 0; mode < 3; mode++) {
		if (mode == 0) source->version++;
		if (mode == 1) ((uint8_t *)source->payload)[0] ^= 1;
		if (mode == 2) { source->access_policy.rule_count = 1; source->access_policy.rules[0] = (struct anx_access_rule){ .operations = ANX_ACCESS_READ_PAYLOAD, .effect = ANX_EFFECT_DENY }; }
		ret = anx_continuation_read(f->view.id, 1, &f->result);
		if (mode == 0) source->version--;
		if (mode == 1) ((uint8_t *)source->payload)[0] ^= 1;
		if (mode == 2) source->access_policy.rule_count = 0;
		if (ret != (mode == 2 ? ANX_EPERM : ANX_EBUSY) || anx_memcmp(&f->result, &f->result_sentinel, sizeof(f->result))) { ret = -6714; goto out; }
	}
	keep_source = true;
	ret = uncertain067(f);
	if (ret != ANX_OK) goto out;
	ret = -6715;
	if (f->calls != 4 || anx_continuation_get(f->view.id, &f->output) != ANX_OK || f->output.completed != 3) goto out;
	/* Collect all sealed objects before the owner's final runtime invocation. */
	retained[retained_count++] = f->view.head;
	for (uint32_t i = 0; i < f->view.events; i++) {
		ret = anx_continuation_event_get(f->view.id, i, &event);
		if (ret != ANX_OK) goto out;
		if (i) retained[retained_count++] = event.previous;
		if (!anx_uuid_is_nil(&event.result_object)) retained[retained_count++] = event.result_object;
	}
	anx_strlcpy(f->active_call.endpoint, "anxresearch067active://inspect", sizeof(f->active_call.endpoint));
	foreign->ext_call = &f->active_call; f->foreign = true;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	owner->ext_call = &f->active_call; f->foreign = false;
	ret = anx_cell_run(owner);
	if (ret != ANX_OK) goto out;
	ret = anx_continuation_destroy(f->view.id);
	if (ret == ANX_OK) f->view.id = 0;
out:
	anx_continuation_test_drop_reply(false);
	anx_external_unregister_handler("anxresearch067"); anx_external_unregister_handler("anxresearch067active");
	if (object) anx_objstore_release(object);
	if (f->view.id) anx_continuation_destroy(f->view.id);
	for (uint32_t i = 0; i < retained_count; i++) anx_so_delete(&retained[i], false);
	for (uint32_t i = 0; i < 4; i++) if (workers[i]) anx_cell_destroy(workers[i]);
	if (source) { anx_oid_t oid = source->oid; anx_objstore_release(source); if (!keep_source) anx_so_delete(&oid, false); }
	if (foreign) anx_cell_destroy(foreign);
	if (owner) anx_cell_destroy(owner);
	if (ret != ANX_OK) kprintf("day067 native failure rc=%d calls=%u\n", ret, f->calls);
	anx_free(f); return ret;
}
#endif
