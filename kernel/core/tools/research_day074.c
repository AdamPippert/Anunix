#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/resource_shape.h>
#include <anx/phase.h>
#include <anx/state_object.h>
#include <anx/string.h>
int anx_research_day074(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *image = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_so_create_params p = { .object_type = ANX_OBJ_STRUCTURED_DATA, .schema_uri = ANX_MODEL_USE_SCHEMA,
		.schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	struct anx_phase_contract contract = { .role = ANX_ROLE_RUNNER };
	struct anx_phase_request request = { ANX_PHASE_INFERENCE, 8192, 0 };
	struct anx_phase_view phase;
	struct anx_resource_shape_view view;
	bool attached = false;
	anx_strlcpy(intent.name, "research-day-074", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_so_create(&p, &image);
	if (ret == ANX_OK) ret = anx_so_seal(&image->oid);
	contract.limits[ANX_PHASE_INFERENCE] = (struct anx_phase_limit){true,ANX_MEM_L1,8192,ANX_ACCEL_NONE,0};
	if (ret == ANX_OK) { ret = anx_phase_attach(&owner->cid, &contract); attached = ret == ANX_OK; }
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &phase);
	if (ret == ANX_OK) ret = anx_phase_begin(&owner->cid, phase.epoch, &request);
	if (ret == ANX_OK && anx_resource_shape_create(&owner->cid, &image->oid, 1, &view) != ANX_OK) ret = -7401;
	if (attached) { anx_phase_get(&owner->cid, &phase); anx_phase_finish(&owner->cid, phase.epoch); anx_phase_detach(&owner->cid); }
	if (image) { anx_so_delete(&image->oid, false); anx_objstore_release(image); }
	if (owner) anx_cell_destroy(owner);
	return ret;
}
#endif
