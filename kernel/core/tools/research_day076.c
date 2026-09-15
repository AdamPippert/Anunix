#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/resource_shape.h>
#include <anx/physical_plan.h>
#include <anx/phase.h>
#include <anx/state_object.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
struct fixture076 {
	struct anx_resource_shape_view shape, shape_out;
	struct anx_logical_graph_view graph, graph_out, graph_saved;
	struct anx_physical_plan_view plans[5], plan_out;
	struct anx_model_use_view uses[2];
	struct anx_anxml_response response, saved;
	struct anx_external_call call;
	uint8_t program[32];
	bool foreign;
};
static int denied076(struct fixture076 *f, uint32_t plan, int expected)
{
	anx_memset(&f->response, 0x55, sizeof(f->response)); f->saved = f->response;
	anx_memset(&f->graph_out, 0x55, sizeof(f->graph_out)); f->graph_saved = f->graph_out;
	int ret = anx_physical_plan_commit(f->plans[plan].id, &f->response, &f->graph_out);
	struct anx_logical_graph_view current;
	if (ret != expected || anx_memcmp(&f->response, &f->saved, sizeof(f->response)) ||
	    anx_memcmp(&f->graph_out, &f->graph_saved, sizeof(f->graph_out))) return -7402;
	ret = anx_logical_graph_get(f->graph.id, &current);
	return ret == ANX_OK && current.epoch == f->graph.epoch && current.completed == f->graph.completed &&
		!anx_memcmp(current.program_digest, f->program, 32) ? ANX_OK : -7403;
}
int anx_research_day076(void)
{
	struct fixture076 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *image = NULL, *prompt = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_so_create_params p = { .object_type = ANX_OBJ_STRUCTURED_DATA, .schema_uri = ANX_MODEL_USE_SCHEMA,
		.schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	struct anx_phase_contract contract = { .role = ANX_ROLE_RUNNER };
	struct anx_phase_request request = { ANX_PHASE_INFERENCE, 8192, 0 };
	struct anx_phase_view phase;
	struct anx_model_use_spec use = { .maximum_tokens = 4 };
	struct anx_logical_graph_spec graph = { .count = 2 };
	bool attached = false;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-076", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret == ANX_OK) ret = anx_so_create(&p, &image);
	if (ret == ANX_OK) ret = anx_so_seal(&image->oid);
	contract.limits[ANX_PHASE_INFERENCE] = (struct anx_phase_limit){true,ANX_MEM_L1,8192,ANX_ACCEL_NONE,0};
	if (ret == ANX_OK) { ret = anx_phase_attach(&owner->cid, &contract); attached = ret == ANX_OK; }
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &phase);
	if (ret == ANX_OK) ret = anx_phase_begin(&owner->cid, phase.epoch, &request);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &phase);
	if (ret != ANX_OK) goto out;
	ret = -7401;
	if (anx_resource_shape_create(&owner->cid, &image->oid, 1, &f->shape) != ANX_OK) goto out;
	if (f->shape.resident_replicas || f->shape.physical_pages ||
	    anx_resource_shape_create(&owner->cid, &image->oid, 1, &f->shape_out) != ANX_EEXIST) { ret = -7407; goto out; }
	p = (struct anx_so_create_params){ .object_type = ANX_OBJ_BYTE_DATA, .payload = "~", .payload_size = 1 };
	ret = anx_so_create(&p, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	use.image = image->oid; use.prompt = prompt->oid;
	for (uint32_t i = 0; i < 2; i++) {
		ret = anx_model_use_prepare(&owner->cid, &use, &f->uses[i]);
		if (ret != ANX_OK) goto out;
		graph.nodes[i] = (struct anx_shape_node){f->uses[i].id, i ? 1U : 0U, ANX_SHAPE_INFER};
	}
	ret = anx_logical_graph_create(&owner->cid, &graph, &f->graph);
	if (ret != ANX_OK) goto out;
	anx_memcpy(f->program, f->graph.program_digest, 32);
	ret = anx_physical_plan_compile_shaped(f->graph.id, f->graph.epoch, phase.epoch, f->shape.id, f->shape.epoch, 0, &f->plans[0]);
	if (ret == ANX_OK) ret = anx_resource_shape_get(f->shape.id, &f->shape);
	if (ret != ANX_OK || f->shape.physical_pages != 1 || f->shape.resident_bytes != sizeof(adapter) ||
	    anx_resource_shape_destroy(f->shape.id) != ANX_EBUSY) { ret = -7408; goto out; }
	uint64_t first_epoch = f->shape.epoch;
	ret = anx_resource_shape_resize(f->shape.id, f->shape.epoch, 2, &f->shape);
	if (ret != ANX_OK || f->shape.epoch != first_epoch + 1 || f->shape.replicas != 2 || f->shape.physical_pages != 1 ||
	    anx_uuid_compare(&f->shape.source.oid, &image->oid) || anx_memcmp(f->shape.source.digest, f->uses[0].image.digest, 32)) { ret = -7409; goto out; }
	if (anx_resource_shape_resize(f->shape.id, f->shape.epoch, 3, &f->shape_out) != ANX_ENOMEM ||
	    anx_resource_shape_resize(f->shape.id, first_epoch, 1, &f->shape_out) != ANX_EBUSY) { ret = -7410; goto out; }
	f->plans[0].resource_epoch = f->shape.epoch; f->plans[0].replica = 1;
	ret = denied076(f, 0, ANX_EBUSY);
	if (ret == ANX_OK) ret = anx_physical_plan_compile_shaped(f->graph.id, f->graph.epoch, phase.epoch, f->shape.id, f->shape.epoch, 1, &f->plans[1]);
	if (ret == ANX_OK) ret = anx_resource_shape_get(f->shape.id, &f->shape);
	if (ret != ANX_OK || f->shape.physical_pages != 2 || f->shape.resident_bytes != 2 * sizeof(adapter)) { ret = -7411; goto out; }
	struct anx_memory_lower_contract lower = {ANX_MEMORY_COHERENT_CPU, true, false, 50};
	ret = anx_resource_shape_share(f->shape.id, f->shape.epoch, &lower, &f->shape_out) == ANX_OK ? ANX_OK : -7601;

out:
	anx_external_unregister_handler("anxresearch076");
	for (uint32_t i = 0; i < 5; i++) if (f->plans[i].id && anx_physical_plan_destroy(f->plans[i].id) != ANX_OK && ret == ANX_OK) ret = -7414;
	if (f->graph.id && anx_logical_graph_destroy(f->graph.id) != ANX_OK && ret == ANX_OK) ret = -7414;
	if (f->shape.id && anx_resource_shape_destroy(f->shape.id) != ANX_OK && ret == ANX_OK) ret = -7414;
	for (uint32_t i = 0; i < 2; i++) if (f->uses[i].id) anx_model_use_destroy(f->uses[i].id);
	if (attached) { anx_phase_get(&owner->cid, &phase); anx_phase_finish(&owner->cid, phase.epoch); anx_phase_detach(&owner->cid); }
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	if (image) { anx_so_delete(&image->oid, false); anx_objstore_release(image); }
	if (foreign && anx_cell_destroy(foreign) != ANX_OK && ret == ANX_OK) ret = -7415;
	if (owner && anx_cell_destroy(owner) != ANX_OK && ret == ANX_OK) ret = -7415;
	if (ret != ANX_OK) kprintf("day076 native failure rc=%d\n", ret);
	anx_free(f); return ret;
}
#endif
