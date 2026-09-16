/* Semantic progress remains live while reconstructible acceleration state is absent. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/continuation.h>
#include <anx/capability.h>
#include <anx/phase.h>
#include <anx/model_use.h>
#include <anx/state_object.h>
#include <anx/effect_fence.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
struct fixture068 {
	struct anx_continuation_view view, output, sentinel;
	struct anx_continuation_result result;
	struct anx_adapter_image image, original;
	struct anx_anxml_request inference;
	struct anx_anxml_response response;
	struct anx_external_call request, call;
	anx_oid_t source;
	uint32_t calls;
	bool foreign;
};
static int provider068(struct anx_external_call *call, void *arg)
{
	struct fixture068 *f = arg; f->calls++;
	if (call->request_size != 4 || anx_memcmp(call->request_body, "data", 4)) return -6802;
	anx_memcpy(call->response_buf, "kept", 4); call->response_size = 4; return ANX_OK;
}
static int materialize068(struct fixture068 *f)
{
	int ret = anx_continuation_acceleration_read(f->view.id, f->view.epoch, f->view.cache_generation, &f->image);
	uint8_t digest[32]; anx_sha256(&f->original, sizeof(f->original), digest);
	if (ret == ANX_OK) ret = anx_anxml_generate_verified(&f->inference, &f->image, digest, &f->response);
	return ret == ANX_OK && f->response.output_len == 4 && !anx_memcmp(f->response.output, "AAAA", 4) ? ANX_OK : -6803;
}
static int active068(struct anx_external_call *call, void *arg)
{
	struct fixture068 *f = arg; (void)call;
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	if (anx_continuation_pause(f->view.id, f->view.epoch, ANX_SUSPEND_HUMAN_APPROVAL, &f->output) != ANX_EPERM ||
	    anx_continuation_hibernate(f->view.id, f->view.epoch, &f->output) != ANX_EPERM ||
	    anx_continuation_resume(f->view.id, f->view.epoch, &f->output) != ANX_EPERM ||
	    anx_continuation_suspend_configure(f->view.id, f->view.epoch, f->view.phase_epoch, &f->source, &f->output) != ANX_EPERM ||
	    anx_continuation_test_cache_corrupt(f->view.id) != ANX_EPERM ||
	    anx_continuation_test_restore_fault(true) != ANX_EPERM ||
	    anx_memcmp(&f->output, &f->sentinel, sizeof(f->output))) return -6804;
	if (f->foreign) return anx_continuation_acceleration_read(f->view.id, f->view.epoch, f->view.cache_generation, &f->image) == ANX_EPERM ? ANX_OK : -6805;
	return anx_continuation_read(f->view.id, 1, &f->result) == ANX_OK && f->result.size == 4 &&
		!anx_memcmp(f->result.bytes, "kept", 4) ? materialize068(f) : -6805;
}
static int denied068(struct fixture068 *f, int expected)
{
	struct anx_continuation_view current;
	struct anx_phase_view before, after;
	struct anx_resource_pool_stats cache_before, cache_after;
	uint64_t total, free_before, free_after;
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	int ret = anx_phase_get(&f->view.owner, &before);
	if (ret != ANX_OK) return ret;
	ret = anx_continuation_cache_stats(f->view.id, &cache_before);
	if (ret != ANX_OK) return ret;
	anx_page_stats(&total, &free_before);
	ret = anx_continuation_resume(f->view.id, f->view.epoch, &f->output);
	anx_page_stats(&total, &free_after);
	if (ret != expected) { kprintf("day068 resume expected=%d actual=%d\n", expected, ret); return -6806; }
	int view_ret = anx_continuation_get(f->view.id, &current), phase_ret = anx_phase_get(&f->view.owner, &after);
	bool view_same = view_ret == ANX_OK && !anx_memcmp(&current, &f->view, sizeof(current));
	bool phase_same = phase_ret == ANX_OK && !anx_memcmp(&before, &after, sizeof(before));
	bool cache_same = anx_continuation_cache_stats(f->view.id, &cache_after) == ANX_OK && !anx_memcmp(&cache_before, &cache_after, sizeof(cache_before));
	bool output_same = !anx_memcmp(&f->output, &f->sentinel, sizeof(f->output));
	if (!view_same || !phase_same || !cache_same || free_after != free_before || !output_same) {
		kprintf("day068 unchanged expected=%d state=%u view_ret=%d view_same=%u phase_ret=%d phase_same=%u pages_before=%llu pages_after=%llu output_same=%u\n",
			expected, (uint32_t)f->view.resource_state, view_ret, view_same, phase_ret, phase_same,
			(unsigned long long)free_before, (unsigned long long)free_after, output_same);
		return -6807;
	}
	return ANX_OK;
}
static int dispatch068(struct fixture068 *f, uint64_t key, struct anx_continuation_view *out)
{
	return anx_continuation_dispatch(f->view.id, f->view.epoch, 1, key, &f->request, "research-day-068", &f->source, out);
}
int anx_research_day068(void)
{
	struct fixture068 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL, *workers[2] = {0};
	struct anx_cell_intent intent = {0};
	struct anx_phase_view phase, initial;
	struct anx_resource_pool_stats cache;
	struct anx_phase_contract contract = { .role = ANX_ROLE_RUNNER };
	struct anx_phase_request request = { ANX_PHASE_INFERENCE, 4096, 25 };
	struct anx_state_object *model = NULL, *source = NULL, *result_object = NULL;
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA, .schema_uri = ANX_MODEL_USE_SCHEMA, .schema_version = "1" };
	struct anx_engine_lease *lease = NULL, *rival = NULL;
	struct anx_effect_fence_view fence;
	struct anx_continuation_event event;
	anx_oid_t retained[ANX_CONTINUATION_EVENTS * 2], semantic; uint32_t retained_count = 0;
	anx_eid_t rival_id;
	uint64_t memory_before, memory_now, total, free_before, free_after;
	uint32_t pct_before, pct_now;
	bool attached = false;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	f->original = (struct anx_adapter_image){ .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	f->inference.prompt[0] = '~'; f->inference.prompt_len = 1; f->inference.max_tokens = 4;
	params.payload = &f->original; params.payload_size = sizeof(f->original);
	anx_lease_avail_mem(ANX_MEM_L1, &memory_before); anx_lease_avail_accel(ANX_ACCEL_GPU, &pct_before);
	anx_strlcpy(intent.name, "research-day-068", sizeof(intent.name));
	contract.limits[ANX_PHASE_INFERENCE] = (struct anx_phase_limit){ true, ANX_MEM_L1, 4096, ANX_ACCEL_GPU, 25 };
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_recursive_cells = owner->execution.allow_side_effects = true;
	ret = anx_effect_fence_create(&fence);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(owner, &fence.id);
	if (ret == ANX_OK) ret = anx_continuation_create(&owner->cid, &f->view);
	if (ret == ANX_OK) ret = anx_so_create(&params, &model);
	if (ret == ANX_OK) ret = anx_so_seal(&model->oid);
	params.object_type = ANX_OBJ_BYTE_DATA; params.schema_uri = NULL; params.schema_version = NULL; params.payload = "data"; params.payload_size = 4;
	if (ret == ANX_OK) ret = anx_so_create(&params, &source);
	if (ret == ANX_OK) ret = anx_so_seal(&source->oid);
	if (ret == ANX_OK) ret = anx_phase_attach(&owner->cid, &contract);
	if (ret != ANX_OK) goto out;
	attached = true; f->source = source->oid;
	ret = anx_phase_get(&owner->cid, &phase);
	if (ret == ANX_OK) ret = anx_phase_begin(&owner->cid, phase.epoch, &request);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &phase);
	if (ret != ANX_OK) goto out;
	initial = phase;
	for (uint32_t i = 0; i < 2; i++) {
		ret = anx_cell_derive_child(owner, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &workers[i]);
		if (ret == ANX_OK) ret = anx_continuation_bind(f->view.id, f->view.epoch, i, &workers[i]->cid, &f->view);
		if (ret != ANX_OK) goto out;
	}
	anx_strlcpy(f->request.endpoint, "anxresearch068://effect", sizeof(f->request.endpoint));
	f->request.request_body = "data"; f->request.request_size = 4;
	ret = anx_sink_register("research-day-068", ANX_SENSITIVITY_PUBLIC, NULL);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch068", provider068, f);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch068active", active068, f);
	if (ret == ANX_OK) ret = anx_continuation_dispatch(f->view.id, f->view.epoch, 0, 1, &f->request, "research-day-068", &f->source, &f->view);
	if (ret != ANX_OK) goto out;
	semantic = f->view.semantic_checkpoint;
	ret = anx_continuation_event_get(f->view.id, 3, &event);
	if (ret == ANX_OK) result_object = anx_objstore_lookup(&event.result_object);
	if (ret != ANX_OK || !result_object) { ret = -6808; goto out; }
	ret = -6801;
	if (anx_continuation_suspend_configure(f->view.id, f->view.epoch, phase.epoch, &model->oid, &f->view) != ANX_OK) goto out;
	ret = -6808;
	if (anx_continuation_cache_stats(f->view.id, &cache) != ANX_OK || cache.physical_pages != 1 || cache.live_records != 1 || cache.live_bytes != sizeof(f->original) ||
	    f->view.physical_pages != 1 || f->view.cache_generation != 1 || anx_uuid_compare(&semantic, &f->view.semantic_checkpoint)) goto out;
	ret = materialize068(f);
	if (ret == ANX_OK) ret = anx_continuation_pause(f->view.id, f->view.epoch, ANX_SUSPEND_TOOL_WAIT, &f->view);
	if (ret != ANX_OK) goto out;
	anx_lease_avail_mem(ANX_MEM_L1, &memory_now); anx_lease_avail_accel(ANX_ACCEL_GPU, &pct_now);
	ret = -6809;
	if (f->view.resource_state != ANX_CONT_SHORT_WAIT || memory_now != memory_before - 4096 || pct_now != pct_before - 25 ||
	    f->view.physical_pages != 1 || f->view.phase_epoch != initial.epoch || dispatch068(f, 2, &f->output) != ANX_EBUSY || f->calls != 1 ||
	    anx_continuation_acceleration_read(f->view.id, f->view.epoch, 1, &f->image) != ANX_EBUSY) goto out;
	ret = anx_continuation_resume(f->view.id, f->view.epoch, &f->view);
	if (ret != ANX_OK) goto out;
	if (f->view.cache_generation != 1 || f->view.phase_epoch != initial.epoch) { ret = -6810; goto out; }
	ret = anx_continuation_pause(f->view.id, f->view.epoch, ANX_SUSPEND_TOOL_WAIT, &f->view);
	if (ret != ANX_OK) goto out;
	lease = anx_lease_lookup(&initial.lease_id);
	if (!lease) { ret = ANX_ENOENT; goto out; }
	lease->mem_used_bytes = 1;
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	ret = anx_continuation_pause(f->view.id, f->view.epoch, ANX_SUSPEND_HUMAN_APPROVAL, &f->output);
	lease->mem_used_bytes = 0;
	if (ret != ANX_EBUSY || anx_memcmp(&f->output, &f->sentinel, sizeof(f->output))) { ret = -6811; goto out; }
	ret = anx_continuation_pause(f->view.id, f->view.epoch, ANX_SUSPEND_HUMAN_APPROVAL, &f->view);
	lease = NULL;
	if (ret != ANX_OK) goto out;
	anx_lease_avail_mem(ANX_MEM_L1, &memory_now); anx_lease_avail_accel(ANX_ACCEL_GPU, &pct_now);
	ret = -6812;
	if (f->view.resource_state != ANX_CONT_SUSPENDED || f->view.physical_pages != 1 || memory_now != memory_before || pct_now != pct_before ||
	    anx_phase_get(&owner->cid, &phase) != ANX_OK || !phase.parked || anx_lease_lookup(&initial.lease_id)) goto out;
	ret = anx_continuation_test_cache_corrupt(f->view.id);
	if (ret == ANX_OK) ret = denied068(f, ANX_EIO);
	if (ret != ANX_OK) goto out;
	anx_page_stats(&total, &free_before);
	ret = anx_continuation_hibernate(f->view.id, f->view.epoch, &f->view);
	anx_page_stats(&total, &free_after);
	if (ret != ANX_OK) goto out;
	ret = -6813;
	int cache_ret = anx_continuation_cache_stats(f->view.id, &cache);
	int result_ret = anx_continuation_read(f->view.id, 1, &f->result);
	kprintf("day068 hibernate state=%u pages=%u free_before=%llu free_after=%llu cache_ret=%d cache_pages=%u records=%u bytes=%llu checkpoint_same=%u completed=%u result_ret=%d result_same=%u\n",
		(uint32_t)f->view.resource_state, f->view.physical_pages, (unsigned long long)free_before, (unsigned long long)free_after,
		cache_ret, cache.physical_pages, cache.live_records, (unsigned long long)cache.live_bytes,
		!anx_uuid_compare(&semantic, &f->view.semantic_checkpoint), f->view.completed, result_ret, !anx_memcmp(f->result.bytes, "kept", 4));
	/* The sealed transition event has separate object-store allocations. */
	if (f->view.resource_state != ANX_CONT_HIBERNATED || f->view.physical_pages ||
	    cache_ret != ANX_OK || cache.physical_pages || cache.live_records || cache.live_bytes ||
	    anx_uuid_compare(&semantic, &f->view.semantic_checkpoint) || f->view.completed != 1 ||
	    result_ret != ANX_OK || anx_memcmp(f->result.bytes, "kept", 4)) goto out;
	model->version++; ret = denied068(f, ANX_EBUSY); model->version--;
	if (ret != ANX_OK) goto out;
	((uint8_t *)model->payload)[0] ^= 1; ret = denied068(f, ANX_EBUSY); ((uint8_t *)model->payload)[0] ^= 1;
	if (ret != ANX_OK) goto out;
	model->access_policy.rule_count = 1; model->access_policy.rules[0] = (struct anx_access_rule){ .operations = ANX_ACCESS_READ_PAYLOAD, .effect = ANX_EFFECT_DENY };
	ret = denied068(f, ANX_EPERM); model->access_policy.rule_count = 0;
	if (ret != ANX_OK) goto out;
	source->version++; ret = denied068(f, ANX_EBUSY); source->version--;
	if (ret != ANX_OK) goto out;
	((uint8_t *)result_object->payload)[0] ^= 1; ret = denied068(f, ANX_EBUSY); ((uint8_t *)result_object->payload)[0] ^= 1;
	if (ret != ANX_OK) goto out;
	owner->execution.allow_network = true;
	ret = anx_continuation_resume(f->view.id, f->view.epoch, &f->output); owner->execution.allow_network = false;
	if (ret != ANX_EBUSY || anx_memcmp(&f->output, &f->sentinel, sizeof(f->output))) { ret = -6814; goto out; }
	ret = anx_effect_fence_get(&fence.id, &fence);
	if (ret == ANX_OK) ret = anx_effect_fence_transition(&fence.id, fence.generation, ANX_FENCE_HELD);
	if (ret != ANX_OK) goto out;
	int denied = anx_continuation_resume(f->view.id, f->view.epoch, &f->output);
	ret = anx_effect_fence_get(&fence.id, &fence);
	if (ret == ANX_OK) ret = anx_effect_fence_transition(&fence.id, fence.generation, ANX_FENCE_RUNNING);
	if (ret != ANX_OK) goto out;
	if (denied != ANX_EBUSY) { ret = -6814; goto out; }
	anx_uuid_generate(&rival_id);
	ret = anx_lease_grant(&rival_id, ANX_MEM_L1, memory_before, ANX_ACCEL_NONE, 0, &rival);
	if (ret == ANX_OK) ret = denied068(f, ANX_ENOMEM);
	if (rival) { anx_lease_release(rival); rival = NULL; }
	if (ret != ANX_OK) goto out;
	ret = anx_lease_grant(&rival_id, ANX_MEM_L1, 0, ANX_ACCEL_GPU, pct_before, &rival);
	if (ret == ANX_OK) ret = denied068(f, ANX_ENOMEM);
	if (rival) { anx_lease_release(rival); rival = NULL; }
	if (ret != ANX_OK) goto out;
	ret = anx_continuation_test_restore_fault(true);
	if (ret == ANX_OK) ret = denied068(f, ANX_EIO);
	if (ret != ANX_OK) goto out;
	ret = anx_continuation_resume(f->view.id, f->view.epoch, &f->view);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &phase);
	if (ret != ANX_OK) goto out;
	ret = -6815;
	if (f->view.resource_state != ANX_CONT_RUNNABLE || f->view.physical_pages != 1 || f->view.cache_generation != 2 || phase.parked ||
	    !anx_uuid_compare(&phase.lease_id, &initial.lease_id) || phase.memory_bytes != 4096 || phase.accelerator_pct != 25 ||
	    anx_uuid_compare(&semantic, &f->view.semantic_checkpoint) ||
	    anx_continuation_acceleration_read(f->view.id, f->view.epoch, 1, &f->image) != ANX_EBUSY) goto out;
	ret = materialize068(f);
	if (ret == ANX_OK) ret = dispatch068(f, 2, &f->view);
	if (ret != ANX_OK) goto out;
	if (f->calls != 2 || f->view.completed != 2 || !anx_uuid_compare(&semantic, &f->view.semantic_checkpoint)) { ret = -6816; goto out; }
	retained[retained_count++] = f->view.head;
	for (uint32_t i = 0; i < f->view.events; i++) {
		ret = anx_continuation_event_get(f->view.id, i, &event);
		if (ret != ANX_OK) goto out;
		if (i) retained[retained_count++] = event.previous;
		if (!anx_uuid_is_nil(&event.result_object)) retained[retained_count++] = event.result_object;
	}
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint, "anxresearch068active://inspect", sizeof(f->call.endpoint));
	foreign->execution.allow_side_effects = true; foreign->ext_call = &f->call; f->foreign = true;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	owner->ext_call = &f->call; f->foreign = false;
	ret = anx_cell_run(owner);
out:
	anx_continuation_test_restore_fault(false);
	if (lease) lease->mem_used_bytes = 0;
	if (rival) anx_lease_release(rival);
	anx_external_unregister_handler("anxresearch068"); anx_external_unregister_handler("anxresearch068active");
	if (f->view.id) anx_continuation_destroy(f->view.id);
	if (attached) { anx_phase_get(&owner->cid, &phase); anx_phase_finish(&owner->cid, phase.epoch); anx_phase_detach(&owner->cid); }
	if (result_object) anx_objstore_release(result_object);
	for (uint32_t i = 0; i < retained_count; i++) anx_so_delete(&retained[i], false);
	for (uint32_t i = 0; i < 2; i++) if (workers[i]) anx_cell_destroy(workers[i]);
	if (source) { anx_oid_t oid = source->oid; anx_objstore_release(source); anx_so_delete(&oid, false); }
	if (model) { anx_oid_t oid = model->oid; anx_objstore_release(model); anx_so_delete(&oid, false); }
	if (foreign) anx_cell_destroy(foreign);
	if (owner) anx_cell_destroy(owner);
	if (ret != ANX_OK) kprintf("day068 native failure rc=%d calls=%u\n", ret, f->calls);
	anx_free(f); return ret;
}
#endif
