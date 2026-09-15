#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/physical_plan.h>
#include <anx/phase.h>
#include <anx/state_object.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
struct fixture071 {
	struct anx_logical_graph_spec spec;
	struct anx_logical_graph_view graph, output, sentinel;
	struct anx_physical_plan_view plans[6], plan_output;
	struct anx_model_use_view uses[7];
	struct anx_anxml_response response, saved, reference[3];
	struct anx_external_call call;
	uint8_t program[32];
	bool foreign;
};
static int denied071(struct fixture071 *f, uint64_t plan, int expected)
{
	struct anx_logical_graph_view current;
	anx_memset(&f->output, 0x55, sizeof(f->output)); f->sentinel = f->output;
	anx_memset(&f->response, 0x55, sizeof(f->response)); f->saved = f->response;
	int ret = anx_physical_plan_commit(plan, &f->response, &f->output);
	if (ret != expected) { kprintf("day071 denied expected=%d actual=%d\n", expected, ret); return -7102; }
	return anx_logical_graph_get(f->graph.id, &current) == ANX_OK && !anx_memcmp(&current, &f->graph, sizeof(current)) &&
		!anx_memcmp(&f->output, &f->sentinel, sizeof(f->output)) && !anx_memcmp(&f->response, &f->saved, sizeof(f->response)) ? ANX_OK : -7103;
}
static int active071(struct anx_external_call *call, void *arg)
{
	struct fixture071 *f = arg; (void)call;
	if (anx_physical_plan_compile(f->graph.id, f->graph.epoch, f->plans[5].phase_epoch, ANX_PHYSICAL_DIRECT, &f->plan_output) != ANX_EPERM ||
	    anx_logical_graph_create(&f->graph.owner, &f->spec, &f->output) != ANX_EPERM ||
	    anx_logical_graph_destroy(f->graph.id) != ANX_EPERM || anx_physical_plan_destroy(f->plans[5].id) != ANX_EPERM) return -7110;
	if (f->foreign) return anx_logical_graph_get(f->graph.id, &f->output) == ANX_EPERM &&
		anx_physical_plan_get(f->plans[5].id, &f->plan_output) == ANX_EPERM &&
		anx_physical_plan_commit(f->plans[5].id, &f->response, &f->output) == ANX_EPERM &&
		anx_logical_graph_read(f->graph.id, 0, &f->response) == ANX_EPERM ? ANX_OK : -7111;
	int ret = anx_physical_plan_commit(f->plans[5].id, &f->response, &f->graph);
	if (ret != ANX_OK || f->graph.completed != 7 || f->graph.state != ANX_SHAPE_COMPLETED ||
	    f->graph.physical_operations != 2 || f->graph.reused_operations != 1 || anx_memcmp(f->program, f->graph.program_digest, 32)) return -7112;
	for (uint32_t i = 0; i < 3; i++) {
		ret = anx_logical_graph_read(f->graph.id, i, &f->response);
		if (ret != ANX_OK || f->response.output_len != f->reference[i].output_len ||
		    anx_memcmp(f->response.output, f->reference[i].output, f->response.output_len)) return -7113;
	}
	return ANX_OK;
}
int anx_research_day071(void)
{
	struct fixture071 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *images[2] = {0}, *prompt = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA, .schema_uri = ANX_MODEL_USE_SCHEMA,
		.schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	struct anx_model_use_spec use = { .maximum_tokens = 4 };
	struct anx_shape_spec literal = { .count = 3, .mode = ANX_SHAPE_LITERAL };
	struct anx_shape_view reference = {0}, shape_output;
	struct anx_phase_contract contract = { .role = ANX_ROLE_RUNNER };
	struct anx_phase_request request = { ANX_PHASE_INFERENCE, 12288, 0 };
	struct anx_phase_view phase;
	bool attached = false;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-071", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	for (uint32_t i = 0; i < 2; i++) {
		adapter.deltas[0].next = adapter.deltas[1].previous = adapter.deltas[1].next = (uint8_t)('A' + i);
		ret = anx_so_create(&params, &images[i]);
		if (ret == ANX_OK) ret = anx_so_seal(&images[i]->oid);
		if (ret != ANX_OK) goto out;
	}
	params = (struct anx_so_create_params){ .object_type = ANX_OBJ_BYTE_DATA, .payload = "~", .payload_size = 1 };
	ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	use.prompt = prompt->oid;
	for (uint32_t i = 0; i < 7; i++) {
		use.image = images[i % 3 == 2 ? 1 : 0]->oid;
		ret = anx_model_use_prepare(i == 6 ? &foreign->cid : &owner->cid, &use, &f->uses[i]);
		if (ret != ANX_OK) goto out;
		if (i < 3) literal.nodes[i] = (struct anx_shape_node){f->uses[i].id,i == 2 ? 3U : 0U,ANX_SHAPE_INFER};
		else if (i < 6) f->spec.nodes[i - 3] = (struct anx_shape_node){f->uses[i].id,i == 5 ? 3U : 0U,ANX_SHAPE_INFER};
	}
	f->spec.count = 3;
	ret = -7101;
	if (anx_logical_graph_create(&owner->cid, &f->spec, &f->graph) != ANX_OK) goto out;
	anx_memcpy(f->program, f->graph.program_digest, 32);
	f->spec.nodes[0].use = f->uses[6].id;
	if (anx_logical_graph_create(&owner->cid, &f->spec, &f->output) != ANX_EPERM ||
	    anx_shape_run(f->graph.id, f->graph.epoch, &shape_output) != ANX_ENOTSUP) { ret = -7104; goto out; }
	f->spec.nodes[0].use = f->uses[3].id; f->spec.nodes[2].dependencies = 4;
	if (anx_logical_graph_create(&owner->cid, &f->spec, &f->output) != ANX_EINVAL) { ret = -7104; goto out; }
	f->spec.nodes[2].dependencies = 3;
	ret = anx_shape_compile(&owner->cid, &literal, &reference);
	if (ret == ANX_OK) ret = anx_shape_run(reference.id, reference.epoch, &reference);
	if (ret != ANX_OK) goto out;
	for (uint32_t i = 0; i < 3; i++) {
		ret = anx_shape_read(reference.id, i, &f->reference[i]);
		if (ret != ANX_OK) goto out;
	}
	if (reference.physical_operations != 3 || reference.reused_operations) { ret = -7104; goto out; }
	contract.limits[ANX_PHASE_INFERENCE] = (struct anx_phase_limit){ true, ANX_MEM_L1, 12288, ANX_ACCEL_NONE, 0 };
	ret = anx_phase_attach(&owner->cid, &contract); attached = ret == ANX_OK;
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &phase);
	if (ret == ANX_OK) ret = anx_phase_begin(&owner->cid, phase.epoch, &request);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &phase);
	if (ret != ANX_OK) goto out;
	if (anx_physical_plan_compile(reference.id, reference.epoch, phase.epoch, ANX_PHYSICAL_DIRECT, &f->plan_output) != ANX_EINVAL) { ret = -7104; goto out; }
	for (uint32_t i = 0; i < 2; i++) {
		ret = anx_physical_plan_compile(f->graph.id, f->graph.epoch, phase.epoch, ANX_PHYSICAL_DIRECT, &f->plans[i]);
		if (ret != ANX_OK) goto out;
	}
	if (anx_logical_graph_destroy(f->graph.id) != ANX_EBUSY || f->plans[0].node || f->plans[0].mode != ANX_PHYSICAL_DIRECT) { ret = -7105; goto out; }
	f->spec.nodes[0].use = f->uses[6].id; f->spec.count = 8;
	f->plans[0].node = 2; f->plans[0].source_node = 2; f->plans[0].logical_epoch++;
	ret = anx_physical_plan_commit(f->plans[0].id, &f->response, &f->graph);
	if (ret != ANX_OK || f->graph.completed != 1 || anx_memcmp(f->response.output, "AAAA", 4) ||
	    anx_memcmp(f->program, f->graph.program_digest, 32)) { ret = -7106; goto out; }
	ret = denied071(f, f->plans[1].id, ANX_EBUSY);
	if (ret == ANX_OK) ret = anx_physical_plan_compile(f->graph.id, f->graph.epoch, phase.epoch, ANX_PHYSICAL_REUSE, &f->plans[2]);
	if (ret == ANX_OK) ret = anx_phase_resize(&owner->cid, phase.epoch, 8192, 0, &phase);
	if (ret == ANX_OK) ret = denied071(f, f->plans[2].id, ANX_EBUSY);
	if (ret != ANX_OK) goto out;
	ret = anx_logical_graph_read(f->graph.id, 0, &f->response);
	if (ret != ANX_OK || anx_memcmp(f->response.output, "AAAA", 4)) { ret = -7106; goto out; }
	ret = anx_physical_plan_compile(f->graph.id, f->graph.epoch, phase.epoch, ANX_PHYSICAL_REUSE, &f->plans[3]);
	if (ret != ANX_OK) goto out;
	if (f->plans[3].node != 1 || f->plans[3].source_node != 0 || f->plans[3].mode != ANX_PHYSICAL_REUSE) { ret = -7107; goto out; }
	ret = anx_physical_plan_commit(f->plans[3].id, &f->response, &f->graph);
	if (ret != ANX_OK || f->graph.completed != 3 || f->response.tokens_generated || anx_memcmp(f->response.output, "AAAA", 4) ||
	    anx_memcmp(f->program, f->graph.program_digest, 32)) { ret = -7107; goto out; }
	ret = anx_physical_plan_compile(f->graph.id, f->graph.epoch, phase.epoch, ANX_PHYSICAL_REUSE, &f->plans[4]);
	if (ret != ANX_OK) goto out;
	if (f->plans[4].mode != ANX_PHYSICAL_DIRECT || f->plans[4].source_node != 2) { ret = -7108; goto out; }
	prompt->access_policy.rule_count = 1;
	prompt->access_policy.rules[0] = (struct anx_access_rule){ .operations = ANX_ACCESS_READ_PAYLOAD, .effect = ANX_EFFECT_DENY };
	ret = denied071(f, f->plans[4].id, ANX_EPERM);
	prompt->access_policy.rule_count = 0;
	if (ret != ANX_OK) goto out;
	ret = denied071(f, f->plans[4].id, ANX_EBUSY);
	if (ret == ANX_OK) ret = anx_physical_plan_compile(f->graph.id, f->graph.epoch, phase.epoch, ANX_PHYSICAL_REUSE, &f->plans[5]);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch071", active071, f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint, "anxresearch071://inspect", sizeof(f->call.endpoint));
	foreign->ext_call = &f->call; f->foreign = true; ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	owner->ext_call = &f->call; f->foreign = false; ret = anx_cell_run(owner);
	if (ret == ANX_OK) kprintf("day071 logical_nodes=3 direct_calls=2 reused_calls=1 logical_digest=unchanged outputs=equal\n");
out:
	anx_external_unregister_handler("anxresearch071");
	for (uint32_t i = 0; i < 6; i++) if (f->plans[i].id) anx_physical_plan_destroy(f->plans[i].id);
	if (f->graph.id) anx_logical_graph_destroy(f->graph.id);
	if (reference.id) anx_shape_destroy(reference.id);
	for (uint32_t i = 0; i < 7; i++) if (f->uses[i].id) anx_model_use_destroy(f->uses[i].id);
	if (attached) { anx_phase_get(&owner->cid, &phase); anx_phase_finish(&owner->cid, phase.epoch); anx_phase_detach(&owner->cid); }
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	for (uint32_t i = 0; i < 2; i++) if (images[i]) { anx_so_delete(&images[i]->oid, false); anx_objstore_release(images[i]); }
	if (foreign) anx_cell_destroy(foreign);
	if (owner) anx_cell_destroy(owner);
	if (ret != ANX_OK) kprintf("day071 native failure rc=%d\n", ret);
	anx_free(f); return ret;
}
#endif
