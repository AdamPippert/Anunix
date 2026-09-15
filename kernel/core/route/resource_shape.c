#include <anx/resource_shape.h>
#include <anx/resource_view.h>
#include <anx/state_object.h>
#include <anx/phase.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/arch.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
struct resource_shape {
	struct anx_resource_shape_view view;
	struct anx_cell *owner;
	anx_oid_t identity;
	anx_cid_t parent;
	anx_oid_t pools[ANX_RESOURCE_SHAPE_REPLICAS], caches[ANX_RESOURCE_SHAPE_REPLICAS];
	uint32_t references;
	bool busy;
};
static struct resource_shape *records[ANX_RESOURCE_SHAPE_MAX];
static struct anx_spinlock geometry_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct resource_shape *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_RESOURCE_SHAPE_MAX; i++) if (records[i] && records[i]->view.id == id) return records[i];
	return NULL;
}
static int access(struct resource_shape *r)
{
	if (!r) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &r->view.owner) ? ANX_EPERM : ANX_OK;
}
static int owner_current(struct resource_shape *r, bool capture)
{
	anx_oid_t identity;
	if (anx_uuid_compare(&r->owner->cid, &r->view.owner) || anx_uuid_compare(&r->owner->parent_cid, &r->parent)) return ANX_EPERM;
	int ret = anx_cell_check_scope(r->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(r->owner, &identity);
	if (ret == ANX_OK && !capture && anx_uuid_compare(&identity, &r->identity)) ret = ANX_EBUSY;
	if (ret == ANX_OK && capture) r->identity = identity;
	return ret;
}
static int capacity(struct resource_shape *r, uint32_t replicas)
{
	struct anx_phase_view phase;
	int ret = anx_phase_get(&r->view.owner, &phase);
	if (ret == ANX_OK && (phase.parked || phase.phase != ANX_PHASE_INFERENCE)) ret = ANX_EBUSY;
	if (ret == ANX_OK && phase.memory_bytes < (uint64_t)replicas * ANX_PAGE_SIZE) ret = ANX_ENOMEM;
	if (ret == ANX_OK) {
		struct anx_engine_lease *lease = anx_lease_lookup(&phase.lease_id);
		if (!lease || lease->revoked || (lease->expires_at && lease->expires_at <= arch_time_now()) ||
		    lease->mem_reserved_bytes != phase.memory_bytes || lease->mem_tier != phase.tier ||
		    lease->accel != phase.accelerator || lease->accel_pct != phase.accelerator_pct) ret = ANX_EBUSY;
	}
	return ret;
}
static int source_read(struct resource_shape *r, bool capture, struct anx_adapter_image *image)
{
	struct anx_state_object *o = anx_objstore_lookup(&r->view.source.oid);
	if (!o) return ANX_ENOENT;
	anx_spin_lock(&o->lock);
	int ret = !o->version || o->state != ANX_OBJ_SEALED || o->object_type != ANX_OBJ_STRUCTURED_DATA || !o->payload ||
		o->payload_size != sizeof(*image) || anx_strcmp(o->schema_uri, ANX_MODEL_USE_SCHEMA) || anx_strcmp(o->schema_version, "1") ||
		o->access_policy.rule_count > ANX_MAX_ACCESS_RULES || (uint32_t)o->sensitivity > ANX_SENSITIVITY_RESTRICTED ? ANX_EINVAL : ANX_OK;
	if (ret == ANX_OK) ret = anx_access_evaluate(&o->access_policy, &r->view.owner, &o->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	if (ret == ANX_OK) {
		uint8_t digest[32]; anx_sha256(o->payload, o->payload_size, digest);
		if (!capture && (o->version != r->view.source.version || o->payload_size != r->view.source.size ||
		    (uint32_t)o->sensitivity != r->view.source.sensitivity || anx_memcmp(digest, r->view.source.digest, 32))) ret = ANX_EBUSY;
		if (ret == ANX_OK && anx_cell_current_id()) ret = anx_effect_fence_observe_read(&o->oid, o->sensitivity);
		if (ret == ANX_OK) {
			anx_memcpy(image, o->payload, sizeof(*image)); ret = anx_adapter_image_check(image);
		}
		if (ret == ANX_OK && capture) {
			r->view.source.version = o->version; r->view.source.size = o->payload_size; r->view.source.sensitivity = o->sensitivity;
			anx_memcpy(r->view.source.digest, digest, 32);
		}
	}
	anx_spin_unlock(&o->lock); anx_objstore_release(o); return ret;
}
static bool same_source(const struct anx_model_use_source *a, const struct anx_model_use_source *b)
{
	return !anx_uuid_compare(&a->oid, &b->oid) && a->version == b->version && a->size == b->size &&
		a->sensitivity == b->sensitivity && !anx_memcmp(a->digest, b->digest, 32);
}
static int inspect(struct resource_shape *r, struct anx_resource_shape_view *out)
{
	struct anx_resource_shape_view view = r->view;
	view.resident_replicas = view.physical_pages = view.resident_bytes = 0;
	for (uint32_t i = 0; i < r->view.replicas; i++) if (!anx_uuid_is_nil(&r->pools[i])) {
		struct anx_resource_pool_stats stats;
		int ret = anx_resource_pool_stats(&r->pools[i], &stats);
		if (ret != ANX_OK) return ret;
		view.resident_replicas++; view.physical_pages += stats.physical_pages; view.resident_bytes += stats.live_bytes;
	}
	*out = view; return ANX_OK;
}
static int drop(struct resource_shape *r, uint32_t i)
{
	if (anx_uuid_is_nil(&r->pools[i])) return ANX_OK;
	int ret = anx_resource_view_release(&r->caches[i]);
	if (ret == ANX_OK) ret = anx_resource_pool_destroy(&r->pools[i]);
	if (ret == ANX_OK) r->pools[i] = r->caches[i] = ANX_UUID_NIL;
	return ret;
}
int anx_resource_shape_create(const anx_cid_t *owner, const anx_oid_t *source, uint32_t replicas, struct anx_resource_shape_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !source || !out || anx_uuid_is_nil(owner) || anx_uuid_is_nil(source) || !replicas || replicas > ANX_RESOURCE_SHAPE_REPLICAS) return ANX_EINVAL;
	struct resource_shape *r = anx_zalloc(sizeof(*r));
	if (!r) return ANX_ENOMEM;
	r->view.owner = *owner; r->view.source.oid = *source; r->view.replicas = replicas;
	r->owner = anx_cell_store_lookup(owner);
	if (r->owner) r->parent = r->owner->parent_cid;
	int ret = r->owner ? owner_current(r, true) : ANX_ENOENT;
	if (ret == ANX_OK) ret = capacity(r, replicas);
	struct anx_adapter_image image;
	if (ret == ANX_OK) ret = source_read(r, true, &image);
	bool flags; anx_spin_lock_irqsave(&geometry_lock, &flags);
	uint32_t slot = ANX_RESOURCE_SHAPE_MAX;
	for (uint32_t i = 0; ret == ANX_OK && i < ANX_RESOURCE_SHAPE_MAX; i++) {
		if (records[i] && !anx_uuid_compare(&records[i]->view.owner, owner)) ret = ANX_EEXIST;
		if (!records[i] && slot == ANX_RESOURCE_SHAPE_MAX) slot = i;
	}
	if (ret == ANX_OK && (slot == ANX_RESOURCE_SHAPE_MAX || sequence == ~(uint64_t)0)) ret = ANX_EFULL;
	if (ret == ANX_OK) { r->view.id = ++sequence; r->view.epoch = 1; records[slot] = r; *out = r->view; }
	anx_spin_unlock_irqrestore(&geometry_lock, flags);
	if (ret != ANX_OK) { if (r->owner) anx_cell_store_release(r->owner); anx_free(r); }
	return ret;
}
int anx_resource_shape_get(uint64_t id, struct anx_resource_shape_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&geometry_lock, &flags);
	struct resource_shape *r = find(id); int ret = access(r);
	if (ret == ANX_OK) ret = inspect(r, out);
	anx_spin_unlock_irqrestore(&geometry_lock, flags); return ret;
}
static int check(struct resource_shape *r, uint64_t epoch, uint32_t replica, const anx_cid_t *owner, const struct anx_model_use_source *source)
{
	int ret = access(r);
	if (ret == ANX_OK && (r->busy || r->view.epoch != epoch || replica >= r->view.replicas)) ret = ANX_EBUSY;
	if (ret == ANX_OK && (anx_uuid_compare(owner, &r->view.owner) || !same_source(source, &r->view.source))) ret = ANX_EPERM;
	if (ret == ANX_OK) ret = owner_current(r, false);
	if (ret == ANX_OK) ret = capacity(r, r->view.replicas);
	struct anx_adapter_image image;
	if (ret == ANX_OK) ret = source_read(r, false, &image);
	return ret;
}
int anx_resource_shape_check(uint64_t id, uint64_t epoch, uint32_t replica, const anx_cid_t *owner, const struct anx_model_use_source *source)
{
	if (!id || !epoch || !owner || !source) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&geometry_lock, &flags);
	int ret = check(find(id), epoch, replica, owner, source);
	anx_spin_unlock_irqrestore(&geometry_lock, flags); return ret;
}
int anx_resource_shape_bind(uint64_t id, uint64_t epoch, uint32_t replica, const anx_cid_t *owner, const struct anx_model_use_source *source)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || !owner || !source) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&geometry_lock, &flags);
	struct resource_shape *r = find(id); int ret = check(r, epoch, replica, owner, source);
	if (ret == ANX_OK && r->references == ~(uint32_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK && anx_uuid_is_nil(&r->pools[replica])) {
		struct anx_adapter_image image; anx_oid_t pool = ANX_UUID_NIL, cache = ANX_UUID_NIL;
		ret = source_read(r, false, &image);
		if (ret == ANX_OK) ret = anx_resource_pool_create(&r->view.owner, 1, &pool);
		if (ret == ANX_OK) ret = anx_resource_view_insert(&pool, &image, sizeof(image), &cache);
		if (ret == ANX_OK) { r->pools[replica] = pool; r->caches[replica] = cache; }
		else if (!anx_uuid_is_nil(&pool)) anx_resource_pool_destroy(&pool);
	}
	if (ret == ANX_OK) r->references++;
	anx_spin_unlock_irqrestore(&geometry_lock, flags); return ret;
}
int anx_resource_shape_unbind(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	bool flags; anx_spin_lock_irqsave(&geometry_lock, &flags);
	struct resource_shape *r = find(id);
	int ret = !r ? ANX_ENOENT : !r->references || r->busy ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) r->references--;
	anx_spin_unlock_irqrestore(&geometry_lock, flags); return ret;
}
int anx_resource_shape_resize(uint64_t id, uint64_t epoch, uint32_t replicas, struct anx_resource_shape_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || !out || !replicas || replicas > ANX_RESOURCE_SHAPE_REPLICAS) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&geometry_lock, &flags);
	struct resource_shape *r = find(id); int ret = access(r);
	if (ret == ANX_OK && (r->busy || r->view.epoch != epoch || r->view.epoch == ~(uint64_t)0)) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = owner_current(r, false);
	if (ret == ANX_OK) ret = capacity(r, replicas);
	struct anx_adapter_image image;
	if (ret == ANX_OK) ret = source_read(r, false, &image);
	if (ret == ANX_OK && replicas != r->view.replicas) {
		/* Invalidate old placements before reclaiming any removed private replica. */
		r->view.epoch++;
		for (uint32_t i = replicas; ret == ANX_OK && i < r->view.replicas; i++) ret = drop(r, i);
		if (ret == ANX_OK) r->view.replicas = replicas;
	}
	if (ret == ANX_OK) ret = inspect(r, out);
	anx_spin_unlock_irqrestore(&geometry_lock, flags); return ret;
}
int anx_resource_shape_execute(uint64_t id, uint64_t epoch, uint32_t replica, uint64_t use, uint64_t use_epoch,
		struct anx_anxml_response *response, struct anx_model_use_view *out)
{
	if (!id || !epoch || !use || !use_epoch || !response || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&geometry_lock, &flags);
	struct resource_shape *r = find(id); struct anx_model_use_view view;
	int ret = access(r);
	if (ret == ANX_OK) ret = anx_model_use_get(use, &view);
	if (ret == ANX_OK) ret = check(r, epoch, replica, &view.owner, &view.image);
	if (ret == ANX_OK && anx_uuid_is_nil(&r->caches[replica])) ret = ANX_ENOENT;
	if (ret == ANX_OK) r->busy = true;
	anx_spin_unlock_irqrestore(&geometry_lock, flags);
	if (ret != ANX_OK) return ret;
	ret = anx_model_use_execute_view(use, use_epoch, &r->caches[replica], response, out);
	anx_spin_lock_irqsave(&geometry_lock, &flags); r->busy = false;
	anx_spin_unlock_irqrestore(&geometry_lock, flags); return ret;
}
int anx_resource_shape_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&geometry_lock, &flags);
	struct resource_shape *r = find(id);
	int ret = !r ? ANX_ENOENT : r->busy || r->references ? ANX_EBUSY : ANX_OK;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; ret == ANX_OK && i < r->view.replicas; i++) ret = drop(r, i);
		if (ret == ANX_OK) {
			for (uint32_t i = 0; i < ANX_RESOURCE_SHAPE_MAX; i++) if (records[i] == r) records[i] = NULL;
			anx_cell_store_release(r->owner); anx_memset(r, 0, sizeof(*r)); anx_free(r);
		}
	}
	anx_spin_unlock_irqrestore(&geometry_lock, flags); return ret;
}
