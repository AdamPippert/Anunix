/* Calling a provider does not authorize exporting a run's confidential reads. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/effect_fence.h>
#include <anx/external_call.h>
#include <anx/external_operation.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
struct boundary_context { anx_oid_t secret; struct anx_external_call *send; uint32_t calls; };
static int boundary_send(struct anx_external_call *call, void *arg)
{
	struct boundary_context *c = arg;
	(void)call; c->calls++; return ANX_OK;
}
static int boundary_read(struct anx_external_call *call, void *arg)
{
	struct boundary_context *c = arg;
	struct anx_object_handle h = {0};
	char bytes[4];
	(void)call;
	int ret = anx_so_open(&c->secret, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	ret = anx_so_read_payload(&h, 0, bytes, sizeof(bytes));
	anx_so_close(&h);
	if (ret != 4) return ANX_EIO;
	return anx_external_invoke(c->send) == ANX_EPERM && !c->calls ? ANX_OK : -5501;
}
static int flow_boundary(void)
{
	struct anx_cell *cell = NULL;
	struct anx_external_call *call = NULL, *send = NULL;
	struct anx_state_object *secret = NULL;
	struct anx_so_create_params params = {0};
	struct anx_cell_intent intent = {0};
	struct anx_effect_fence_view fence;
	struct boundary_context context = {0};
	int ret = anx_effect_fence_create(&fence);
	if (ret != ANX_OK) return ret;
	anx_strlcpy(intent.name, "research-day-055", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(cell, &fence.id);
	if (ret != ANX_OK) goto out;
	params.object_type = ANX_OBJ_BYTE_DATA; params.payload = "data"; params.payload_size = 4;
	params.sensitivity = ANX_SENSITIVITY_CONFIDENTIAL; params.creator_cell = cell->cid;
	ret = anx_so_create(&params, &secret);
	if (ret != ANX_OK) goto out;
	context.secret = secret->oid;
	call = anx_zalloc(sizeof(*call)); send = anx_zalloc(sizeof(*send));
	if (!call || !send) { ret = ANX_ENOMEM; goto out; }
	context.send = send;
	anx_strlcpy(call->endpoint, "anxresearch055read://read", sizeof(call->endpoint));
	anx_strlcpy(send->endpoint, "anxresearch055send://public", sizeof(send->endpoint));
	cell->ext_call = call; cell->execution.allow_side_effects = true;
	ret = anx_external_register_handler("anxresearch055read", boundary_read, &context);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch055send", boundary_send, &context);
	if (ret == ANX_OK) ret = anx_cell_run(cell);
out:
	anx_external_unregister_handler("anxresearch055read");
	anx_external_unregister_handler("anxresearch055send");
	if (secret) { anx_oid_t id = secret->oid; anx_objstore_release(secret); anx_so_delete(&id, false); }
	if (cell) anx_cell_destroy(cell);
	anx_free(call); anx_free(send);
	return ret;
}

struct operation_context {
	anx_oid_t operation, source;
	struct anx_external_call *reply;
	uint32_t mode, calls;
};
static int operation_provider(struct anx_external_call *call, void *arg)
{
	struct operation_context *c = arg;
	c->calls++;
	if (c->calls != 1 || call->request_size != 4 || anx_memcmp(call->request_body, "data", 4)) return -5502;
	struct anx_external_operation_view view;
	if (anx_external_operation_get(&c->operation, &view) != ANX_OK || view.phase != ANX_EFFECT_DISPATCHING ||
	    anx_external_operation_dispatch(&c->operation, c->reply) != ANX_EBUSY) return -5503;
	if (c->mode == 1) return ANX_ETIMEDOUT;
	anx_memcpy(call->response_buf, "done", 4); call->response_size = 4; call->status_code = 200;
	return ANX_OK;
}
static int operation_driver(struct anx_external_call *call, void *arg)
{
	struct operation_context *c = arg;
	struct anx_object_handle h = {0};
	struct anx_external_operation_view view;
	char data[4];
	(void)call;
	int ret = anx_so_open(&c->source, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	ret = anx_so_read_payload(&h, 0, data, sizeof(data)); anx_so_close(&h);
	if (ret != 4) return ANX_EIO;
	int expected = c->mode == 1 ? ANX_ETIMEDOUT : c->mode == 3 ? ANX_EPERM : c->mode >= 2 ? ANX_EBUSY : ANX_OK;
	ret = anx_external_operation_dispatch(&c->operation, c->reply);
	if (ret != expected || c->calls != (c->mode <= 1 ? 1U : 0U)) return -5504;
	if (anx_external_operation_get(&c->operation, &view) != ANX_OK) return -5505;
	enum anx_effect_phase phase = c->mode == 0 ? ANX_EFFECT_COMMITTED : c->mode == 1 ? ANX_EFFECT_UNKNOWN : ANX_EFFECT_PREPARED;
	if (view.phase != phase || (c->mode <= 1 && view.transport_result != expected)) return -5505;
	if (c->mode <= 1 && (anx_external_operation_dispatch(&c->operation, c->reply) != ANX_EBUSY || c->calls != 1)) return -5506;
	if (c->mode == 0 && (c->reply->response_size != 4 || anx_memcmp(c->reply->response_buf, "done", 4))) return -5507;
	if (anx_external_operation_discard(&c->operation) != ANX_EPERM ||
	    anx_external_operation_prepare(NULL, NULL, NULL, NULL, NULL) != ANX_EPERM) return -5508;
	return ANX_OK;
}
static int operation_foreign(struct anx_external_call *call, void *arg)
{
	struct operation_context *c = arg;
	struct anx_external_operation_view view;
	(void)call;
	return anx_external_operation_get(&c->operation, &view) == ANX_EPERM &&
		anx_external_operation_dispatch(&c->operation, c->reply) == ANX_EPERM ? ANX_OK : -5509;
}
static int protected_boundary(uint32_t mode)
{
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_external_call *call = NULL, *provider = NULL, *reply = NULL;
	struct anx_state_object *source = NULL;
	struct anx_object_handle write = {0};
	struct anx_so_create_params params = {0};
	struct anx_cell_intent intent = {0};
	struct anx_effect_fence_view fence;
	struct anx_external_operation_view view;
	struct operation_context context = { .mode = mode };
	struct anx_sink *sink = NULL;
	bool prepared = false;
	int ret = anx_effect_fence_create(&fence);
	if (ret != ANX_OK) return ret;
	anx_strlcpy(intent.name, "research-day-055-protected", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(owner, &fence.id);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	params.object_type = ANX_OBJ_BYTE_DATA; params.payload = "data"; params.payload_size = 4;
	params.sensitivity = ANX_SENSITIVITY_CONFIDENTIAL; params.creator_cell = owner->cid;
	ret = anx_so_create(&params, &source);
	if (ret == ANX_OK) ret = anx_sink_register("research-day-055-approved", ANX_SENSITIVITY_CONFIDENTIAL, &sink);
	if (ret != ANX_OK) goto out;
	context.source = source->oid;
	call = anx_zalloc(sizeof(*call)); provider = anx_zalloc(sizeof(*provider)); reply = anx_zalloc(sizeof(*reply));
	if (!call || !provider || !reply) { ret = ANX_ENOMEM; goto out; }
	context.reply = reply;
	anx_strlcpy(provider->endpoint, "anxresearch055provider://update", sizeof(provider->endpoint));
	provider->request_body = "data"; provider->request_size = 4;
	ret = anx_external_register_handler("anxresearch055provider", operation_provider, &context);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch055driver", operation_driver, &context);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch055foreign", operation_foreign, &context);
	if (ret != ANX_OK) goto out;
	ret = -5510;
	if (anx_external_operation_prepare(&owner->cid, provider, NULL, &source->oid, &context.operation) != ANX_EPERM) goto out;
	ret = anx_external_operation_prepare(&owner->cid, provider, sink->name, &source->oid, &context.operation);
	if (ret != ANX_OK) goto out;
	prepared = true;
	/* Original descriptor changes cannot redirect the copied protected request. */
	provider->request_body = "evil"; provider->request_size = 4;
	anx_strlcpy(provider->endpoint, "missing://different", sizeof(provider->endpoint));
	ret = -5511;
	if (anx_external_operation_dispatch(&context.operation, reply) != ANX_EPERM) goto out;
	anx_strlcpy(call->endpoint, "anxresearch055foreign://inspect", sizeof(call->endpoint));
	foreign->ext_call = call;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	if (mode == 2) {
		ret = anx_so_open(&source->oid, ANX_OPEN_READWRITE, &write);
		if (ret == ANX_OK) ret = anx_so_replace_payload(&write, "new!", 4);
	} else if (mode == 3) ret = anx_sink_register(sink->name, ANX_SENSITIVITY_PUBLIC, NULL);
	else if (mode == 4) ret = anx_external_register_handler("anxresearch055provider", operation_provider, &context);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(call->endpoint, "anxresearch055driver://run", sizeof(call->endpoint)); owner->ext_call = call;
	ret = anx_cell_run(owner);
	if (ret != ANX_OK) goto out;
	ret = anx_external_operation_get(&context.operation, &view);
	if (ret != ANX_OK) goto out;
	ret = -5512;
	if (anx_uuid_compare(&view.owner, &owner->cid) || anx_uuid_compare(&view.source, &source->oid) ||
	    anx_strcmp(view.sink_name, "research-day-055-approved")) goto out;
	if (mode == 1) {
		if (view.phase != ANX_EFFECT_UNKNOWN || anx_external_operation_discard(&context.operation) != ANX_EBUSY) goto out;
	} else {
		ret = anx_external_operation_discard(&context.operation);
		if (ret != ANX_OK) goto out;
		prepared = false;
	}
	ret = ANX_OK;
out:
	if (prepared) anx_external_operation_discard(&context.operation);
	if (write.obj) anx_so_close(&write);
	if (source) { anx_oid_t id = source->oid; anx_objstore_release(source); anx_so_delete(&id, false); }
	if (owner) { int r = anx_cell_destroy(owner); if (ret == ANX_OK && r != ANX_OK) ret = -5513; }
	if (foreign) anx_cell_destroy(foreign);
	anx_external_unregister_handler("anxresearch055provider");
	anx_external_unregister_handler("anxresearch055driver");
	anx_external_unregister_handler("anxresearch055foreign");
	anx_free(call); anx_free(provider); anx_free(reply);
	return ret;
}
int anx_research_day055(void)
{
	int ret = flow_boundary();
	for (uint32_t mode = 0; ret == ANX_OK && mode < 5; mode++) ret = protected_boundary(mode);
	return ret;
}
#endif
