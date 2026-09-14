/* Baseline eagerly dispatches a predicted action before the actual choice. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/state_object.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>

struct eager_context { struct anx_object_handle handle; uint32_t calls; };
static int eager_handler(struct anx_external_call *call, void *arg)
{
	struct eager_context *c = arg;
	(void)call; c->calls++;
	return anx_so_replace_payload(&c->handle, "predicted", 9);
}
int anx_research_day045(void)
{
	struct anx_so_create_params p = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "original", .payload_size = 8 };
	struct anx_state_object *obj = NULL;
	struct anx_external_call *call = NULL;
	struct eager_context context = {0};
	int ret = anx_so_create(&p, &obj);
	if (ret == ANX_OK) ret = anx_so_open(&obj->oid, ANX_OPEN_READWRITE, &context.handle);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch045", eager_handler, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch045://prediction", sizeof(call->endpoint));
	ret = anx_external_invoke(call);
	if (ret != ANX_OK) goto out;
	/* The final action differs; discarding now cannot undo the handler. */
	ret = !context.calls && obj->version == 1 ? ANX_OK : -4501;
out:
	anx_external_unregister_handler("anxresearch045");
	anx_so_close(&context.handle);
	if (obj) { anx_so_delete(&obj->oid, false); anx_objstore_release(obj); }
	anx_free(call);
	return ret;
}
#endif
