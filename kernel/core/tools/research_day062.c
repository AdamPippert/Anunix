/* Required results and competing trials obey different completion rules. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/branch_group.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/sched.h>
#include <anx/effect_fence.h>
#include <anx/external_operation.h>
#include <anx/kprintf.h>

struct fixture062 {
	struct anx_branch_spec spec;
	struct anx_branch_view group;
	struct anx_external_call call;
	bool foreign;
	uint32_t calls;
};
static void configure062(struct anx_branch_spec *s, const anx_oid_t *prompt, enum anx_branch_semantics semantics,
		uint32_t count, uint32_t budget)
{
	anx_memset(s, 0, sizeof(*s));
	s->schema = 1; s->prompt = *prompt; s->semantics = semantics; s->count = count; s->token_budget = budget;
	for (uint32_t i = 0; i < count && i < ANX_BRANCH_MAX; i++) {
		char c = (char)('A' + i);
		s->candidates[i].image = (struct anx_adapter_image){ .format = 1, .count = 2,
			.deltas = {{'~',(uint8_t)c,4096},{(uint8_t)c,(uint8_t)c,4096}} };
		s->candidates[i].maximum_tokens = s->candidates[i].expected_size = 4;
		anx_memset(s->candidates[i].expected, c, 4);
	}
}
static int active062(struct anx_external_call *call, void *arg)
{
	struct fixture062 *f = arg;
	struct anx_branch_view out, sentinel;
	char result[4]; uint32_t size = 0;
	(void)call; f->calls++;
	anx_memset(&out, 0x55, sizeof(out)); sentinel = out;
	if (anx_branch_group_create(&f->group.owner, &f->spec, &out) != ANX_EPERM ||
	    anx_branch_group_destroy(f->group.id) != ANX_EPERM || anx_memcmp(&out, &sentinel, sizeof(out))) return -6215;
	if (f->foreign) {
		return anx_branch_group_get(f->group.id, &out) == ANX_EPERM &&
			anx_branch_group_run(f->group.id, f->group.epoch, &out) == ANX_EPERM &&
			anx_branch_group_abort(f->group.id, f->group.epoch) == ANX_EPERM &&
			anx_branch_group_read(f->group.id, 0, result, sizeof(result), &size) == ANX_EPERM &&
			!anx_memcmp(&out, &sentinel, sizeof(out)) ? ANX_OK : -6216;
	}
	int ret = anx_branch_group_run(f->group.id, f->group.epoch, &f->group);
	if (ret == ANX_OK && (f->group.state != ANX_BRANCH_COMPLETED || f->group.winner != 0 ||
	    anx_branch_group_read(f->group.id, 0, result, sizeof(result), &size) != ANX_OK ||
	    size != 4 || anx_memcmp(result, "AAAA", 4))) ret = -6217;
	return ret;
}
static int destroy062(struct anx_branch_view *g)
{
	int ret = anx_branch_group_destroy(g->id);
	if (ret == ANX_OK) anx_memset(g, 0, sizeof(*g));
	return ret;
}
static bool status062(const anx_cid_t *id, enum anx_cell_status status)
{
	struct anx_cell *c = anx_cell_store_lookup(id);
	bool valid = c && c->status == status;
	if (c) anx_cell_store_release(c);
	return valid;
}
int anx_research_day062(void)
{
	struct anx_cell *owner = NULL, *foreign = NULL, *member_cell = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_state_object *prompt = NULL, *object = NULL;
	struct anx_so_create_params params = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "~", .payload_size = 1 };
	struct fixture062 *f = anx_zalloc(sizeof(*f));
	struct anx_branch_view group = {0}, output, sentinel;
	struct anx_effect_fence_view fence, current_fence;
	struct anx_pending_effect *pending = NULL;
	struct anx_object_handle writer = {0};
	anx_oid_t operation = ANX_UUID_NIL;
	char bytes[8]; uint32_t size;
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	struct anx_branch_spec *spec = &f->spec;
	anx_strlcpy(intent.name, "research-day-062", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_so_create(&params, &prompt);
	if (ret == ANX_OK) ret = anx_so_seal(&prompt->oid);
	if (ret != ANX_OK) goto out;
	owner->execution.allow_recursive_cells = true;
	owner->execution.allow_side_effects = true;
	ret = anx_effect_fence_create(&fence);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(owner, &fence.id);
	if (ret != ANX_OK) goto out;
	configure062(spec, &prompt->oid, ANX_BRANCH_TRIAL, 3, 8);
	anx_memcpy(spec->candidates[0].expected, "ZZZZ", 4);
	ret = -6201;
	if (anx_branch_group_create(&owner->cid, spec, &group) != ANX_OK) goto out;
	anx_memset(&output, 0x55, sizeof(output)); sentinel = output;
	ret = -6202;
	if (anx_branch_group_run(group.id, group.epoch + 1, &output) != ANX_EBUSY ||
	    anx_memcmp(&output, &sentinel, sizeof(output)) || anx_cell_destroy(owner) != ANX_EBUSY) goto out;
	/* Even an edited public side-effect flag cannot authorize a pure branch. */
	member_cell = anx_cell_store_lookup(&group.branches[0]);
	if (!member_cell) { ret = ANX_ENOENT; goto out; }
	member_cell->execution.allow_side_effects = true;
	ret = -6203;
	if (anx_effect_prepare(member_cell->cid, NULL, NULL, &pending) != ANX_EPERM || pending ||
	    anx_cell_destroy(member_cell) != ANX_EBUSY) goto out;
	params.payload = "original"; params.payload_size = 8;
	ret = anx_so_create(&params, &object);
	if (ret == ANX_OK) ret = anx_so_open(&object->oid, ANX_OPEN_READWRITE, &writer);
	if (ret == ANX_OK) ret = anx_object_stage(&writer, member_cell->cid);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&writer, "replaced", 8);
	if (ret != ANX_OK) goto out;
	ret = -6204;
	if (anx_object_commit(&writer) != ANX_EPERM || anx_memcmp(object->payload, "original", 8)) goto out;
	ret = anx_object_abort(&writer);
	member_cell->execution.allow_side_effects = false;
	anx_cell_store_release(member_cell); member_cell = NULL;
	if (ret != ANX_OK) goto out;
	ret = anx_sched_enqueue(&group.branches[2], ANX_QUEUE_BATCH, ANX_PRIO_LOW);
	if (ret != ANX_OK) goto out;
	/* Private candidate specifications survive changes to the caller's copy. */
	anx_memset(spec->candidates[1].expected, 'Z', 4);
	ret = anx_branch_group_run(group.id, group.epoch, &group);
	if (ret != ANX_OK) goto out;
	ret = -6205;
	if (group.state != ANX_BRANCH_COMPLETED || group.winner != 1 || group.attempted != 3 || group.accepted != 2 ||
	    group.cancelled != 4 || group.charged_tokens != 8 || group.result != ANX_OK ||
	    !status062(&group.branches[0], ANX_CELL_FAILED) || !status062(&group.branches[1], ANX_CELL_COMPLETED) ||
	    !status062(&group.branches[2], ANX_CELL_CANCELLED) || anx_sched_queue_depth(ANX_QUEUE_BATCH) ||
	    anx_effect_fence_get(&fence.id, &current_fence) != ANX_OK || current_fence.state != ANX_FENCE_RUNNING ||
	    current_fence.read_count < 2) goto out;
	anx_memset(bytes, 0x55, sizeof(bytes)); size = 0x5555;
	if (anx_branch_group_read(group.id, 0, bytes, sizeof(bytes), &size) != ANX_EPERM ||
	    anx_branch_group_read(group.id, 2, bytes, sizeof(bytes), &size) != ANX_EPERM ||
	    anx_branch_group_read(group.id, 1, bytes, 3, &size) != ANX_ENOMEM || size != 0x5555 || bytes[0] != 0x55 ||
	    anx_branch_group_read(group.id, 1, bytes, sizeof(bytes), &size) != ANX_OK || size != 4 || anx_memcmp(bytes, "BBBB", 4)) goto out;
	prompt->access_policy.rule_count = 1; prompt->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	prompt->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	int denied = anx_branch_group_read(group.id, 1, bytes, sizeof(bytes), &size);
	prompt->access_policy.rule_count = 0;
	if (denied != ANX_EPERM) goto out;
	((char *)prompt->payload)[0] ^= 1;
	denied = anx_branch_group_read(group.id, 1, bytes, sizeof(bytes), &size);
	((char *)prompt->payload)[0] ^= 1;
	if (denied != ANX_EBUSY || anx_branch_group_abort(group.id, group.epoch) != ANX_EBUSY) goto out;
	ret = destroy062(&group);
	if (ret != ANX_OK) goto out;
	/* Required groups retain every accepted output and never cancel a successful peer. */
	configure062(spec, &prompt->oid, ANX_BRANCH_REQUIRED, 2, 8);
	ret = anx_branch_group_create(&owner->cid, spec, &group);
	if (ret == ANX_OK) ret = anx_branch_group_run(group.id, group.epoch, &group);
	if (ret != ANX_OK) goto out;
	ret = -6206;
	if (group.state != ANX_BRANCH_COMPLETED || group.attempted != 3 || group.accepted != 3 || group.cancelled ||
	    group.charged_tokens != 8 || group.winner != ~(uint32_t)0 ||
	    anx_branch_group_read(group.id, 0, bytes, sizeof(bytes), &size) != ANX_OK || anx_memcmp(bytes, "AAAA", 4) ||
	    anx_branch_group_read(group.id, 1, bytes, sizeof(bytes), &size) != ANX_OK || anx_memcmp(bytes, "BBBB", 4)) goto out;
	ret = destroy062(&group);
	if (ret != ANX_OK) goto out;
	configure062(spec, &prompt->oid, ANX_BRANCH_REQUIRED, 3, 12);
	anx_memset(spec->candidates[1].expected, 'Z', 4);
	ret = anx_branch_group_create(&owner->cid, spec, &group);
	if (ret == ANX_OK) ret = anx_branch_group_run(group.id, group.epoch, &group);
	if (ret != ANX_OK) goto out;
	ret = -6207;
	if (group.state != ANX_BRANCH_FAILED || group.result != ANX_EIO || group.accepted != 1 || group.attempted != 3 ||
	    group.cancelled || !status062(&group.branches[2], ANX_CELL_CREATED) ||
	    anx_branch_group_read(group.id, 0, bytes, sizeof(bytes), &size) != ANX_EPERM ||
	    anx_branch_group_abort(group.id, group.epoch) != ANX_OK ||
	    !status062(&group.branches[2], ANX_CELL_CANCELLED)) goto out;
	ret = destroy062(&group);
	if (ret != ANX_OK) goto out;
	configure062(spec, &prompt->oid, ANX_BRANCH_TRIAL, 3, 6);
	anx_memset(spec->candidates[0].expected, 'Z', 4);
	ret = anx_branch_group_create(&owner->cid, spec, &group);
	if (ret == ANX_OK) ret = anx_branch_group_run(group.id, group.epoch, &group);
	if (ret != ANX_OK) goto out;
	ret = -6208;
	if (group.state != ANX_BRANCH_FAILED || group.result != ANX_EFULL || group.accepted || group.attempted != 1 ||
	    group.cancelled != 6 || group.charged_tokens != 4) goto out;
	ret = destroy062(&group);
	if (ret != ANX_OK) goto out;
	/* A child's source permission is checked independently of the parent's. */
	configure062(spec, &prompt->oid, ANX_BRANCH_TRIAL, 2, 8);
	ret = anx_branch_group_create(&owner->cid, spec, &group);
	if (ret != ANX_OK) goto out;
	prompt->access_policy.rule_count = 1; prompt->access_policy.rules[0].principal = group.branches[0];
	ret = anx_branch_group_run(group.id, group.epoch, &group);
	prompt->access_policy.rule_count = 0; prompt->access_policy.rules[0].principal = ANX_UUID_NIL;
	if (ret != ANX_OK) goto out;
	ret = -6209;
	if (group.state != ANX_BRANCH_FAILED || group.result != ANX_EPERM || group.accepted || group.attempted != 1 || group.cancelled != 2) goto out;
	ret = destroy062(&group);
	if (ret != ANX_OK) goto out;
	/* Direct calls cannot turn a pure group member into an external task. */
	ret = anx_branch_group_create(&owner->cid, spec, &group);
	if (ret != ANX_OK) goto out;
	member_cell = anx_cell_store_lookup(&group.branches[0]);
	if (!member_cell) { ret = ANX_ENOENT; goto out; }
	anx_strlcpy(f->call.endpoint, "anxresearch062://group", sizeof(f->call.endpoint));
	ret = anx_external_register_handler("anxresearch062", active062, f);
	if (ret != ANX_OK) goto out;
	member_cell->cell_type = ANX_CELL_TASK_EXTERNAL_CALL; member_cell->ext_call = &f->call;
	member_cell->execution.allow_side_effects = true;
	ret = -6210;
	if (anx_external_operation_prepare(&member_cell->cid, &f->call, NULL, NULL, &operation) != ANX_EPERM ||
	    !anx_uuid_is_nil(&operation) || anx_cell_run(member_cell) != ANX_EPERM || f->calls) goto out;
	anx_cell_store_release(member_cell); member_cell = NULL;
	ret = destroy062(&group);
	if (ret != ANX_OK) goto out;
	/* Failed construction returns child slots and preserves caller output. */
	owner->constraints.max_child_cells = 1;
	denied = anx_branch_group_create(&owner->cid, spec, &output);
	owner->constraints.max_child_cells = ANX_MAX_CHILD_CELLS;
	ret = -6211;
	if (denied != ANX_ENOMEM || owner->child_count || anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	owner->cognitive.max_tokens = 2;
	denied = anx_branch_group_create(&owner->cid, spec, &output);
	owner->cognitive.max_tokens = 0;
	if (denied != ANX_EPERM || owner->child_count || anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	spec->semantics = ANX_BRANCH_REQUIRED; spec->token_budget = 7;
	if (anx_branch_group_create(&owner->cid, spec, &output) != ANX_ENOMEM) goto out;
	spec->count = ANX_BRANCH_MAX + 1;
	if (anx_branch_group_create(&owner->cid, spec, &output) != ANX_EINVAL) goto out;
	/* A foreign actor cannot control the group; its actual parent can execute it. */
	configure062(spec, &prompt->oid, ANX_BRANCH_TRIAL, 2, 8);
	ret = anx_branch_group_create(&owner->cid, spec, &group);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	f->group = group; f->foreign = true;
	foreign->ext_call = &f->call; foreign->execution.allow_side_effects = true;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	f->foreign = false; owner->cell_type = ANX_CELL_TASK_EXTERNAL_CALL; owner->ext_call = &f->call;
	ret = anx_cell_run(owner);
	group = f->group;
	if (ret != ANX_OK || f->calls != 2) { ret = -6212; goto out; }
	ret = ANX_OK;
out:
	if (ret != ANX_OK) kprintf("day062 failure rc=%d\n", ret);
	anx_external_unregister_handler("anxresearch062");
	if (member_cell) anx_cell_store_release(member_cell);
	if (!anx_uuid_is_nil(&operation)) anx_external_operation_discard(&operation);
	anx_effect_destroy(pending);
	if (writer.obj && writer.obj->staged) anx_object_abort(&writer);
	anx_so_close(&writer);
	if (object) { anx_so_delete(&object->oid, false); anx_objstore_release(object); }
	if (group.id && destroy062(&group) != ANX_OK && ret == ANX_OK) ret = -6213;
	if (foreign && anx_cell_destroy(foreign) != ANX_OK && ret == ANX_OK) ret = -6214;
	if (owner && anx_cell_destroy(owner) != ANX_OK && ret == ANX_OK) ret = -6214;
	if (prompt) { anx_so_delete(&prompt->oid, false); anx_objstore_release(prompt); }
	anx_free(f);
	return ret;
}
#endif
