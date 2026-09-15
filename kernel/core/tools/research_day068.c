/* Semantic progress remains live while reconstructible acceleration state is absent. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/continuation.h>
#include <anx/phase.h>
#include <anx/model_use.h>
#include <anx/state_object.h>
#include <anx/string.h>
int anx_research_day068(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_continuation_view view = {0};
	struct anx_phase_view phase;
	struct anx_phase_contract contract = { .role = ANX_ROLE_RUNNER };
	struct anx_phase_request request = { ANX_PHASE_INFERENCE, 4096, 25 };
	struct anx_adapter_image image = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_state_object *model = NULL;
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA, .schema_uri = ANX_MODEL_USE_SCHEMA,
		.schema_version = "1", .payload = &image, .payload_size = sizeof(image) };
	bool attached = false;
	anx_strlcpy(intent.name, "research-day-068", sizeof(intent.name));
	contract.limits[ANX_PHASE_INFERENCE] = (struct anx_phase_limit){ true, ANX_MEM_L1, 4096, ANX_ACCEL_GPU, 25 };
	int ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret != ANX_OK) return ret;
	owner->execution.allow_recursive_cells = owner->execution.allow_side_effects = true;
	ret = anx_continuation_create(&owner->cid, &view);
	if (ret == ANX_OK) ret = anx_so_create(&params, &model);
	if (ret == ANX_OK) ret = anx_so_seal(&model->oid);
	if (ret == ANX_OK) ret = anx_phase_attach(&owner->cid, &contract);
	if (ret != ANX_OK) goto out;
	attached = true;
	ret = anx_phase_get(&owner->cid, &phase);
	if (ret == ANX_OK) ret = anx_phase_begin(&owner->cid, phase.epoch, &request);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &phase);
	if (ret != ANX_OK) goto out;
	ret = anx_continuation_suspend_configure(view.id, view.epoch, phase.epoch, &model->oid, &view) == ANX_OK ? ANX_OK : -6801;
out:
	if (view.id) anx_continuation_destroy(view.id);
	if (attached) { anx_phase_get(&owner->cid, &phase); anx_phase_finish(&owner->cid, phase.epoch); anx_phase_detach(&owner->cid); }
	if (model) { anx_oid_t oid = model->oid; anx_objstore_release(model); anx_so_delete(&oid, false); }
	anx_cell_destroy(owner); return ret;
}
#endif
