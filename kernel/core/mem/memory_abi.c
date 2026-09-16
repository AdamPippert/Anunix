#include <anx/memory_abi.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
struct memory_source { anx_oid_t oid; uint64_t version; uint32_t size, sensitivity; uint8_t digest[32]; };
struct memory_contract {
	struct anx_memory_contract_view view;
	struct anx_cell *owner;
	anx_cid_t parent;
	anx_oid_t identity;
	struct memory_source reader, space;
	struct anx_memory_reader_format reader_format;
	struct anx_memory_space_format space_format;
	uint32_t references;
};
struct memory_index {
	struct anx_memory_index_view view;
	struct memory_contract *contract;
	anx_cid_t parent;
	anx_oid_t identity;
	struct memory_source reader, space, sources[ANX_MEMORY_ABI_SOURCES];
	uint8_t vectors[ANX_MEMORY_ABI_SOURCES][ANX_MEMORY_ABI_DIMENSIONS];
};
static struct memory_contract *contracts[ANX_MEMORY_ABI_MAX];
static struct memory_index *indices[ANX_MEMORY_ABI_MAX];
static struct anx_spinlock abi_lock = ANX_SPINLOCK_INIT;
static uint64_t contract_sequence, index_sequence;
static struct memory_contract *find_contract(const anx_cid_t *owner)
{
	for (uint32_t i = 0; i < ANX_MEMORY_ABI_MAX; i++) if (contracts[i] && !anx_uuid_compare(&contracts[i]->view.owner, owner)) return contracts[i];
	return NULL;
}
static struct memory_index *find_index(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_MEMORY_ABI_MAX; i++) if (indices[i] && indices[i]->view.id == id) return indices[i];
	return NULL;
}
static int access(const anx_cid_t *owner)
{
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, owner) ? ANX_EPERM : ANX_OK;
}
static int owner_current(struct memory_contract *c, bool capture)
{
	anx_oid_t identity;
	if (anx_uuid_compare(&c->owner->cid, &c->view.owner) || anx_uuid_compare(&c->owner->parent_cid, &c->parent)) return ANX_EPERM;
	int ret = anx_cell_check_scope(c->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(c->owner, &identity);
	if (ret == ANX_OK && !capture && anx_uuid_compare(&identity, &c->identity)) ret = ANX_EBUSY;
	if (ret == ANX_OK && capture) c->identity = identity;
	return ret;
}
static bool same(const struct memory_source *a, const struct memory_source *b)
{
	return !anx_uuid_compare(&a->oid, &b->oid) && a->version == b->version && a->size == b->size &&
		a->sensitivity == b->sensitivity && !anx_memcmp(a->digest, b->digest, 32);
}
/* kind: zero is canonical text; one and two are fixed reader and embedding schemas. */
static int read_source(const anx_cid_t *owner, struct memory_source *source, uint32_t kind, bool capture, void *bytes)
{
	struct anx_state_object *o = anx_objstore_lookup(&source->oid);
	if (!o) return ANX_ENOENT;
	anx_spin_lock(&o->lock);
	int ret = !o->version || o->state != ANX_OBJ_SEALED || !o->payload || !o->payload_size ||
		o->access_policy.rule_count > ANX_MAX_ACCESS_RULES || (uint32_t)o->sensitivity > ANX_SENSITIVITY_RESTRICTED ? ANX_EINVAL : ANX_OK;
	if (ret == ANX_OK && !kind && (o->object_type != ANX_OBJ_BYTE_DATA || o->payload_size > 1024)) ret = ANX_EINVAL;
	if (ret == ANX_OK && kind && (o->object_type != ANX_OBJ_STRUCTURED_DATA || anx_strcmp(o->schema_version, "1") ||
	    anx_strcmp(o->schema_uri, kind == 1 ? ANX_MEMORY_READER_SCHEMA : ANX_MEMORY_SPACE_SCHEMA) ||
	    o->payload_size != (kind == 1 ? sizeof(struct anx_memory_reader_format) : sizeof(struct anx_memory_space_format)))) ret = ANX_EINVAL;
	if (ret == ANX_OK) ret = anx_access_evaluate(&o->access_policy, owner, &o->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	if (ret == ANX_OK) {
		struct memory_source now = {.oid = o->oid, .version = o->version, .size = o->payload_size, .sensitivity = o->sensitivity};
		anx_sha256(o->payload, o->payload_size, now.digest);
		if (!capture && !same(&now, source)) ret = ANX_EBUSY;
		if (ret == ANX_OK && anx_cell_current_id()) ret = anx_effect_fence_observe_read(&o->oid, o->sensitivity);
		if (ret == ANX_OK) { if (capture) *source = now; if (bytes) anx_memcpy(bytes, o->payload, o->payload_size); }
	}
	anx_spin_unlock(&o->lock); anx_objstore_release(o); return ret;
}
static int contract_current(struct memory_contract *c)
{
	int ret = owner_current(c, false);
	if (ret == ANX_OK) ret = read_source(&c->view.owner, &c->reader, 1, false, NULL);
	if (ret == ANX_OK) ret = read_source(&c->view.owner, &c->space, 2, false, NULL);
	return ret;
}
static void project(const struct memory_source *source, uint32_t transform, uint8_t out[ANX_MEMORY_ABI_DIMENSIONS])
{
	for (uint32_t i = 0; i < ANX_MEMORY_ABI_DIMENSIONS; i++) out[i] = source->digest[(i + (transform == 2 ? 1U : 0U)) % ANX_MEMORY_ABI_DIMENSIONS];
}
int anx_memory_contract_set(const anx_cid_t *owner, uint64_t expected_epoch, const struct anx_memory_contract_spec *spec, struct anx_memory_contract_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !spec || !out || anx_uuid_is_nil(owner) || anx_uuid_is_nil(&spec->reader) || anx_uuid_is_nil(&spec->space)) return ANX_EINVAL;
	struct memory_contract *fresh = anx_zalloc(sizeof(*fresh));
	if (!fresh) return ANX_ENOMEM;
	fresh->view.owner = *owner; fresh->view.sources = *spec;
	fresh->reader.oid = fresh->view.sources.reader; fresh->space.oid = fresh->view.sources.space;
	fresh->owner = anx_cell_store_lookup(&fresh->view.owner);
	if (fresh->owner) fresh->parent = fresh->owner->parent_cid;
	int ret = fresh->owner ? owner_current(fresh, true) : ANX_ENOENT;
	bool flags; anx_spin_lock_irqsave(&abi_lock, &flags);
	struct memory_contract *old = find_contract(&fresh->view.owner);
	if (ret == ANX_OK && (old ? old->view.epoch != expected_epoch : expected_epoch != 0)) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = read_source(&fresh->view.owner, &fresh->reader, 1, true, &fresh->reader_format);
	if (ret == ANX_OK) ret = read_source(&fresh->view.owner, &fresh->space, 2, true, &fresh->space_format);
	if (ret == ANX_OK && (fresh->reader_format.format != 1 || fresh->reader_format.semantic_schema != 1 ||
	    fresh->reader_format.dimensions != ANX_MEMORY_ABI_DIMENSIONS || fresh->space_format.format != 1 ||
	    fresh->space_format.dimensions != ANX_MEMORY_ABI_DIMENSIONS || !fresh->space_format.transform || fresh->space_format.transform > 2)) ret = ANX_ENOTSUP;
	uint32_t slot;
	for (slot = 0; slot < ANX_MEMORY_ABI_MAX; slot++) if (!contracts[slot]) break;
	if (ret == ANX_OK && ((!old && slot == ANX_MEMORY_ABI_MAX) || contract_sequence == ~(uint64_t)0)) ret = ANX_EFULL;
	if (ret == ANX_OK) {
		fresh->view.epoch = ++contract_sequence; fresh->view.semantic_schema = fresh->reader_format.semantic_schema;
		fresh->view.dimensions = fresh->reader_format.dimensions;
		if (old) {
			old->view = fresh->view; old->parent = fresh->parent; old->identity = fresh->identity;
			old->reader = fresh->reader; old->space = fresh->space; old->reader_format = fresh->reader_format; old->space_format = fresh->space_format;
		} else contracts[slot] = fresh;
		*out = fresh->view;
	}
	anx_spin_unlock_irqrestore(&abi_lock, flags);
	if (ret != ANX_OK || old) { if (fresh->owner) anx_cell_store_release(fresh->owner); anx_free(fresh); }
	return ret;
}
int anx_memory_contract_get(const anx_cid_t *owner, struct anx_memory_contract_view *out)
{
	if (!owner || !out) return ANX_EINVAL;
	int ret = access(owner); if (ret != ANX_OK) return ret;
	bool flags; anx_spin_lock_irqsave(&abi_lock, &flags);
	struct memory_contract *c = find_contract(owner);
	ret = c ? contract_current(c) : ANX_ENOENT;
	if (ret == ANX_OK) *out = c->view;
	anx_spin_unlock_irqrestore(&abi_lock, flags); return ret;
}
int anx_memory_index_build(const anx_cid_t *owner, const anx_oid_t *sources, uint32_t count, struct anx_memory_index_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !sources || !out || !count || count > ANX_MEMORY_ABI_SOURCES) return ANX_EINVAL;
	struct memory_index *index = anx_zalloc(sizeof(*index));
	if (!index) return ANX_ENOMEM;
	index->view.owner = *owner; index->view.count = count;
	for (uint32_t i = 0; i < count; i++) index->sources[i].oid = sources[i];
	bool flags; anx_spin_lock_irqsave(&abi_lock, &flags);
	struct memory_contract *c = find_contract(&index->view.owner);
	int ret = c ? contract_current(c) : ANX_ENOENT;
	for (uint32_t i = 0; ret == ANX_OK && i < count; i++) {
		for (uint32_t j = 0; j < i; j++) if (!anx_uuid_compare(&index->sources[i].oid, &index->sources[j].oid)) ret = ANX_EINVAL;
		if (ret == ANX_OK) ret = read_source(&c->view.owner, &index->sources[i], 0, true, NULL);
		if (ret == ANX_OK) project(&index->sources[i], c->space_format.transform, index->vectors[i]);
	}
	if (ret == ANX_OK) {
		uint32_t slot;
		for (slot = 0; slot < ANX_MEMORY_ABI_MAX; slot++) if (!indices[slot]) break;
		if (slot == ANX_MEMORY_ABI_MAX || index_sequence == ~(uint64_t)0) ret = ANX_EFULL;
		else {
			index->view.id = ++index_sequence; index->view.sources = c->view.sources; index->view.dimensions = c->view.dimensions;
			index->contract = c; index->reader = c->reader; index->space = c->space; index->identity = c->identity; index->parent = c->parent;
			c->references++; indices[slot] = index; *out = index->view;
		}
	}
	anx_spin_unlock_irqrestore(&abi_lock, flags);
	if (ret != ANX_OK) anx_free(index);
	return ret;
}
int anx_memory_index_query(uint64_t id, const anx_oid_t *query, struct anx_memory_match *out)
{
	if (!id || !query || !out || anx_uuid_is_nil(query)) return ANX_EINVAL;
	struct memory_source input = {.oid = *query};
	bool flags; anx_spin_lock_irqsave(&abi_lock, &flags);
	struct memory_index *index = find_index(id);
	int ret = index ? access(&index->view.owner) : ANX_ENOENT;
	struct memory_contract *c = index ? index->contract : NULL;
	if (ret == ANX_OK) ret = contract_current(c);
	if (ret == ANX_OK && (!same(&index->reader, &c->reader) || !same(&index->space, &c->space) ||
	    anx_uuid_compare(&index->identity, &c->identity) || anx_uuid_compare(&index->parent, &c->parent))) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = read_source(&c->view.owner, &input, 0, true, NULL);
	uint8_t vector[ANX_MEMORY_ABI_DIMENSIONS]; uint32_t best = ~(uint32_t)0, selected = 0;
	if (ret == ANX_OK) project(&input, c->space_format.transform, vector);
	for (uint32_t i = 0; ret == ANX_OK && i < index->view.count; i++) {
		ret = read_source(&c->view.owner, &index->sources[i], 0, false, NULL);
		if (ret != ANX_OK) break;
		uint32_t score = 0;
		for (uint32_t j = 0; j < ANX_MEMORY_ABI_DIMENSIONS; j++) { int32_t delta = vector[j] - index->vectors[i][j]; score += (uint32_t)(delta * delta); }
		if (score < best) { best = score; selected = i; }
	}
	if (ret == ANX_OK) *out = (struct anx_memory_match){index->view.id, c->view.epoch, index->sources[selected].oid, best};
	anx_spin_unlock_irqrestore(&abi_lock, flags); return ret;
}
int anx_memory_index_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&abi_lock, &flags);
	struct memory_index *index = find_index(id); int ret = index ? ANX_OK : ANX_ENOENT;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_MEMORY_ABI_MAX; i++) if (indices[i] == index) indices[i] = NULL;
		index->contract->references--; anx_memset(index, 0, sizeof(*index)); anx_free(index);
	}
	anx_spin_unlock_irqrestore(&abi_lock, flags); return ret;
}
int anx_memory_contract_remove(const anx_cid_t *owner)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&abi_lock, &flags);
	struct memory_contract *c = find_contract(owner); int ret = !c ? ANX_ENOENT : c->references ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_MEMORY_ABI_MAX; i++) if (contracts[i] == c) contracts[i] = NULL;
		anx_cell_store_release(c->owner); anx_memset(c, 0, sizeof(*c)); anx_free(c);
	}
	anx_spin_unlock_irqrestore(&abi_lock, flags); return ret;
}
