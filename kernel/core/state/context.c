#include <anx/context.h>
#include <anx/cell.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
struct context_record {
	struct anx_context_view view;
	struct anx_cell *owner;
	anx_oid_t identity;
	anx_cid_t owner_parent;
	struct context_record *parents[ANX_CONTEXT_PARENTS];
	uint32_t references, depth, slot;
};
static struct context_record *records[ANX_CONTEXT_MAX];
static struct anx_spinlock context_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct context_record *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_CONTEXT_MAX; i++) if (records[i] && records[i]->view.id == id) return records[i];
	return NULL;
}
static int access_owner(const anx_cid_t *owner)
{
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, owner) ? ANX_EPERM : ANX_OK;
}
static int owner_current(struct context_record *r, bool capture)
{
	anx_oid_t identity;
	if (anx_uuid_compare(&r->owner->cid, &r->view.owner) || anx_uuid_compare(&r->owner->parent_cid, &r->owner_parent)) return ANX_EPERM;
	int ret = anx_cell_check_scope(r->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(r->owner, &identity);
	if (ret == ANX_OK && !capture && anx_uuid_compare(&identity, &r->identity)) ret = ANX_EBUSY;
	if (ret == ANX_OK && capture) r->identity = identity;
	return ret;
}
static int source_current(struct context_record *r, bool capture, uint8_t *bytes)
{
	struct anx_state_object *object = anx_objstore_lookup(&r->view.source);
	if (!object) return ANX_ENOENT;
	anx_spin_lock(&object->lock);
	int ret = object->state != ANX_OBJ_SEALED || object->object_type != ANX_OBJ_BYTE_DATA || !object->version ||
		!object->payload || !object->payload_size || object->payload_size > ANX_CONTEXT_BYTES ||
		object->parent_count != r->view.parent_count || (object->parent_count && !object->parent_oids) ||
		object->access_policy.rule_count > ANX_MAX_ACCESS_RULES ||
		(uint32_t)object->sensitivity > ANX_SENSITIVITY_RESTRICTED ? ANX_EINVAL : ANX_OK;
	for (uint32_t i = 0; ret == ANX_OK && i < r->view.parent_count; i++) {
		if (anx_uuid_compare(&object->parent_oids[i], &r->parents[i]->view.source) ||
		    (uint32_t)object->sensitivity < r->parents[i]->view.sensitivity) ret = ANX_EPERM;
	}
	if (ret == ANX_OK) ret = anx_access_evaluate(&object->access_policy, &r->view.owner, &object->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	if (ret == ANX_OK) {
		uint8_t digest[32]; anx_sha256(object->payload, object->payload_size, digest);
		if (!capture && (object->version != r->view.version || object->payload_size != r->view.size ||
		    (uint32_t)object->sensitivity != r->view.sensitivity || anx_memcmp(digest, r->view.digest, 32))) ret = ANX_EBUSY;
		if (ret == ANX_OK && anx_cell_current_id()) ret = anx_effect_fence_observe_read(&object->oid, object->sensitivity);
		if (ret == ANX_OK && capture) {
			r->view.version = object->version; r->view.size = object->payload_size; r->view.sensitivity = object->sensitivity;
			anx_memcpy(r->view.digest, digest, 32);
		}
		if (ret == ANX_OK && bytes) anx_memcpy(bytes, object->payload, r->view.size);
	}
	anx_spin_unlock(&object->lock); anx_objstore_release(object); return ret;
}
static int validate(struct context_record *r, uint32_t *seen)
{
	if (*seen & (1U << r->slot)) return ANX_OK;
	int ret = owner_current(r, false);
	for (uint32_t i = 0; ret == ANX_OK && i < r->view.parent_count; i++) ret = validate(r->parents[i], seen);
	if (ret == ANX_OK) ret = source_current(r, false, NULL);
	if (ret == ANX_OK) *seen |= 1U << r->slot;
	return ret;
}
static int create(const anx_cid_t *owner, const anx_oid_t *source, enum anx_context_origin origin,
		  const uint64_t *parents, uint32_t count, struct anx_context_view *out)
{
	if (origin != ANX_CONTEXT_DERIVED && anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !source || !out || anx_uuid_is_nil(owner) || anx_uuid_is_nil(source) ||
	    (uint32_t)origin > ANX_CONTEXT_DERIVED || count > ANX_CONTEXT_PARENTS ||
	    (origin == ANX_CONTEXT_DERIVED ? !count || !parents : count != 0)) return ANX_EINVAL;
	anx_cid_t owner_id = *owner; anx_oid_t source_id = *source;
	int ret = access_owner(&owner_id);
	if (ret != ANX_OK) return ret;
	uint64_t inputs[ANX_CONTEXT_PARENTS] = {0};
	if (count) anx_memcpy(inputs, parents, count * sizeof(*parents));
	struct context_record *r = anx_zalloc(sizeof(*r));
	if (!r) return ANX_ENOMEM;
	r->view.owner = owner_id; r->view.source = source_id; r->view.origin = origin; r->view.parent_count = count;
	r->view.role_ceiling = ANX_CONTEXT_SYSTEM; r->view.scope_ceiling = ANX_CONTEXT_MACHINE;
	if (origin == ANX_CONTEXT_TOOL_OUTPUT) { r->view.role_ceiling = ANX_CONTEXT_TOOL; r->view.scope_ceiling = ANX_CONTEXT_SESSION; }
	if (origin == ANX_CONTEXT_USER_INPUT) { r->view.role_ceiling = ANX_CONTEXT_USER; r->view.scope_ceiling = ANX_CONTEXT_SESSION; }
	if (origin == ANX_CONTEXT_REPOSITORY) { r->view.role_ceiling = ANX_CONTEXT_USER; r->view.scope_ceiling = ANX_CONTEXT_PROJECT; }
	r->owner = anx_cell_store_lookup(&owner_id);
	if (r->owner) r->owner_parent = r->owner->parent_cid;
	ret = r->owner ? owner_current(r, true) : ANX_ENOENT;
	bool flags; anx_spin_lock_irqsave(&context_lock, &flags);
	uint32_t seen = 0;
	for (uint32_t i = 0; ret == ANX_OK && i < count; i++) {
		struct context_record *parent = find(inputs[i]);
		if (!parent) { ret = ANX_ENOENT; break; }
		if (anx_uuid_compare(&parent->view.owner, &owner_id) || !anx_uuid_compare(&parent->view.source, &source_id)) { ret = ANX_EPERM; break; }
		for (uint32_t j = 0; j < i; j++) if (inputs[j] == inputs[i]) ret = ANX_EINVAL;
		if (ret == ANX_OK) ret = validate(parent, &seen);
		if (ret != ANX_OK) break;
		r->parents[i] = parent;
		if (parent->view.role_ceiling < r->view.role_ceiling) r->view.role_ceiling = parent->view.role_ceiling;
		if (parent->view.scope_ceiling < r->view.scope_ceiling) r->view.scope_ceiling = parent->view.scope_ceiling;
		if (parent->depth >= r->depth) r->depth = parent->depth + 1;
	}
	if (ret == ANX_OK && r->depth >= ANX_CONTEXT_SEGMENTS) ret = ANX_EFULL;
	if (ret == ANX_OK) ret = source_current(r, true, NULL);
	if (ret == ANX_OK) {
		uint32_t slot;
		for (slot = 0; slot < ANX_CONTEXT_MAX; slot++) if (!records[slot]) break;
		if (slot == ANX_CONTEXT_MAX || sequence == ~(uint64_t)0) ret = ANX_EFULL;
		else {
			r->slot = slot; r->view.id = ++sequence; records[slot] = r;
			for (uint32_t i = 0; i < count; i++) r->parents[i]->references++;
			*out = r->view;
		}
	}
	anx_spin_unlock_irqrestore(&context_lock, flags);
	if (ret != ANX_OK) { if (r->owner) anx_cell_store_release(r->owner); anx_free(r); }
	return ret;
}
int anx_context_import(const anx_cid_t *owner, const anx_oid_t *source, enum anx_context_origin origin, struct anx_context_view *out)
{
	if (origin == ANX_CONTEXT_DERIVED) return ANX_EINVAL;
	return create(owner, source, origin, NULL, 0, out);
}
int anx_context_derive(const anx_cid_t *owner, const anx_oid_t *source, const uint64_t *parents, uint32_t count, struct anx_context_view *out)
{ return create(owner, source, ANX_CONTEXT_DERIVED, parents, count, out); }
int anx_context_get(uint64_t id, struct anx_context_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&context_lock, &flags);
	struct context_record *r = find(id);
	int ret = r ? access_owner(&r->view.owner) : ANX_ENOENT; uint32_t seen = 0;
	if (ret == ANX_OK) ret = validate(r, &seen);
	if (ret == ANX_OK) *out = r->view;
	anx_spin_unlock_irqrestore(&context_lock, flags); return ret;
}
int anx_context_compile(const anx_cid_t *owner, const struct anx_context_spec *spec, struct anx_context_bundle *out)
{
	if (!owner || !spec || !out || anx_uuid_is_nil(owner)) return ANX_EINVAL;
	anx_cid_t owner_id = *owner; struct anx_context_spec input = *spec;
	int ret = access_owner(&owner_id);
	if (ret != ANX_OK) return ret;
	if (!input.count || input.count > ANX_CONTEXT_SEGMENTS) return ANX_EINVAL;
	struct anx_context_bundle *bundle = anx_zalloc(sizeof(*bundle));
	if (!bundle) return ANX_ENOMEM;
	bool flags; anx_spin_lock_irqsave(&context_lock, &flags);
	uint32_t seen = 0;
	for (uint32_t i = 0; ret == ANX_OK && i < input.count; i++) {
		struct context_record *r = find(input.segments[i].id);
		if (!r) { ret = ANX_ENOENT; break; }
		if (anx_uuid_compare(&r->view.owner, &owner_id) ||
		    (uint32_t)input.segments[i].role > (uint32_t)r->view.role_ceiling ||
		    (uint32_t)input.segments[i].scope > (uint32_t)r->view.scope_ceiling) { ret = ANX_EPERM; break; }
		if (r->view.size > ANX_CONTEXT_BYTES - bundle->size) { ret = ANX_EFULL; break; }
		ret = validate(r, &seen);
		if (ret == ANX_OK) ret = source_current(r, false, bundle->bytes + bundle->size);
		if (ret != ANX_OK) break;
		bundle->segments[i].id = r->view.id; bundle->segments[i].offset = bundle->size; bundle->segments[i].size = r->view.size;
		bundle->segments[i].role = input.segments[i].role; bundle->segments[i].scope = input.segments[i].scope;
		anx_memcpy(bundle->segments[i].digest, r->view.digest, 32); bundle->size += r->view.size; bundle->count++;
	}
	/* A final dependency check precedes publication of the complete typed bundle. */
	seen = 0;
	for (uint32_t i = 0; ret == ANX_OK && i < input.count; i++) ret = validate(find(input.segments[i].id), &seen);
	if (ret == ANX_OK) *out = *bundle;
	anx_spin_unlock_irqrestore(&context_lock, flags);
	anx_memset(bundle, 0, sizeof(*bundle)); anx_free(bundle); return ret;
}
int anx_context_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&context_lock, &flags);
	struct context_record *r = find(id);
	int ret = !r ? ANX_ENOENT : r->references ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		records[r->slot] = NULL;
		for (uint32_t i = 0; i < r->view.parent_count; i++) r->parents[i]->references--;
		anx_cell_store_release(r->owner); anx_memset(r, 0, sizeof(*r)); anx_free(r);
	}
	anx_spin_unlock_irqrestore(&context_lock, flags); return ret;
}
