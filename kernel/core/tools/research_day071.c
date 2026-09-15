#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/physical_plan.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
int anx_research_day071(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *image = NULL, *prompt = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA, .schema_uri = ANX_MODEL_USE_SCHEMA,
		.schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	struct anx_model_use_spec use_spec = { .maximum_tokens = 4 };
	struct anx_model_use_view use = {0};
	struct anx_logical_graph_spec spec = { .count = 1 };
	struct anx_logical_graph_view graph = {0};
	int ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_so_create(&params, &image);
	if (ret == ANX_OK) ret = anx_so_seal(&image->oid);
	params = (struct anx_so_create_params){ .object_type = ANX_OBJ_BYTE_DATA, .payload = "~", .payload_size = 1 };
	if (ret == ANX_OK) ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	use_spec.image = image->oid; use_spec.prompt = prompt->oid;
	ret = anx_model_use_prepare(&owner->cid, &use_spec, &use);
	if (ret != ANX_OK) goto out;
	spec.nodes[0] = (struct anx_shape_node){use.id,0,ANX_SHAPE_INFER};
	ret = -7101;
	if (anx_logical_graph_create(&owner->cid, &spec, &graph) != ANX_OK) goto out;
	ret = ANX_OK;
out:
	if (graph.id) anx_logical_graph_destroy(graph.id);
	if (use.id) anx_model_use_destroy(use.id);
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	if (image) { anx_so_delete(&image->oid, false); anx_objstore_release(image); }
	if (owner) anx_cell_destroy(owner);
	return ret;
}
#endif
