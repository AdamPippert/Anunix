/* Bind validated source objects to the exact bytes consumed by inference. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/model_use.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
int anx_research_day064(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *image = NULL, *prompt = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_model_use_view use = {0};
	struct anx_model_use_spec spec = { .maximum_tokens = 4 };
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA,
		.schema_uri = ANX_MODEL_USE_SCHEMA, .schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	anx_strlcpy(intent.name, "research-day-064", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_so_create(&params, &image);
	if (ret == ANX_OK) ret = anx_so_seal(&image->oid);
	params.object_type = ANX_OBJ_BYTE_DATA; params.schema_uri = NULL; params.schema_version = NULL;
	params.payload = "~"; params.payload_size = 1;
	if (ret == ANX_OK) ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	spec.image = image->oid; spec.prompt = prompt->oid;
	ret = -6401;
	if (anx_model_use_prepare(&owner->cid, &spec, &use) != ANX_OK) goto out;
	ret = ANX_OK;
out:
	if (use.id) anx_model_use_destroy(use.id);
	if (owner) anx_cell_destroy(owner);
	if (image) { anx_so_delete(&image->oid, false); anx_objstore_release(image); }
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	if (ret != ANX_OK) kprintf("day064 failure rc=%d\n", ret);
	return ret;
}
#endif
