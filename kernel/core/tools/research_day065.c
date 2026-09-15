#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/execution_shape.h>
#include <anx/state_object.h>
#include <anx/string.h>
int anx_research_day065(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *image = NULL, *prompt = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_model_use_view use = {0};
	struct anx_model_use_spec model = { .maximum_tokens = 4 };
	struct anx_shape_spec spec = { .count = 1, .mode = ANX_SHAPE_REUSE };
	struct anx_shape_view shape = {0};
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA,
		.schema_uri = ANX_MODEL_USE_SCHEMA, .schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	anx_strlcpy(intent.name, "research-day-065", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_so_create(&params, &image);
	if (ret == ANX_OK) ret = anx_so_seal(&image->oid);
	params.object_type = ANX_OBJ_BYTE_DATA; params.schema_uri = NULL; params.schema_version = NULL;
	params.payload = "~"; params.payload_size = 1;
	if (ret == ANX_OK) ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	model.image = image->oid; model.prompt = prompt->oid;
	ret = anx_model_use_prepare(&owner->cid, &model, &use);
	if (ret != ANX_OK) goto out;
	spec.nodes[0].use = use.id;
	ret = -6501;
	if (anx_shape_compile(&owner->cid, &spec, &shape) != ANX_OK) goto out;
	ret = ANX_OK;
out:
	if (shape.id) anx_shape_destroy(shape.id);
	if (use.id) anx_model_use_destroy(use.id);
	if (owner) anx_cell_destroy(owner);
	if (image) { anx_so_delete(&image->oid, false); anx_objstore_release(image); }
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	return ret;
}
#endif
