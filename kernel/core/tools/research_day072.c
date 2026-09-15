#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/epistemic.h>
#include <anx/cell.h>
int anx_research_day072(void)
{
	struct anx_cell *owner = NULL, *reviewer = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *object = NULL;
	struct anx_object_handle handle = {0};
	struct anx_so_create_params params = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "old!", .payload_size = 4 };
	struct anx_epistemic_spec spec = { .count = 1, .threshold = 1, .minimum_cut = 1 };
	struct anx_epistemic_view view = {0};
	int ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &reviewer);
	if (ret == ANX_OK) ret = anx_so_create(&params, &object);
	if (ret == ANX_OK) ret = anx_so_open(&object->oid, ANX_OPEN_READWRITE, &handle);
	if (ret == ANX_OK) ret = anx_object_stage(&handle, owner->cid);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&handle, "new!", 4);
	if (ret != ANX_OK) goto out;
	spec.reviewers[0] = reviewer->cid;
	ret = -7201;
	if (anx_epistemic_begin(&handle, &spec, &view) != ANX_OK) goto out;
	ret = ANX_OK;
out:
	if (handle.obj) { if (handle.obj->staged) anx_object_abort(&handle); anx_so_close(&handle); }
	if (view.id) anx_epistemic_destroy(view.id);
	if (object) { anx_so_delete(&object->oid, false); anx_objstore_release(object); }
	if (reviewer) anx_cell_destroy(reviewer);
	if (owner) anx_cell_destroy(owner);
	return ret;
}
#endif
