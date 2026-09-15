#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/procedure.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
int anx_research_day066(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *image = NULL, *prompt = NULL, *knowledge = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_model_use_view use = {0};
	struct anx_model_use_spec model = { .maximum_tokens = 4 };
	struct anx_procedure_view procedure = {0};
	struct anx_anxml_response *response = anx_zalloc(sizeof(*response));
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA,
		.schema_uri = ANX_MODEL_USE_SCHEMA, .schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	int ret = ANX_ENOMEM;
	if (!response) return ret;
	anx_strlcpy(intent.name, "research-day-066", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_so_create(&params, &image);
	if (ret == ANX_OK) ret = anx_so_seal(&image->oid);
	params = (struct anx_so_create_params){ .object_type = ANX_OBJ_BYTE_DATA, .payload = "~", .payload_size = 1 };
	if (ret == ANX_OK) ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	params.payload = "Use the validated A adapter for this fixed prompt."; params.payload_size = anx_strlen(params.payload);
	if (ret == ANX_OK) ret = anx_so_create(&params, &knowledge);
	if (ret == ANX_OK) ret = anx_so_seal(&knowledge->oid);
	if (ret != ANX_OK) goto out;
	model.image = image->oid; model.prompt = prompt->oid;
	ret = anx_model_use_prepare(&owner->cid, &model, &use);
	if (ret == ANX_OK) ret = anx_model_use_execute(use.id, use.epoch, response, &use);
	if (ret != ANX_OK) goto out;
	ret = -6601;
	if (anx_procedure_compile(&owner->cid, use.id, &knowledge->oid, 0, &procedure) != ANX_OK) goto out;
	ret = ANX_OK;
out:
	if (procedure.id) anx_procedure_destroy(procedure.id);
	if (use.id) anx_model_use_destroy(use.id);
	if (owner) anx_cell_destroy(owner);
	if (image) { anx_so_delete(&image->oid, false); anx_objstore_release(image); }
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	if (knowledge) { anx_so_delete(&knowledge->oid, false); anx_objstore_release(knowledge); }
	anx_free(response);
	return ret;
}
#endif
