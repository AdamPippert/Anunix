#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/context.h>
#include <anx/cell.h>
#include <anx/string.h>
int anx_research_day073(void)
{
	struct anx_cell *owner = NULL;
	struct anx_state_object *object = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_context_view view = {0};
	struct anx_so_create_params p = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "tool", .payload_size = 4 };
	anx_strlcpy(intent.name, "research-day-073", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_so_create(&p, &object);
	if (ret == ANX_OK) ret = anx_so_seal(&object->oid);
	if (ret == ANX_OK && anx_context_import(&owner->cid, &object->oid, ANX_CONTEXT_TOOL_OUTPUT, &view) != ANX_OK) ret = -7301;
	if (object) { anx_so_delete(&object->oid, false); anx_objstore_release(object); }
	if (owner) anx_cell_destroy(owner);
	return ret;
}
#endif
