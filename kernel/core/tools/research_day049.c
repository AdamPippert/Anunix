/* Advice does not grant authority over the manager's memory placements. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/memplane.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>

struct ownership_context { struct anx_mem_entry *entry; };
static int advise(struct anx_external_call *call, void *arg)
{
	(void)call;
	struct ownership_context *c = arg;
	struct anx_mem_retention_hint hint = {100, 16};
	uint8_t before = c->entry->tier_mask;
	int ret = anx_memplane_hint(c->entry, &hint);
	if (ret != ANX_OK) return ret;
	return anx_memplane_promote(c->entry, ANX_MEM_L0) == ANX_EPERM && c->entry->tier_mask == before ? ANX_OK : -4901;
}

int anx_research_day049(void)
{
	struct anx_so_create_params p = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "resource", .payload_size = 8 };
	struct anx_state_object *object = NULL;
	struct anx_cell *caller = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct ownership_context context = {0};
	int ret = anx_so_create(&p, &object);
	if (ret == ANX_OK) ret = anx_so_seal(&object->oid);
	if (ret == ANX_OK) ret = anx_memplane_admit(&object->oid, ANX_ADMIT_LONG_TERM_CANDIDATE, &context.entry);
	anx_strlcpy(intent.name, "research-day-049", sizeof(intent.name));
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch049", advise, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	if (!call) { ret = ANX_ENOMEM; goto out; }
	anx_strlcpy(call->endpoint, "anxresearch049://advice", sizeof(call->endpoint));
	caller->ext_call = call; caller->execution.allow_side_effects = true;
	ret = anx_cell_run(caller);
out:
	anx_external_unregister_handler("anxresearch049");
	if (context.entry) anx_memplane_forget(context.entry, ANX_FORGET_HARD_DELETE);
	if (object) { anx_so_delete(&object->oid, false); anx_objstore_release(object); }
	if (caller) anx_cell_destroy(caller);
	anx_free(call);
	return ret;
}
#endif
