/* Stable logical bindings reject stale semantics and recheck physical readiness. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/route_binding.h>
#include <anx/model_server.h>
#include <anx/engine_lease.h>
#include <anx/state_object.h>
#include <anx/external_call.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
struct context059 { struct anx_route_binding_view view; struct anx_route_binding_spec spec; bool foreign; };
static int select059(struct anx_route_binding_view *view, const anx_eid_t *engine)
{
	struct anx_route_binding_view next;
	int ret = anx_route_binding_select(view->id, view->epoch, &next);
	if (ret != ANX_OK) return ret;
	if (next.id != view->id || next.epoch != view->epoch + 1 ||
	    anx_uuid_compare(&next.cell, &view->cell) || anx_uuid_compare(&next.model, &view->model) ||
	    next.model_version != view->model_version || anx_uuid_compare(&next.engine, engine)) return -5903;
	*view = next;
	return ANX_OK;
}
static int deny059(struct anx_route_binding_view *view, int expected)
{
	struct anx_route_binding_view output, sentinel, current;
	anx_memset(&output, 0x55, sizeof(output)); sentinel = output;
	if (anx_route_binding_select(view->id, view->epoch, &output) != expected ||
	    anx_memcmp(&output, &sentinel, sizeof(output)) || anx_route_binding_get(view->id, &current) != ANX_OK ||
	    anx_memcmp(&current, view, sizeof(current))) return -5904;
	return ANX_OK;
}
static int active059(struct anx_external_call *call, void *context)
{
	struct context059 *c = context;
	struct anx_route_binding_view output, sentinel;
	(void)call;
	anx_memset(&output, 0x55, sizeof(output)); sentinel = output;
	if (anx_route_binding_create(&c->view.cell, &c->spec, &output) != ANX_EPERM ||
	    anx_route_binding_destroy(c->view.id) != ANX_EPERM || anx_memcmp(&output, &sentinel, sizeof(output))) return -5915;
	if (c->foreign) {
		if (anx_route_binding_get(c->view.id, &output) != ANX_EPERM ||
		    anx_route_binding_select(c->view.id, c->view.epoch, &output) != ANX_EPERM ||
		    anx_route_binding_check(c->view.id, c->view.epoch) != ANX_EPERM ||
		    anx_memcmp(&output, &sentinel, sizeof(output))) return -5915;
		return ANX_OK;
	}
	if (anx_route_binding_get(c->view.id, &output) != ANX_OK ||
	    anx_memcmp(&output, &c->view, sizeof(output)) || anx_route_binding_check(c->view.id, c->view.epoch) != ANX_OK) return -5916;
	return select059(&c->view, &c->view.engine);
}
int anx_research_day059(void)
{
	struct anx_cell *cell = NULL, *dependency = NULL, *foreign = NULL;
	struct anx_engine *engines[3] = {0};
	struct anx_model_server *servers[3] = {0};
	struct anx_state_object *model = NULL, *input = NULL;
	struct anx_object_handle writer = {0};
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct context059 context = {0};
	struct anx_route_binding_spec spec = { .schema = 1, .required_context_tokens = 16,
		.required_caps = ANX_CAP_SUMMARIZATION, .engine_count = 3 };
	struct anx_route_binding_view view = {0}, old = {0};
	struct anx_so_create_params p = { .object_type = ANX_OBJ_STRUCTURED_DATA, .payload = "model-v1", .payload_size = 8 };
	int ret = anx_so_create(&p, &model);
	if (ret == ANX_OK) ret = anx_so_seal(&model->oid);
	p.object_type = ANX_OBJ_BYTE_DATA; p.payload = "state-v1"; p.sensitivity = ANX_SENSITIVITY_CONFIDENTIAL;
	if (ret == ANX_OK) ret = anx_so_create(&p, &input);
	if (ret == ANX_OK) ret = anx_so_open(&input->oid, ANX_OPEN_READWRITE, &writer);
	anx_strlcpy(intent.name, "research-day-059", sizeof(intent.name));
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &dependency);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
	if (ret == ANX_OK) ret = anx_cell_add_dependency(cell, &dependency->cid);
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	cell->constraints.locality = ANX_LOCAL_ONLY; cell->execution.allow_side_effects = true;
	cell->input_count = 1; cell->inputs[0].state_object_ref = input->oid; cell->inputs[0].required = true;
	anx_strlcpy(cell->inputs[0].name, "current-state", sizeof(cell->inputs[0].name));
	for (uint32_t i = 0; i < 3; i++) {
		ret = anx_engine_register(i == 0 ? "research-day-059-a" : i == 1 ? "research-day-059-b" : "research-day-059-remote",
			i < 2 ? ANX_ENGINE_LOCAL_MODEL : ANX_ENGINE_REMOTE_MODEL, ANX_CAP_SUMMARIZATION, &engines[i]);
		if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
		engines[i]->max_context_tokens = 128; engines[i]->supports_private_data = true;
		engines[i]->model.mem_footprint_bytes = 4096; engines[i]->is_local = i < 2; engines[i]->requires_network = i == 2;
		engines[i]->quality_score = i == 0 ? 90 : i == 1 ? 10 : 100;
		spec.engines[i] = engines[i]->eid;
	}
	spec.model = model->oid;
	ret = -5901;
	if (anx_route_binding_create(&cell->cid, &spec, &view) != ANX_OK) goto out;
	ret = -5902;
	if (!view.id || view.epoch != 1 || !anx_uuid_is_nil(&view.engine) || anx_cell_destroy(cell) != ANX_EBUSY ||
	    deny059(&view, ANX_EBUSY) != ANX_OK || anx_route_binding_check(view.id, view.epoch) != ANX_ENODEV) goto out;
	ret = anx_cell_run(dependency);
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	if (deny059(&view, ANX_ENODEV) != ANX_OK) { ret = -5905; goto out; }
	for (uint32_t i = 0; i < 3; i++) {
		engines[i]->status = ANX_ENGINE_REGISTERED;
		ret = anx_msrv_create(engines[i], &servers[i]);
		if (ret == ANX_OK) ret = anx_msrv_start(servers[i]);
		if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	}
	ret = select059(&view, &engines[0]->eid);
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	uint64_t stale = view.epoch;
	engines[0]->status = ANX_ENGINE_OFFLINE;
	ret = -5906;
	if (anx_route_binding_check(view.id, view.epoch) != ANX_ENODEV || select059(&view, &engines[1]->eid) != ANX_OK) goto out;
	engines[0]->status = ANX_ENGINE_AVAILABLE;
	if (anx_route_binding_check(view.id, stale) != ANX_EBUSY || select059(&view, &engines[1]->eid) != ANX_OK) goto out;
	servers[1]->msrv_status = ANX_MSRV_DRAINING;
	ret = select059(&view, &engines[0]->eid);
	servers[1]->msrv_status = ANX_MSRV_SERVING;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	servers[0]->pending_count = servers[0]->max_pending;
	ret = select059(&view, &engines[1]->eid);
	servers[0]->pending_count = 0;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	ret = anx_lease_revoke(engines[1]->lease);
	if (ret == ANX_OK) ret = select059(&view, &engines[0]->eid);
	if (ret == ANX_OK) ret = anx_msrv_stop(servers[1]);
	if (ret == ANX_OK) ret = anx_msrv_start(servers[1]);
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	engines[0]->lease->expires_at = 1;
	ret = select059(&view, &engines[1]->eid);
	engines[0]->lease->expires_at = 0;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	engines[1]->max_context_tokens = 8;
	ret = select059(&view, &engines[0]->eid);
	engines[1]->max_context_tokens = 128;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	engines[0]->supports_private_data = false;
	ret = select059(&view, &engines[1]->eid);
	engines[0]->supports_private_data = true;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	engines[1]->model.quant = ANX_QUANT_Q8;
	ret = select059(&view, &engines[0]->eid);
	engines[1]->model.quant = ANX_QUANT_NONE;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	/* No local candidate can turn the private pool into permission for a remote backend. */
	engines[0]->status = engines[1]->status = ANX_ENGINE_OFFLINE;
	ret = deny059(&view, ANX_EPERM);
	engines[0]->status = engines[1]->status = ANX_ENGINE_AVAILABLE;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	cell->execution.allow_network = true;
	ret = deny059(&view, ANX_EBUSY);
	cell->execution.allow_network = false;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	/* Caller mutation of the original specification cannot replace the private pool. */
	struct anx_route_binding_spec original = spec;
	spec.engines[0] = engines[2]->eid; spec.engine_count = 1; spec.required_caps = 0;
	ret = select059(&view, &engines[0]->eid); spec = original;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	ret = anx_so_replace_payload(&writer, "state-v2", 8);
	if (ret == ANX_OK) ret = deny059(&view, ANX_EBUSY);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&writer, "state-v1", 8);
	if (ret == ANX_OK) ret = deny059(&view, ANX_EBUSY);
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	old = view;
	ret = anx_route_binding_create(&cell->cid, &spec, &view);
	if (ret == ANX_OK) ret = select059(&view, &engines[0]->eid);
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	ret = -5907;
	if (view.id <= old.id || anx_uuid_compare(&view.cell, &old.cell) ||
	    anx_uuid_compare(&view.model, &old.model) || anx_route_binding_check(old.id, old.epoch) != ANX_EBUSY) goto out;
	((char *)input->payload)[0] ^= 1;
	ret = deny059(&view, ANX_EBUSY); ((char *)input->payload)[0] ^= 1;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	input->access_policy.rule_count = 1; input->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	input->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	ret = deny059(&view, ANX_EPERM); input->access_policy.rule_count = 0;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	((char *)model->payload)[0] ^= 1;
	ret = deny059(&view, ANX_EBUSY); ((char *)model->payload)[0] ^= 1;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	model->access_policy.rule_count = 1; model->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	model->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	ret = deny059(&view, ANX_EPERM); model->access_policy.rule_count = 0;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	dependency->status = ANX_CELL_FAILED;
	ret = deny059(&view, ANX_EPERM); dependency->status = ANX_CELL_COMPLETED;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	struct anx_route_binding_view invalid, sentinel;
	anx_memset(&invalid, 0x55, sizeof(invalid)); sentinel = invalid;
	ret = -5908;
	spec.engines[1] = spec.engines[0];
	if (anx_route_binding_create(&cell->cid, &spec, &invalid) != ANX_EINVAL) goto out;
	spec = original; spec.required_context_tokens = 0;
	if (anx_route_binding_create(&cell->cid, &spec, &invalid) != ANX_EINVAL) goto out;
	spec = original; spec.model = input->oid;
	if (anx_route_binding_create(&cell->cid, &spec, &invalid) != ANX_EBUSY) goto out;
	spec = original; cell->inputs[0].mode = ANX_INPUT_READ_WRITE;
	int invalid_mode = anx_route_binding_create(&cell->cid, &spec, &invalid);
	cell->inputs[0].mode = ANX_INPUT_READ;
	if (invalid_mode != ANX_ENOTSUP || anx_memcmp(&invalid, &sentinel, sizeof(invalid))) goto out;
	/* Both an actual foreign Cell and the owner traverse the public binding API. */
	ret = anx_external_register_handler("anxresearch059", active059, &context);
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	call = anx_zalloc(sizeof(*call)); ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch059://bind", sizeof(call->endpoint));
	context.view = view; context.spec = spec; context.foreign = true;
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	foreign->execution.allow_side_effects = true; foreign->ext_call = call;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	context.foreign = false; cell->ext_call = call;
	ret = anx_cell_run(cell); view = context.view;
	if (ret != ANX_OK) { kprintf("day059 line %u rc=%d\n", __LINE__, ret); goto out; }
	ret = -5909;
	if (anx_route_binding_check(view.id, view.epoch) != ANX_EPERM || anx_uuid_compare(&view.cell, &cell->cid)) goto out;
	ret = anx_route_binding_destroy(view.id); view.id = 0;
	if (ret == ANX_OK) ret = anx_route_binding_destroy(old.id); old.id = 0;
out:
	if (view.id) anx_route_binding_destroy(view.id);
	if (old.id && old.id != view.id) anx_route_binding_destroy(old.id);
	if (cell) anx_cell_destroy(cell);
	if (dependency) anx_cell_destroy(dependency);
	if (foreign) anx_cell_destroy(foreign);
	anx_external_unregister_handler("anxresearch059"); anx_free(call);
	for (uint32_t i = 0; i < 3; i++) {
		if (servers[i]) {
			anx_cid_t cid = servers[i]->cell_id;
			anx_msrv_stop(servers[i]); anx_msrv_destroy(servers[i]);
			struct anx_cell *server_cell = anx_cell_store_lookup(&cid);
			if (server_cell) anx_cell_destroy(server_cell);
		}
		if (engines[i]) anx_engine_unregister(engines[i]);
	}
	anx_so_close(&writer);
	if (input) { anx_so_delete(&input->oid, false); anx_objstore_release(input); }
	if (model) { anx_so_delete(&model->oid, false); anx_objstore_release(model); }
	return ret;
}
#endif
