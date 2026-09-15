/* Generated control flow selects only privately authorized model groups. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/control_image.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
struct fixture063 {
	struct anx_branch_spec spec;
	struct anx_control_program program;
};
int anx_research_day063(void)
{
	struct fixture063 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *prompt = NULL, *image = NULL;
	struct anx_branch_view groups[2] = {0};
	struct anx_control_view control = {0};
	struct anx_control_authority authority = { .allowed_operations = (1U << ANX_CONTROL_OPCODE_COUNT) - 1,
		.maximum_steps = 7, .group_count = 2 };
	struct anx_so_create_params params = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "~", .payload_size = 1 };
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-063", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_recursive_cells = true;
	f->spec.schema = 1; f->spec.count = 1; f->spec.token_budget = 4;
	f->spec.semantics = ANX_BRANCH_TRIAL; f->spec.prompt = prompt->oid;
	for (uint32_t i = 0; i < 2; i++) {
		uint8_t c = (uint8_t)('A' + i);
		f->spec.candidates[0].image = (struct anx_adapter_image){ .format = 1, .count = 2, .deltas = {{'~',c,4096},{c,c,4096}} };
		f->spec.candidates[0].maximum_tokens = f->spec.candidates[0].expected_size = 4;
		anx_memset(f->spec.candidates[0].expected, i ? 'B' : 'Z', 4);
		ret = anx_branch_group_create(&owner->cid, &f->spec, &groups[i]);
		if (ret != ANX_OK) goto out;
		authority.groups[i] = groups[i].id;
	}
	f->program.abi = 1; f->program.count = 9; f->program.maximum_steps = 7;
	f->program.instructions[0] = (struct anx_control_instruction){ ANX_CONTROL_RUN, 0, 0, 1, 4 };
	f->program.instructions[1] = (struct anx_control_instruction){ ANX_CONTROL_VERIFY, 0, 0, 2, 4 };
	f->program.instructions[2] = (struct anx_control_instruction){ ANX_CONTROL_READ, 0, ANX_CONTROL_HALT, 3, 4 };
	f->program.instructions[3] = (struct anx_control_instruction){ ANX_CONTROL_RETURN, 0, 0, ANX_CONTROL_HALT, ANX_CONTROL_HALT };
	f->program.instructions[4] = (struct anx_control_instruction){ ANX_CONTROL_RUN, 1, 0, 5, 8 };
	f->program.instructions[5] = (struct anx_control_instruction){ ANX_CONTROL_VERIFY, 1, 0, 6, 8 };
	f->program.instructions[6] = (struct anx_control_instruction){ ANX_CONTROL_READ, 1, ANX_CONTROL_HALT, 7, 8 };
	f->program.instructions[7] = f->program.instructions[3];
	f->program.instructions[8] = (struct anx_control_instruction){ ANX_CONTROL_FAIL, 0, 0, ANX_CONTROL_HALT, ANX_CONTROL_HALT };
	params.object_type = ANX_OBJ_STRUCTURED_DATA; params.schema_uri = ANX_CONTROL_SCHEMA; params.schema_version = "1";
	params.payload = &f->program; params.payload_size = sizeof(f->program);
	ret = anx_so_create(&params, &image);
	if (ret == ANX_OK) ret = anx_so_seal(&image->oid);
	if (ret != ANX_OK) goto out;
	ret = -6301;
	if (anx_control_create(&owner->cid, &image->oid, &authority, &control) != ANX_OK) goto out;
	ret = ANX_OK;
out:
	if (control.id) anx_control_destroy(control.id);
	for (uint32_t i = 0; i < 2; i++) if (groups[i].id) anx_branch_group_destroy(groups[i].id);
	if (owner) anx_cell_destroy(owner);
	if (image) { anx_so_delete(&image->oid, false); anx_objstore_release(image); }
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	anx_free(f);
	return ret;
}
#endif
