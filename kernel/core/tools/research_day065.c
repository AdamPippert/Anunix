/* Compare literal and safely coalesced native model execution graphs. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/execution_shape.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/external_call.h>
#include <anx/kprintf.h>
struct fixture065 {
	struct anx_model_use_view uses[12];
	struct anx_shape_view shape;
	struct anx_shape_spec spec;
	struct anx_anxml_response response, sentinel;
	struct anx_external_call call;
	bool foreign;
	uint32_t calls;
};
static int active065(struct anx_external_call *call, void *arg)
{
	struct fixture065 *f = arg;
	struct anx_shape_view out, sentinel;
	(void)call; f->calls++;
	anx_memset(&out, 0x55, sizeof(out)); sentinel = out;
	if (anx_shape_compile(&f->shape.owner, &f->spec, &out) != ANX_EPERM || anx_shape_destroy(f->shape.id) != ANX_EPERM) return -6510;
	if (f->foreign) {
		anx_memset(&f->response, 0x55, sizeof(f->response)); f->sentinel = f->response;
		return anx_shape_get(f->shape.id, &out) == ANX_EPERM && anx_shape_run(f->shape.id, f->shape.epoch, &out) == ANX_EPERM &&
			anx_shape_read(f->shape.id, 0, &f->response) == ANX_EPERM &&
			anx_model_use_read(f->uses[0].id, &f->response) == ANX_EPERM &&
			anx_model_use_reuse(f->uses[3].id, f->uses[3].epoch, f->uses[0].id, &f->response, &f->uses[11]) == ANX_EPERM &&
			!anx_memcmp(&out, &sentinel, sizeof(out)) && !anx_memcmp(&f->response, &f->sentinel, sizeof(f->response)) ? ANX_OK : -6511;
	}
	int ret = anx_shape_run(f->shape.id, f->shape.epoch, &f->shape);
	if (ret != ANX_OK || f->shape.state != ANX_SHAPE_COMPLETED) return -6512;
	for (uint32_t i = 0; i < 3; i++) {
		ret = anx_shape_read(f->shape.id, i, &f->response);
		if (ret != ANX_OK || f->response.output_len != 4 || anx_memcmp(f->response.output, i == 2 ? "BBBB" : "AAAA", 4)) return -6513;
	}
	return ANX_OK;
}
int anx_research_day065(void)
{
	struct fixture065 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *images[2] = {0}, *prompt = NULL;
	struct anx_adapter_image adapter = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_model_use_spec model = { .maximum_tokens = 4 };
	struct anx_shape_spec spec = { .count = 3, .mode = ANX_SHAPE_LITERAL };
	struct anx_shape_view shapes[2] = {0}, other = {0}, output, sentinel;
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA,
		.schema_uri = ANX_MODEL_USE_SCHEMA, .schema_version = "1", .payload = &adapter, .payload_size = sizeof(adapter) };
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-065", sizeof(intent.name));
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
	model.prompt = prompt->oid;
	for (uint32_t i = 0; i < 6; i++) {
		model.image = images[i % 3 == 2 ? 1 : 0]->oid;
		ret = anx_model_use_prepare(&owner->cid, &model, &f->uses[i]);
		if (ret != ANX_OK) goto out;
	}
	for (uint32_t i = 0; i < 3; i++) spec.nodes[i] = (struct anx_shape_node){ f->uses[i].id, i == 2 ? 3 : 0, ANX_SHAPE_INFER };
	ret = -6501;
	if (anx_shape_compile(&owner->cid, &spec, &shapes[0]) != ANX_OK) goto out;
	spec.mode = ANX_SHAPE_REUSE;
	for (uint32_t i = 0; i < 3; i++) spec.nodes[i].use = f->uses[i + 3].id;
	ret = anx_shape_compile(&owner->cid, &spec, &shapes[1]);
	if (ret != ANX_OK) goto out;
	ret = -6502;
	if (shapes[0].planned_physical_operations != 3 || shapes[1].planned_physical_operations != 2 ||
	    shapes[1].source_node[0] || shapes[1].source_node[1] || shapes[1].source_node[2] != 2) goto out;
	anx_memset(&output, 0x55, sizeof(output)); sentinel = output;
	for (uint32_t i = 0; i < 6; i++) {
		struct anx_shape_spec invalid = spec;
		int expected = ANX_EINVAL;
		switch (i) {
		case 0: invalid.count = 0; break;
		case 1: invalid.count = ANX_SHAPE_NODES_MAX + 1; break;
		case 2: invalid.nodes[0].dependencies = 2; break;
		case 3: invalid.nodes[2].dependencies = 4; break;
		case 4: invalid.nodes[1].use = invalid.nodes[0].use; break;
		case 5: invalid.nodes[1].opcode = 99; expected = ANX_ENOTSUP; break;
		}
		if (anx_shape_compile(&owner->cid, &invalid, &output) != expected || anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	}
	/* Equivalent values on different dependency boundaries are deliberately left separate. */
	spec.nodes[1].dependencies = 1;
	ret = anx_shape_compile(&owner->cid, &spec, &other);
	if (ret != ANX_OK) goto out;
	ret = -6503;
	if (other.planned_physical_operations != 3) goto out;
	ret = anx_shape_destroy(other.id); other.id = 0;
	if (ret != ANX_OK) goto out;
	spec.nodes[1].dependencies = 0;
	/* A stochastic request is never coalesced, even when the seed matches. */
	model.image = images[0]->oid; model.seed = 7;
	for (uint32_t i = 6; i < 8; i++) {
		ret = anx_model_use_prepare(&owner->cid, &model, &f->uses[i]);
		if (ret != ANX_OK) goto out;
	}
	struct anx_shape_spec seeded = { .count = 2, .mode = ANX_SHAPE_REUSE,
		.nodes = {{f->uses[6].id,0,ANX_SHAPE_INFER},{f->uses[7].id,0,ANX_SHAPE_INFER}} };
	ret = anx_shape_compile(&owner->cid, &seeded, &other);
	if (ret != ANX_OK) goto out;
	ret = -6504;
	if (other.planned_physical_operations != 2) goto out;
	/* A record consumed outside the compiled plan invalidates its execution preflight. */
	ret = anx_model_use_execute(f->uses[6].id, f->uses[6].epoch, &f->response, &f->uses[6]);
	if (ret != ANX_OK) goto out;
	ret = -6504;
	if (anx_shape_run(other.id, other.epoch, &output) != ANX_EBUSY || anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	ret = anx_shape_destroy(other.id); other.id = 0;
	if (ret != ANX_OK) goto out;
	model.seed = 0;
	ret = anx_model_use_prepare(&foreign->cid, &model, &f->uses[8]);
	if (ret != ANX_OK) goto out;
	struct anx_shape_spec single = { .count = 1, .mode = ANX_SHAPE_REUSE, .nodes = {{f->uses[8].id,0,ANX_SHAPE_INFER}} };
	ret = -6505;
	if (anx_shape_compile(&owner->cid, &single, &output) != ANX_EPERM) goto out;
	/* Recheck authority at run time; a failed plan exposes no logical outputs. */
	ret = anx_model_use_prepare(&owner->cid, &model, &f->uses[9]);
	if (ret != ANX_OK) goto out;
	single.nodes[0].use = f->uses[9].id;
	ret = anx_shape_compile(&owner->cid, &single, &other);
	if (ret != ANX_OK) goto out;
	prompt->access_policy.rule_count = 1; prompt->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	prompt->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	ret = anx_shape_run(other.id, other.epoch, &other);
	prompt->access_policy.rule_count = 0;
	if (ret != ANX_OK) goto out;
	ret = -6506;
	if (other.state != ANX_SHAPE_FAILED || other.result != ANX_EPERM || other.completed || other.physical_operations ||
	    anx_shape_read(other.id, 0, &f->response) != ANX_EPERM || anx_shape_run(other.id, other.epoch, &output) != ANX_EBUSY) goto out;
	ret = anx_shape_destroy(other.id); other.id = 0;
	if (ret != ANX_OK) goto out;
	ret = anx_shape_run(shapes[0].id, shapes[0].epoch, &shapes[0]);
	if (ret != ANX_OK) goto out;
	ret = -6507;
	if (shapes[0].state != ANX_SHAPE_COMPLETED || shapes[0].physical_operations != 3 || shapes[0].reused_operations ||
	    shapes[0].generated_tokens != 12 || shapes[0].completed != 7) goto out;
	/* A completed value cannot be borrowed for a different image. */
	anx_memset(&f->response, 0x55, sizeof(f->response)); f->sentinel = f->response;
	if (anx_model_use_reuse(f->uses[5].id, f->uses[5].epoch, f->uses[0].id, &f->response, &f->uses[11]) != ANX_EPERM ||
	    anx_memcmp(&f->response, &f->sentinel, sizeof(f->response))) goto out;
	f->shape = shapes[1]; f->spec = spec;
	anx_strlcpy(f->call.endpoint, "anxresearch065://shape", sizeof(f->call.endpoint));
	ret = anx_external_register_handler("anxresearch065", active065, f);
	if (ret != ANX_OK) goto out;
	f->foreign = true; foreign->ext_call = &f->call;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	/* The issued graph remains unchanged when the caller edits its compilation input. */
	spec.count = 1; spec.nodes[0].use = f->uses[8].id;
	f->foreign = false; owner->ext_call = &f->call;
	ret = anx_cell_run(owner); shapes[1] = f->shape;
	if (ret != ANX_OK) goto out;
	ret = -6508;
	if (f->calls != 2 || shapes[1].logical_operations != 3 || shapes[1].physical_operations != 2 ||
	    shapes[1].reused_operations != 1 || shapes[1].generated_tokens != 8 || shapes[1].completed != 7 || shapes[1].epoch != 2) goto out;
	struct anx_model_use_view reused;
	if (anx_model_use_get(f->uses[4].id, &reused) != ANX_OK || reused.reused_from != f->uses[3].id ||
	    reused.state != ANX_MODEL_USE_COMPLETED || reused.tokens_generated) goto out;
	for (uint32_t i = 0; i < 3; i++) {
		if (anx_shape_read(shapes[0].id, i, &f->response) != ANX_OK) goto out;
		f->sentinel = f->response;
		if (anx_shape_read(shapes[1].id, i, &f->response) != ANX_OK || f->response.output_len != f->sentinel.output_len ||
		    anx_memcmp(f->response.output, f->sentinel.output, f->response.output_len)) goto out;
	}
	prompt->access_policy.rule_count = 1; prompt->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	prompt->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	int denied = anx_shape_read(shapes[1].id, 1, &f->response);
	prompt->access_policy.rule_count = 0;
	if (denied != ANX_EPERM) goto out;
	kprintf("day065 comparison logical=3 literal_compute=%u reused_compute=%u literal_tokens=%u reused_tokens=%u outputs=equal\n",
		shapes[0].physical_operations, shapes[1].physical_operations, shapes[0].generated_tokens, shapes[1].generated_tokens);
	ret = ANX_OK;
out:
	anx_external_unregister_handler("anxresearch065");
	if (other.id) anx_shape_destroy(other.id);
	for (uint32_t i = 0; i < 2; i++) if (shapes[i].id && anx_shape_destroy(shapes[i].id) != ANX_OK && ret == ANX_OK) ret = -6514;
	if (f) for (uint32_t i = 0; i < 12; i++) if (f->uses[i].id && anx_model_use_destroy(f->uses[i].id) != ANX_OK && ret == ANX_OK) ret = -6515;
	if (foreign && anx_cell_destroy(foreign) != ANX_OK && ret == ANX_OK) ret = -6516;
	if (owner && anx_cell_destroy(owner) != ANX_OK && ret == ANX_OK) ret = -6517;
	for (uint32_t i = 0; i < 2; i++) if (images[i]) { anx_so_delete(&images[i]->oid, false); anx_objstore_release(images[i]); }
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	anx_free(f);
	if (ret != ANX_OK) kprintf("day065 failure rc=%d\n", ret);
	return ret;
}
#endif
