#include <anx/control_image.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
#include <anx/identity.h>
#include <anx/sched_domain.h>

_Static_assert(sizeof(struct anx_control_program) == 336, "control ABI word count");
struct control_record {
	struct anx_control_view view;
	struct anx_control_program program;
	struct anx_control_authority authority;
	struct anx_control_event events[ANX_CONTROL_STEPS_MAX];
	struct anx_cell *owner;
	uint64_t image_version, group_epochs[ANX_CONTROL_GROUPS_MAX];
	uint32_t ran, verified, result_slot, result_branch, image_sensitivity;
	uint8_t image_hash[32];
	char result[129];
};
static struct control_record *records[ANX_CONTROL_MAX];
static struct anx_spinlock control_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct control_record *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_CONTROL_MAX; i++) if (records[i] && records[i]->view.id == id) return records[i];
	return NULL;
}
static int access(struct control_record *c)
{
	if (!c) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &c->view.owner) ? ANX_EPERM : ANX_OK;
}
static uint32_t little_word(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int image_read(struct control_record *c, bool capture)
{
	struct anx_object_handle handle = {0};
	uint8_t wire[sizeof(struct anx_control_program)], digest[32];
	uint64_t version;
	uint32_t sensitivity;
	int ret = anx_so_open(&c->view.image, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK) return ret;
	struct anx_state_object *o = handle.obj;
	anx_spin_lock(&o->lock);
	version = o->version; sensitivity = o->sensitivity;
	if (!version || o->state != ANX_OBJ_SEALED || o->object_type != ANX_OBJ_STRUCTURED_DATA ||
	    o->payload_size != sizeof(wire) || !o->payload ||
	    anx_strcmp(o->schema_uri, ANX_CONTROL_SCHEMA) || anx_strcmp(o->schema_version, "1") ||
	    o->access_policy.rule_count > ANX_MAX_ACCESS_RULES || sensitivity > ANX_SENSITIVITY_RESTRICTED) ret = ANX_EINVAL;
	else ret = anx_access_evaluate(&o->access_policy, &c->view.owner, &o->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	if (ret == ANX_OK && !capture && (version != c->image_version || sensitivity != c->image_sensitivity)) ret = ANX_EBUSY;
	anx_spin_unlock(&o->lock);
	if (ret == ANX_OK) {
		ret = anx_so_read_payload(&handle, 0, wire, sizeof(wire));
		if (ret == sizeof(wire)) ret = ANX_OK;
		else if (ret >= 0) ret = ANX_EIO;
	}
	if (ret == ANX_OK) {
		anx_sha256(wire, sizeof(wire), digest);
		anx_spin_lock(&o->lock);
		if (o->state != ANX_OBJ_SEALED || o->object_type != ANX_OBJ_STRUCTURED_DATA ||
		    anx_strcmp(o->schema_uri, ANX_CONTROL_SCHEMA) || anx_strcmp(o->schema_version, "1") ||
		    o->version != version || o->payload_size != sizeof(wire) ||
		    (uint32_t)o->sensitivity != sensitivity || o->access_policy.rule_count > ANX_MAX_ACCESS_RULES) ret = ANX_EBUSY;
		else ret = anx_access_evaluate(&o->access_policy, &c->view.owner, &o->creator_cell, ANX_ACCESS_READ_PAYLOAD);
		anx_spin_unlock(&o->lock);
		if (ret == ANX_OK && !capture && anx_memcmp(digest, c->image_hash, sizeof(digest))) ret = ANX_EBUSY;
		if (ret == ANX_OK && capture) {
			c->image_version = version; c->image_sensitivity = sensitivity; anx_memcpy(c->image_hash, digest, 32);
			c->program.abi = little_word(wire); c->program.count = little_word(wire + 4);
			c->program.maximum_steps = little_word(wire + 8); c->program.reserved = little_word(wire + 12);
			for (uint32_t i = 0; i < ANX_CONTROL_STEPS_MAX; i++) {
				const uint8_t *p = wire + 16 + i * 20;
				c->program.instructions[i] = (struct anx_control_instruction){ little_word(p), little_word(p+4),
					little_word(p+8), little_word(p+12), little_word(p+16) };
			}
		}
	}
	anx_memset(wire, 0, sizeof(wire)); anx_so_close(&handle);
	return ret;
}
struct abstract_state { uint32_t may_run, must_run, verified, depth; bool reached, result; };
static void propagate(struct abstract_state *target, struct abstract_state source)
{
	source.depth++;
	if (!target->reached) { *target = source; target->reached = true; return; }
	target->may_run |= source.may_run; target->must_run &= source.must_run;
	target->verified &= source.verified; target->result = target->result && source.result;
	if (source.depth > target->depth) target->depth = source.depth;
}
static int program_check(const struct control_record *c)
{
	const struct anx_control_program *p = &c->program;
	const struct anx_control_authority *a = &c->authority;
	struct abstract_state states[ANX_CONTROL_STEPS_MAX] = {0};
	if (p->abi != 1) return ANX_ENOTSUP;
	if (!p->count || p->count > ANX_CONTROL_STEPS_MAX || p->reserved ||
	    !p->maximum_steps || p->maximum_steps > a->maximum_steps) return ANX_EINVAL;
	states[0].reached = true; states[0].depth = 1;
	for (uint32_t i = 0; i < ANX_CONTROL_STEPS_MAX; i++) {
		const struct anx_control_instruction *n = &p->instructions[i];
		if (i >= p->count) {
			if (n->opcode || n->slot || n->branch || n->on_success || n->on_failure) return ANX_EINVAL;
			continue;
		}
		if (!states[i].reached || states[i].depth > p->maximum_steps) return ANX_EINVAL;
		if (n->opcode >= ANX_CONTROL_OPCODE_COUNT) return ANX_ENOTSUP;
		if (!(a->allowed_operations & (1U << n->opcode))) return ANX_EPERM;
		if (n->opcode == ANX_CONTROL_RETURN || n->opcode == ANX_CONTROL_FAIL) {
			if (n->slot || n->branch || n->on_success != ANX_CONTROL_HALT || n->on_failure != ANX_CONTROL_HALT ||
			    (n->opcode == ANX_CONTROL_RETURN && !states[i].result)) return ANX_EINVAL;
			continue;
		}
		if (n->slot >= a->group_count) return ANX_EPERM;
		if (n->on_success <= i || n->on_failure <= i || n->on_success >= p->count || n->on_failure >= p->count ||
		    (n->opcode != ANX_CONTROL_READ && n->branch) ||
		    (n->opcode == ANX_CONTROL_READ && n->branch >= ANX_BRANCH_MAX && n->branch != ANX_CONTROL_HALT)) return ANX_EINVAL;
		struct abstract_state success = states[i], failure = states[i];
		uint32_t bit = 1U << n->slot;
		if (n->opcode == ANX_CONTROL_RUN) {
			if (states[i].may_run & bit) return ANX_EINVAL;
			success.may_run |= bit; success.must_run |= bit; failure = success;
		} else if (n->opcode == ANX_CONTROL_VERIFY) {
			if (!(states[i].must_run & bit)) return ANX_EINVAL;
			success.verified |= bit; failure.verified &= ~bit;
		} else {
			if (!(states[i].verified & bit)) return ANX_EINVAL;
			success.result = true; failure.result = false;
		}
		propagate(&states[n->on_success], success); propagate(&states[n->on_failure], failure);
	}
	return ANX_OK;
}
static int owner_check(struct control_record *c)
{
	if (anx_cell_status_terminal(c->owner->status)) return ANX_EPERM;
	int ret = anx_cell_check_scope(c->owner);
	if (ret == ANX_OK) ret = anx_cell_check_contract(c->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(c->owner, NULL);
	if (ret == ANX_OK) ret = anx_sched_domain_check(c->owner);
	return ret;
}
static int group_check(struct control_record *c, uint32_t slot, struct anx_branch_view *out)
{
	int ret = anx_branch_group_get(c->authority.groups[slot], out);
	if (ret == ANX_OK && (anx_uuid_compare(&out->owner, &c->view.owner) || out->epoch != c->group_epochs[slot])) ret = ANX_EBUSY;
	return ret;
}
int anx_control_create(const anx_cid_t *owner, const anx_oid_t *image, const struct anx_control_authority *authority, struct anx_control_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !image || !authority || !out) return ANX_EINVAL;
	struct anx_control_authority a = *authority;
	if (!a.group_count || a.group_count > ANX_CONTROL_GROUPS_MAX || !a.maximum_steps || a.maximum_steps > ANX_CONTROL_STEPS_MAX ||
	    !a.allowed_operations || (a.allowed_operations >> ANX_CONTROL_OPCODE_COUNT)) return ANX_EINVAL;
	for (uint32_t i = 0; i < ANX_CONTROL_GROUPS_MAX; i++) {
		if ((i < a.group_count && !a.groups[i]) || (i >= a.group_count && a.groups[i])) return ANX_EINVAL;
		for (uint32_t j = 0; j < i && i < a.group_count; j++) if (a.groups[j] == a.groups[i]) return ANX_EINVAL;
	}
	struct control_record *c = anx_zalloc(sizeof(*c));
	if (!c) return ANX_ENOMEM;
	c->authority = a; c->view.owner = *owner; c->view.image = *image; c->result_slot = ANX_CONTROL_HALT;
	c->owner = anx_cell_store_lookup(owner);
	int ret = c->owner ? owner_check(c) : ANX_ENOENT;
	if (ret == ANX_OK) ret = image_read(c, true);
	if (ret == ANX_OK) ret = program_check(c);
	for (uint32_t i = 0; ret == ANX_OK && i < a.group_count; i++) {
		struct anx_branch_view group;
		ret = anx_branch_group_get(a.groups[i], &group);
		if (ret == ANX_OK && anx_uuid_compare(&group.owner, owner)) ret = ANX_EPERM;
		if (ret == ANX_OK && group.state != ANX_BRANCH_READY) ret = ANX_EBUSY;
		if (ret == ANX_OK) c->group_epochs[i] = group.epoch;
	}
	if (ret == ANX_OK) {
		bool flags;
		anx_spin_lock_irqsave(&control_lock, &flags);
		uint32_t i;
		for (i = 0; i < ANX_CONTROL_MAX; i++) if (!records[i]) break;
		if (i == ANX_CONTROL_MAX || sequence == ~(uint64_t)0) ret = ANX_EFULL;
		else { c->view.id = ++sequence; c->view.epoch = 1; c->view.state = ANX_CONTROL_READY; records[i] = c; *out = c->view; }
		anx_spin_unlock_irqrestore(&control_lock, flags);
	}
	if (ret != ANX_OK) { if (c->owner) anx_cell_store_release(c->owner); anx_memset(c, 0, sizeof(*c)); anx_free(c); }
	return ret;
}

int anx_control_get(uint64_t id, struct anx_control_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&control_lock, &flags);
	struct control_record *c = find(id);
	int ret = access(c);
	if (ret == ANX_OK && c->view.state == ANX_CONTROL_BUSY) ret = ANX_EBUSY;
	if (ret == ANX_OK) *out = c->view;
	anx_spin_unlock_irqrestore(&control_lock, flags);
	return ret;
}
static int result_check(struct control_record *c)
{
	if (!c->view.result_size || c->result_slot >= c->authority.group_count || !(c->verified & (1U << c->result_slot))) return ANX_EPERM;
	struct anx_branch_view group;
	int ret = group_check(c, c->result_slot, &group);
	char bytes[129]; uint32_t size = 0;
	if (ret == ANX_OK) ret = anx_branch_group_read(group.id, c->result_branch, bytes, sizeof(bytes), &size);
	if (ret == ANX_OK && (size != c->view.result_size || anx_memcmp(bytes, c->result, size))) ret = ANX_EBUSY;
	anx_memset(bytes, 0, sizeof(bytes));
	return ret;
}
int anx_control_step(uint64_t id, uint64_t epoch, struct anx_control_view *out)
{
	if (!id || !epoch || !out) return ANX_EINVAL;
	bool flags;
	struct anx_branch_view group = {0};
	anx_spin_lock_irqsave(&control_lock, &flags);
	struct control_record *c = find(id);
	int ret = access(c);
	if (ret == ANX_OK && (c->view.epoch != epoch || c->view.state != ANX_CONTROL_READY)) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = owner_check(c);
	if (ret == ANX_OK) ret = image_read(c, false);
	if (ret == ANX_OK && (c->view.event_count >= c->program.maximum_steps || c->view.program_counter >= c->program.count)) ret = ANX_EFULL;
	struct anx_control_instruction instruction = {0};
	uint32_t branch = 0, pc = 0;
	if (ret == ANX_OK) {
		pc = c->view.program_counter; instruction = c->program.instructions[pc];
		if (instruction.opcode <= ANX_CONTROL_READ) {
			ret = group_check(c, instruction.slot, &group);
			uint32_t bit = 1U << instruction.slot;
			if (ret == ANX_OK && instruction.opcode == ANX_CONTROL_RUN && ((c->ran & bit) || group.state != ANX_BRANCH_READY)) ret = ANX_EBUSY;
			if (ret == ANX_OK && instruction.opcode == ANX_CONTROL_VERIFY && !(c->ran & bit)) ret = ANX_EPERM;
			if (ret == ANX_OK && instruction.opcode == ANX_CONTROL_READ) {
				branch = instruction.branch == ANX_CONTROL_HALT ? group.winner : instruction.branch;
				if (!(c->verified & bit) || group.state != ANX_BRANCH_COMPLETED || branch >= group.count) ret = ANX_EPERM;
			}
		} else if (instruction.opcode == ANX_CONTROL_RETURN) ret = result_check(c);
	}
	if (ret == ANX_OK) c->view.state = ANX_CONTROL_BUSY;
	anx_spin_unlock_irqrestore(&control_lock, flags);
	if (ret != ANX_OK) return ret;
	/* Exactly one admitted instruction is in flight; BUSY prevents reentry/removal. */
	int operation = ANX_OK;
	if (instruction.opcode == ANX_CONTROL_RUN) {
		ret = anx_branch_group_run(group.id, group.epoch, &group);
		if (ret != ANX_OK) {
			anx_spin_lock_irqsave(&control_lock, &flags); c->view.state = ANX_CONTROL_READY;
			anx_spin_unlock_irqrestore(&control_lock, flags); return ret;
		}
		c->group_epochs[instruction.slot] = group.epoch;
		c->ran |= 1U << instruction.slot; c->verified &= ~(1U << instruction.slot);
		operation = group.state == ANX_BRANCH_COMPLETED ? ANX_OK : group.result;
		if (group.state != ANX_BRANCH_COMPLETED && operation == ANX_OK) operation = ANX_EIO;
	} else if (instruction.opcode == ANX_CONTROL_VERIFY) {
		operation = group.state == ANX_BRANCH_COMPLETED && group.result == ANX_OK ? ANX_OK : ANX_EIO;
		if (operation == ANX_OK) c->verified |= 1U << instruction.slot;
		else c->verified &= ~(1U << instruction.slot);
	} else if (instruction.opcode == ANX_CONTROL_READ) {
		c->view.result_size = 0; c->result_slot = ANX_CONTROL_HALT;
		anx_memset(c->result, 0, sizeof(c->result));
		operation = anx_branch_group_read(group.id, branch, c->result, sizeof(c->result), &c->view.result_size);
		if (operation == ANX_OK) { c->result_slot = instruction.slot; c->result_branch = branch; }
	} else if (instruction.opcode == ANX_CONTROL_FAIL) {
		operation = ANX_EIO; c->view.result_size = 0; c->result_slot = ANX_CONTROL_HALT;
		anx_memset(c->result, 0, sizeof(c->result));
	}
	anx_spin_lock_irqsave(&control_lock, &flags);
	uint32_t next = operation == ANX_OK ? instruction.on_success : instruction.on_failure;
	uint32_t event = c->view.event_count;
	c->events[event] = (struct anx_control_event){ event + 1, pc, instruction.opcode, instruction.slot, next, operation };
	c->view.event_count++; c->view.program_counter = next; c->view.epoch++; c->view.result = operation;
	c->view.state = instruction.opcode == ANX_CONTROL_RETURN ? ANX_CONTROL_COMPLETED :
		instruction.opcode == ANX_CONTROL_FAIL ? ANX_CONTROL_FAILED : ANX_CONTROL_READY;
	*out = c->view;
	anx_spin_unlock_irqrestore(&control_lock, flags);
	return ANX_OK;
}
int anx_control_event_get(uint64_t id, uint32_t event, struct anx_control_event *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&control_lock, &flags);
	struct control_record *c = find(id);
	int ret = access(c);
	if (ret == ANX_OK && event >= c->view.event_count) ret = ANX_ENOENT;
	if (ret == ANX_OK) *out = c->events[event];
	anx_spin_unlock_irqrestore(&control_lock, flags);
	return ret;
}
int anx_control_read(uint64_t id, void *bytes, uint32_t capacity, uint32_t *size)
{
	if (!id || !bytes || !size) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&control_lock, &flags);
	struct control_record *c = find(id);
	int ret = access(c);
	if (ret == ANX_OK && c->view.state != ANX_CONTROL_COMPLETED) ret = ANX_EPERM;
	if (ret == ANX_OK && capacity < c->view.result_size) ret = ANX_ENOMEM;
	if (ret == ANX_OK) ret = image_read(c, false);
	if (ret == ANX_OK) ret = result_check(c);
	if (ret == ANX_OK) { anx_memcpy(bytes, c->result, c->view.result_size); *size = c->view.result_size; }
	anx_spin_unlock_irqrestore(&control_lock, flags);
	return ret;
}
int anx_control_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags;
	anx_spin_lock_irqsave(&control_lock, &flags);
	struct control_record *c = find(id);
	int ret = !c ? ANX_ENOENT : c->view.state == ANX_CONTROL_BUSY ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_CONTROL_MAX; i++) if (records[i] == c) records[i] = NULL;
		anx_cell_store_release(c->owner); anx_memset(c, 0, sizeof(*c)); anx_free(c);
	}
	anx_spin_unlock_irqrestore(&control_lock, flags);
	return ret;
}
