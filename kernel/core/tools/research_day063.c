/* Generated control flow selects only privately authorized model groups. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/control_image.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/external_call.h>
#include <anx/kprintf.h>
struct fixture063 {
	struct anx_branch_spec spec;
	struct anx_control_program program;
	struct anx_control_view control;
	struct anx_control_authority authority;
	struct anx_external_call call;
	bool foreign;
	uint32_t calls;
};
static int active063(struct anx_external_call *call, void *arg)
{
	struct fixture063 *f = arg;
	struct anx_control_view out, sentinel;
	struct anx_control_event event;
	char bytes[8]; uint32_t size = 0;
	(void)call; f->calls++;
	anx_memset(&out, 0x55, sizeof(out)); sentinel = out;
	if (anx_control_create(&f->control.owner, &f->control.image, &f->authority, &out) != ANX_EPERM ||
	    anx_control_destroy(f->control.id) != ANX_EPERM || anx_memcmp(&out, &sentinel, sizeof(out))) return -6315;
	if (f->foreign) return anx_control_get(f->control.id, &out) == ANX_EPERM &&
		anx_control_step(f->control.id, f->control.epoch, &out) == ANX_EPERM &&
		anx_control_event_get(f->control.id, 0, &event) == ANX_EPERM &&
		anx_control_read(f->control.id, bytes, sizeof(bytes), &size) == ANX_EPERM &&
		!anx_memcmp(&out, &sentinel, sizeof(out)) ? ANX_OK : -6316;
	for (uint32_t i = 0; i < ANX_CONTROL_STEPS_MAX && f->control.state == ANX_CONTROL_READY; i++) {
		int ret = anx_control_step(f->control.id, f->control.epoch, &f->control);
		if (ret != ANX_OK) return ret;
	}
	return f->control.state == ANX_CONTROL_COMPLETED &&
		anx_control_read(f->control.id, bytes, sizeof(bytes), &size) == ANX_OK && size == 4 &&
		!anx_memcmp(bytes, "BBBB", 4) ? ANX_OK : -6317;
}
static int make_image063(const struct anx_control_program *program, bool seal, struct anx_state_object **out)
{
	struct anx_so_create_params params = { .object_type = ANX_OBJ_STRUCTURED_DATA,
		.schema_uri = ANX_CONTROL_SCHEMA, .schema_version = "1", .payload = program, .payload_size = sizeof(*program) };
	int ret = anx_so_create(&params, out);
	if (ret == ANX_OK && seal) ret = anx_so_seal(&(*out)->oid);
	return ret;
}
static int denied063(const struct anx_control_view *view, int expected)
{
	struct anx_control_view out, sentinel, current;
	anx_memset(&out, 0x55, sizeof(out)); sentinel = out;
	return anx_control_step(view->id, view->epoch, &out) == expected && !anx_memcmp(&out, &sentinel, sizeof(out)) &&
		anx_control_get(view->id, &current) == ANX_OK && current.epoch == view->epoch &&
		current.event_count == view->event_count && current.program_counter == view->program_counter ? ANX_OK : -6310;
}
int anx_research_day063(void)
{
	struct fixture063 *f = anx_zalloc(sizeof(*f));
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *prompt = NULL, *image = NULL, *bad_image = NULL;
	struct anx_branch_view groups[3] = {0};
	struct anx_control_view control = {0}, other = {0}, output, sentinel;
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
	anx_memset(&output, 0x55, sizeof(output)); sentinel = output;
	const struct anx_control_program original = f->program;
	/* Mutate a generated artifact before sealing; the loader must reject it before execution. */
	for (uint32_t i = 0; i < 11; i++) {
		f->program = original;
		int expected = ANX_EINVAL;
		switch (i) {
		case 0: f->program.abi = 2; expected = ANX_ENOTSUP; break;
		case 1: f->program.instructions[0].on_success = 0; break;
		case 2: f->program.instructions[0].opcode = 999; expected = ANX_ENOTSUP; break;
		case 3: f->program.instructions[4].slot = 2; expected = ANX_EPERM; break;
		case 4: f->program.instructions[1].opcode = ANX_CONTROL_READ; break;
		case 5: f->program.instructions[0] = original.instructions[3]; break;
		case 6: f->program.maximum_steps = 6; break;
		case 7: f->program.instructions[4].slot = 0; break;
		case 8: f->program.instructions[15].opcode = ANX_CONTROL_FAIL; break;
		case 9: f->program.reserved = 1; break;
		case 10: f->program.instructions[6].branch = ANX_BRANCH_MAX; break;
		}
		ret = make_image063(&f->program, true, &bad_image);
		if (ret != ANX_OK) goto out;
		ret = -6302;
		if (anx_control_create(&owner->cid, &bad_image->oid, &authority, &output) != expected ||
		    anx_memcmp(&output, &sentinel, sizeof(output))) { kprintf("day063 invalid case=%u\n", i); goto out; }
		anx_so_delete(&bad_image->oid, false); anx_objstore_release(bad_image); bad_image = NULL;
	}
	f->program = original;
	ret = make_image063(&f->program, false, &bad_image);
	if (ret != ANX_OK) goto out;
	ret = -6303;
	if (anx_control_create(&owner->cid, &bad_image->oid, &authority, &output) != ANX_EINVAL) goto out;
	anx_so_delete(&bad_image->oid, false); anx_objstore_release(bad_image); bad_image = NULL;
	struct anx_control_authority changed = authority;
	changed.allowed_operations &= ~(1U << ANX_CONTROL_READ);
	if (anx_control_create(&owner->cid, &image->oid, &changed, &output) != ANX_EPERM) goto out;
	changed = authority; changed.maximum_steps = 6;
	if (anx_control_create(&owner->cid, &image->oid, &changed, &output) != ANX_EINVAL) goto out;
	changed = authority; changed.groups[1] = changed.groups[0];
	if (anx_control_create(&owner->cid, &image->oid, &changed, &output) != ANX_EINVAL ||
	    anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	foreign->execution.allow_recursive_cells = foreign->execution.allow_side_effects = true;
	ret = anx_branch_group_create(&foreign->cid, &f->spec, &groups[2]);
	if (ret != ANX_OK) goto out;
	changed = authority; changed.groups[0] = groups[2].id;
	ret = -6304;
	if (anx_control_create(&owner->cid, &image->oid, &changed, &output) != ANX_EPERM ||
	    anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	ret = anx_branch_group_destroy(groups[2].id); groups[2].id = 0;
	if (ret != ANX_OK) goto out;
	/* An explicit terminal failure is a valid, inspectable control decision. */
	anx_memset(&f->program, 0, sizeof(f->program));
	f->program.abi = f->program.count = f->program.maximum_steps = 1;
	f->program.instructions[0] = original.instructions[8];
	ret = make_image063(&f->program, true, &bad_image);
	if (ret == ANX_OK) ret = anx_control_create(&owner->cid, &bad_image->oid, &authority, &other);
	if (ret == ANX_OK) ret = anx_control_step(other.id, other.epoch, &other);
	if (ret != ANX_OK) goto out;
	char bytes[8]; uint32_t size = 0x5555;
	ret = -6305;
	if (other.state != ANX_CONTROL_FAILED || other.event_count != 1 || other.result != ANX_EIO ||
	    anx_control_read(other.id, bytes, sizeof(bytes), &size) != ANX_EPERM || size != 0x5555) goto out;
	ret = anx_control_destroy(other.id); other.id = 0;
	anx_so_delete(&bad_image->oid, false); anx_objstore_release(bad_image); bad_image = NULL;
	if (ret != ANX_OK) goto out;
	f->program = original;
	/* Both controls bind the same initial group epochs; only one may consume them. */
	ret = anx_control_create(&owner->cid, &image->oid, &authority, &other);
	if (ret != ANX_OK) goto out;
	ret = -6306;
	if (anx_control_step(control.id, control.epoch + 1, &output) != ANX_EBUSY ||
	    anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	image->access_policy.rule_count = 1; image->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	image->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	ret = denied063(&control, ANX_EPERM);
	image->access_policy.rule_count = 0;
	if (ret != ANX_OK) goto out;
	((uint8_t *)image->payload)[20] ^= 1;
	ret = denied063(&control, ANX_EBUSY); ((uint8_t *)image->payload)[20] ^= 1;
	if (ret != ANX_OK) goto out;
	/* Changing caller-owned copies cannot change the issued image or its authority. */
	f->program.instructions[4].slot = 0;
	changed = authority; authority.groups[1] = authority.groups[0];
	ret = anx_control_step(control.id, control.epoch, &control);
	authority = changed;
	if (ret != ANX_OK) goto out;
	ret = -6307;
	if (control.state != ANX_CONTROL_READY || control.program_counter != 4 || control.event_count != 1 ||
	    control.result != ANX_EIO || control.epoch != 2 || denied063(&other, ANX_EBUSY) != ANX_OK) goto out;
	struct anx_control_event first, after;
	if (anx_control_event_get(control.id, 0, &first) != ANX_OK || first.instruction != 0 || first.next_instruction != 4 ||
	    first.result != ANX_EIO) goto out;
	ret = anx_control_destroy(other.id); other.id = 0;
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint, "anxresearch063://control", sizeof(f->call.endpoint));
	ret = anx_external_register_handler("anxresearch063", active063, f);
	if (ret != ANX_OK) goto out;
	f->control = control; f->authority = authority; f->foreign = true;
	foreign->ext_call = &f->call;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	f->foreign = false; owner->cell_type = ANX_CELL_TASK_EXTERNAL_CALL;
	owner->execution.allow_side_effects = true; owner->ext_call = &f->call;
	ret = anx_cell_run(owner);
	control = f->control;
	if (ret != ANX_OK) goto out;
	ret = -6308;
	if (f->calls != 2 || control.state != ANX_CONTROL_COMPLETED || control.event_count != 5 || control.epoch != 6 ||
	    control.program_counter != ANX_CONTROL_HALT || control.result_size != 4 || control.result != ANX_OK ||
	    anx_control_event_get(control.id, 0, &after) != ANX_OK || anx_memcmp(&first, &after, sizeof(first))) goto out;
	const uint32_t path[5] = {0, 4, 5, 6, 7};
	for (uint32_t i = 0; i < 5; i++) {
		if (anx_control_event_get(control.id, i, &after) != ANX_OK || after.sequence != i + 1 || after.instruction != path[i] ||
		    after.next_instruction != (i == 4 ? ANX_CONTROL_HALT : path[i + 1]) || after.result != (i ? ANX_OK : ANX_EIO)) goto out;
	}
	anx_memset(bytes, 0x55, sizeof(bytes)); size = 0x5555;
	if (anx_control_read(control.id, bytes, 3, &size) != ANX_ENOMEM || bytes[0] != 0x55 || size != 0x5555 ||
	    anx_control_read(control.id, bytes, sizeof(bytes), &size) != ANX_OK || size != 4 || anx_memcmp(bytes, "BBBB", 4) ||
	    anx_control_step(control.id, control.epoch, &output) != ANX_EBUSY ||
	    anx_control_event_get(control.id, 5, &after) != ANX_ENOENT) goto out;
	prompt->access_policy.rule_count = 1; prompt->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	prompt->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	int denied = anx_control_read(control.id, bytes, sizeof(bytes), &size);
	prompt->access_policy.rule_count = 0;
	if (denied != ANX_EPERM) goto out;
	ret = ANX_OK;
out:
	if (ret != ANX_OK) kprintf("day063 failure rc=%d\n", ret);
	anx_external_unregister_handler("anxresearch063");
	if (other.id) anx_control_destroy(other.id);
	if (control.id && anx_control_destroy(control.id) != ANX_OK && ret == ANX_OK) ret = -6311;
	for (uint32_t i = 0; i < 3; i++) if (groups[i].id && anx_branch_group_destroy(groups[i].id) != ANX_OK && ret == ANX_OK) ret = -6312;
	if (foreign && anx_cell_destroy(foreign) != ANX_OK && ret == ANX_OK) ret = -6313;
	if (owner && anx_cell_destroy(owner) != ANX_OK && ret == ANX_OK) ret = -6314;
	if (bad_image) { anx_so_delete(&bad_image->oid, false); anx_objstore_release(bad_image); }
	if (image) { anx_so_delete(&image->oid, false); anx_objstore_release(image); }
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	anx_free(f);
	return ret;
}
#endif
