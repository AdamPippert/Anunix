/* Calling a provider does not authorize exporting a run's confidential reads. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/effect_fence.h>
#include <anx/external_call.h>
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
int anx_research_day055(void)
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
#endif
