#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/frontier.h>
#include <anx/phase.h>
#include <anx/state_object.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/kprintf.h>
struct fixture069 {
	struct anx_frontier_view full, frontier, probe, output, sentinel;
	struct anx_frontier_spec spec;
	struct anx_model_use_view uses[6];
	struct anx_anxml_response response, saved;
	struct anx_external_call call;
	bool foreign;
};
static int denied069(struct fixture069 *f, struct anx_frontier_view *view, int expected)
{
	struct anx_frontier_view current;
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	anx_memset(&f->response, 0x55, sizeof(f->response)); f->saved = f->response;
	int ret = anx_frontier_step(view->id, view->epoch, &f->response, &f->output);
	if (ret != expected) { kprintf("day069 denied expected=%d actual=%d\n", expected, ret); return -6902; }
	return anx_frontier_get(view->id, &current) == ANX_OK && !anx_memcmp(view, &current, sizeof(current)) &&
		!anx_memcmp(&f->output, &f->sentinel, sizeof(f->output)) && !anx_memcmp(&f->response, &f->saved, sizeof(f->response)) ? ANX_OK : -6903;
}
static int active069(struct anx_external_call *call, void *arg)
{
	struct fixture069 *f = arg; (void)call;
	if (anx_frontier_restore(f->frontier.id, f->frontier.epoch, &f->output) != ANX_EPERM ||
	    anx_frontier_reclaim(f->frontier.id, f->frontier.epoch, &f->output) != ANX_EPERM ||
	    anx_frontier_destroy(f->frontier.id) != ANX_EPERM || anx_frontier_test_corrupt(f->frontier.id, 0) != ANX_EPERM) return -6910;
	if (f->foreign) return anx_frontier_get(f->frontier.id, &f->output) == ANX_EPERM &&
		anx_frontier_step(f->frontier.id, f->frontier.epoch, &f->response, &f->output) == ANX_EPERM &&
		anx_frontier_read(f->frontier.id, 0, &f->response) == ANX_EPERM ? ANX_OK : -6911;
	int ret = anx_frontier_step(f->frontier.id, f->frontier.epoch, &f->response, &f->frontier);
	if (ret != ANX_OK || f->frontier.completed != 3 || anx_memcmp(f->response.output, "BBBB", 4)) return -6912;
	return anx_frontier_read(f->frontier.id, 0, &f->response) == ANX_OK &&
		f->response.output_len == 4 && !anx_memcmp(f->response.output, "AAAA", 4) ? ANX_OK : -6913;
}
int anx_research_day069(void)
{
	struct fixture069 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *images[2] = {0}, *prompt = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA, .schema_uri = ANX_MODEL_USE_SCHEMA,
		.schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	struct anx_model_use_spec use = { .maximum_tokens = 4 };
	struct anx_phase_contract contract = { .role = ANX_ROLE_RUNNER };
	struct anx_phase_request request = { ANX_PHASE_INFERENCE, 4096, 0 };
	struct anx_phase_view phase;
	uint64_t total, before, after;
	bool attached = false;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-069", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_side_effects = true;
	for (uint32_t i = 0; i < 2; i++) {
		adapter.deltas[0].next = adapter.deltas[1].previous = adapter.deltas[1].next = (uint8_t)('A' + i);
		ret = anx_so_create(&params, &images[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&images[i]->oid);
		if (ret != ANX_OK) goto out;
	}
	params = (struct anx_so_create_params){ .object_type = ANX_OBJ_BYTE_DATA, .payload = "~", .payload_size = 1 };
	ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	use.prompt = prompt->oid;
	for (uint32_t i = 0; i < 6; i++) {
		use.image = images[i % 2]->oid;
		ret = anx_model_use_prepare(&owner->cid, &use, &f->uses[i]);
		if (ret != ANX_OK) goto out;
	}
	contract.limits[ANX_PHASE_INFERENCE] = (struct anx_phase_limit){ true, ANX_MEM_L1, 4096, ANX_ACCEL_NONE, 0 };
	ret = anx_phase_attach(&owner->cid, &contract); attached = ret == ANX_OK;
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &phase);
	if (ret == ANX_OK) ret = anx_phase_begin(&owner->cid, phase.epoch, &request);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &phase);
	if (ret != ANX_OK) goto out;
	f->spec = (struct anx_frontier_spec){ .phase_epoch = phase.epoch, .count = 2,
		.nodes = {{f->uses[4].id,0},{f->uses[5].id,1}} };
	ret = -6901;
	if (anx_frontier_create(&owner->cid, &f->spec, &f->probe) != ANX_OK) goto out;
	ret = anx_frontier_restore(f->probe.id, f->probe.epoch, &f->probe);
	if (ret == ANX_OK) ret = anx_frontier_restore(f->probe.id, f->probe.epoch, &f->probe);
	if (ret != ANX_OK) goto out;
	anx_page_stats(&total, &before);
	ret = anx_frontier_reclaim(f->probe.id, f->probe.epoch, &f->probe);
	anx_page_stats(&total, &after);
	if (ret != ANX_OK || after <= before || f->probe.resident != 1 || f->probe.physical_pages != 1 ||
	    anx_frontier_reclaim(f->probe.id, f->probe.epoch, &f->output) != ANX_EBUSY) { ret = -6904; goto out; }
	ret = anx_phase_resize(&owner->cid, phase.epoch, 2048, 0, &phase);
	if (ret != ANX_OK) goto out;
	ret = denied069(f, &f->probe, ANX_EBUSY);
	if (ret != ANX_OK) goto out;
	if (anx_frontier_restore(f->probe.id, f->probe.epoch, &f->output) != ANX_EBUSY) { ret = -6905; goto out; }
	ret = anx_frontier_destroy(f->probe.id); f->probe.id = 0;
	if (ret != ANX_OK) goto out;
	f->spec.phase_epoch = phase.epoch; f->spec.nodes[0].use = f->uses[0].id; f->spec.nodes[1].use = f->uses[1].id;
	f->spec.mode = ANX_FRONTIER_FULL;
	ret = anx_frontier_create(&owner->cid, &f->spec, &f->full);
	if (ret != ANX_OK) goto out;
	f->spec.nodes[1].dependencies = 2;
	if (anx_frontier_create(&owner->cid, &f->spec, &f->output) != ANX_EINVAL) { ret = -6905; goto out; }
	f->spec.nodes[1].dependencies = 1; f->spec.nodes[0].use = f->uses[2].id; f->spec.nodes[1].use = f->uses[3].id;
	f->spec.mode = ANX_FRONTIER_INCREMENTAL;
	ret = anx_frontier_create(&owner->cid, &f->spec, &f->frontier);
	if (ret != ANX_OK) goto out;
	f->spec.nodes[0].use = f->uses[5].id; f->spec.count = 8;
	ret = anx_frontier_restore(f->full.id, f->full.epoch, &f->full);
	if (ret == ANX_OK) ret = denied069(f, &f->full, ANX_EBUSY);
	if (ret == ANX_OK) ret = anx_frontier_restore(f->full.id, f->full.epoch, &f->full);
	for (uint32_t i = 0; ret == ANX_OK && i < 2; i++) {
		ret = anx_frontier_step(f->full.id, f->full.epoch, &f->response, &f->full);
		if (ret == ANX_OK && (f->response.output_len != 4 || anx_memcmp(f->response.output, i ? "BBBB" : "AAAA", 4))) ret = -6906;
	}
	if (ret != ANX_OK) goto out;
	ret = anx_frontier_restore(f->frontier.id, f->frontier.epoch, &f->frontier);
	if (ret == ANX_OK) ret = anx_frontier_test_corrupt(f->frontier.id, 0);
	if (ret == ANX_OK) ret = denied069(f, &f->frontier, ANX_EIO);
	if (ret == ANX_OK) ret = anx_frontier_test_corrupt(f->frontier.id, 0);
	if (ret != ANX_OK) goto out;
	((uint8_t *)images[1]->payload)[10] ^= 1;
	ret = anx_frontier_step(f->frontier.id, f->frontier.epoch, &f->response, &f->frontier);
	int missing = anx_frontier_restore(f->frontier.id, f->frontier.epoch, &f->output);
	((uint8_t *)images[1]->payload)[10] ^= 1;
	if (ret != ANX_OK || missing != ANX_EBUSY || f->response.output_len != 4 || anx_memcmp(f->response.output, "AAAA", 4)) { ret = -6907; goto out; }
	ret = -6908;
	if (f->frontier.completed != 1 || f->frontier.resident != 1 || f->full.bytes_before_first_work != 144 ||
	    f->frontier.bytes_before_first_work != 72 || f->full.pages_before_first_work != 2 || f->frontier.pages_before_first_work != 1) goto out;
	ret = denied069(f, &f->frontier, ANX_EBUSY);
	if (ret == ANX_OK) ret = anx_frontier_restore(f->frontier.id, f->frontier.epoch, &f->frontier);
	if (ret != ANX_OK) goto out;
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch069", active069, f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint, "anxresearch069://inspect", sizeof(f->call.endpoint));
	foreign->execution.allow_side_effects = true; foreign->ext_call = &f->call; f->foreign = true;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	owner->ext_call = &f->call; f->foreign = false; ret = anx_cell_run(owner);
	if (ret == ANX_OK) kprintf("day069 full_before_first=%llu frontier_before_first=%llu output=AAAABBBB\n",
		(unsigned long long)f->full.bytes_before_first_work, (unsigned long long)f->frontier.bytes_before_first_work);
out:
	anx_external_unregister_handler("anxresearch069");
	if (f->full.id) anx_frontier_destroy(f->full.id);
	if (f->frontier.id) anx_frontier_destroy(f->frontier.id);
	if (f->probe.id) anx_frontier_destroy(f->probe.id);
	for (uint32_t i = 0; i < 6; i++) if (f->uses[i].id) anx_model_use_destroy(f->uses[i].id);
	if (attached) { anx_phase_get(&owner->cid, &phase); anx_phase_finish(&owner->cid, phase.epoch); anx_phase_detach(&owner->cid); }
	for (uint32_t i = 0; i < 2; i++) if (images[i]) { anx_so_delete(&images[i]->oid, false); anx_objstore_release(images[i]); }
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	if (foreign) anx_cell_destroy(foreign);
	if (owner) anx_cell_destroy(owner);
	if (ret != ANX_OK) kprintf("day069 native failure rc=%d\n", ret);
	anx_free(f); return ret;
}
#endif
