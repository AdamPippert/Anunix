#include <anx/frontier.h>
#include <anx/resource_view.h>
#include <anx/phase.h>
#include <anx/state_object.h>
#include <anx/identity.h>
#include <anx/sched_domain.h>
#include <anx/alloc.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>
#include <anx/arch.h>
#include <anx/page.h>
struct frontier_record {
	struct anx_frontier_view view;
	struct anx_frontier_spec spec;
	struct anx_phase_view phase;
	struct anx_model_use_view uses[ANX_FRONTIER_NODES];
	anx_oid_t pools[ANX_FRONTIER_NODES], handles[ANX_FRONTIER_NODES];
	struct anx_cell *owner;
	bool busy;
};
static struct frontier_record *records[ANX_FRONTIER_MAX];
static struct anx_spinlock frontier_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct frontier_record *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_FRONTIER_MAX; i++) if (records[i] && records[i]->view.id == id) return records[i];
	return NULL;
}
static int access(struct frontier_record *r)
{
	if (!r) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	if (caller && anx_uuid_compare(caller, &r->view.owner)) return ANX_EPERM;
	return r->busy ? ANX_EBUSY : ANX_OK;
}
static int current(struct frontier_record *r)
{
	if (anx_cell_status_terminal(r->owner->status)) return ANX_EPERM;
	int ret = anx_cell_check_scope(r->owner);
	if (ret == ANX_OK) ret = anx_cell_check_contract(r->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(r->owner, NULL);
	if (ret == ANX_OK) ret = anx_sched_domain_check(r->owner);
	struct anx_phase_view phase;
	if (ret == ANX_OK) ret = anx_phase_get(&r->view.owner, &phase);
	if (ret == ANX_OK && (phase.epoch != r->phase.epoch || phase.parked || phase.phase != ANX_PHASE_INFERENCE ||
	    phase.memory_bytes != r->phase.memory_bytes || phase.tier != r->phase.tier || phase.accelerator != r->phase.accelerator ||
	    phase.accelerator_pct != r->phase.accelerator_pct || anx_uuid_compare(&phase.lease_id, &r->phase.lease_id))) ret = ANX_EBUSY;
	if (ret == ANX_OK) {
		struct anx_engine_lease *lease = anx_lease_lookup(&phase.lease_id);
		if (!lease || lease->revoked || (lease->expires_at && lease->expires_at <= arch_time_now()) ||
		    lease->mem_tier != phase.tier || lease->mem_reserved_bytes != phase.memory_bytes ||
		    lease->accel != phase.accelerator || lease->accel_pct != phase.accelerator_pct) ret = ANX_EBUSY;
	}
	return ret;
}
static int use_current(struct frontier_record *r, uint32_t i, bool completed)
{
	struct anx_model_use_view use;
	int ret = anx_model_use_get(r->uses[i].id, &use);
	if (ret == ANX_OK && (use.epoch != r->uses[i].epoch || !anx_model_use_same_request(&use, &r->uses[i]) ||
	    use.state != (completed ? ANX_MODEL_USE_COMPLETED : ANX_MODEL_USE_READY))) ret = ANX_EBUSY;
	return ret;
}
static int change_check(struct frontier_record *r, uint64_t epoch)
{
	int ret = access(r);
	if (ret == ANX_OK && r->view.epoch != epoch) ret = ANX_EBUSY;
	if (ret == ANX_OK && r->view.epoch == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) ret = current(r);
	return ret;
}
static int image_read(struct frontier_record *r, uint32_t i, struct anx_adapter_image *image)
{
	const struct anx_model_use_source *s = &r->uses[i].image;
	struct anx_object_handle h = {0};
	int ret = anx_so_open(&s->oid, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) return ret;
	anx_spin_lock(&h.obj->lock);
	if (h.obj->state != ANX_OBJ_SEALED || h.obj->version != s->version || h.obj->payload_size != sizeof(*image) ||
	    h.obj->sensitivity != s->sensitivity || h.obj->object_type != ANX_OBJ_STRUCTURED_DATA ||
	    anx_strcmp(h.obj->schema_uri, ANX_MODEL_USE_SCHEMA) || anx_strcmp(h.obj->schema_version, "1") ||
	    h.obj->access_policy.rule_count > ANX_MAX_ACCESS_RULES) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = anx_access_evaluate(&h.obj->access_policy, &r->view.owner, &h.obj->creator_cell, ANX_ACCESS_READ_PAYLOAD);
	anx_spin_unlock(&h.obj->lock);
	if (ret == ANX_OK) {
		ret = anx_so_read_payload(&h, 0, image, sizeof(*image));
		if (ret == sizeof(*image)) ret = ANX_OK;
		else if (ret >= 0) ret = ANX_EIO;
	}
	if (ret == ANX_OK) {
		uint8_t digest[32]; anx_sha256(image, sizeof(*image), digest);
		if (anx_memcmp(digest, s->digest, 32)) ret = ANX_EBUSY;
	}
	anx_so_close(&h);
	return ret == ANX_OK ? anx_adapter_image_check(image) : ret;
}
static int drop(struct frontier_record *r, uint32_t i)
{
	if (anx_uuid_is_nil(&r->pools[i])) return ANX_OK;
	int ret = anx_resource_view_release(&r->handles[i]);
	if (ret == ANX_OK) ret = anx_resource_pool_destroy(&r->pools[i]);
	if (ret == ANX_OK) { r->pools[i] = r->handles[i] = ANX_UUID_NIL; r->view.resident &= ~(1U << i); r->view.physical_pages--; }
	return ret;
}
int anx_frontier_create(const anx_cid_t *owner, const struct anx_frontier_spec *spec, struct anx_frontier_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || !spec || !out) return ANX_EINVAL;
	struct frontier_record *r = anx_zalloc(sizeof(*r));
	if (!r) return ANX_ENOMEM;
	r->spec = *spec; r->view.owner = *owner;
	int ret = !r->spec.count || r->spec.count > ANX_FRONTIER_NODES || !r->spec.phase_epoch ||
		r->spec.mode < ANX_FRONTIER_INCREMENTAL || r->spec.mode > ANX_FRONTIER_FULL ? ANX_EINVAL : ANX_OK;
	if (ret == ANX_OK) { r->owner = anx_cell_store_lookup(owner); if (!r->owner) ret = ANX_ENOENT; }
	if (ret == ANX_OK) ret = anx_phase_get(owner, &r->phase);
	if (ret == ANX_OK && r->phase.epoch != r->spec.phase_epoch) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = current(r);
	for (uint32_t i = 0; ret == ANX_OK && i < ANX_FRONTIER_NODES; i++) {
		const struct anx_frontier_node *n = &r->spec.nodes[i];
		if (i >= r->spec.count) { if (n->use || n->dependencies) ret = ANX_EINVAL; continue; }
		if (!n->use || (n->dependencies & ~((1U << i) - 1))) { ret = ANX_EINVAL; break; }
		for (uint32_t j = 0; j < i; j++) if (n->use == r->spec.nodes[j].use) ret = ANX_EINVAL;
		if (ret == ANX_OK) ret = anx_model_use_get(n->use, &r->uses[i]);
		if (ret == ANX_OK && anx_uuid_compare(&r->uses[i].owner, owner)) ret = ANX_EPERM;
		if (ret == ANX_OK && r->uses[i].state != ANX_MODEL_USE_READY) ret = ANX_EBUSY;
	}
	if (ret == ANX_OK) {
		bool flags; anx_spin_lock_irqsave(&frontier_lock, &flags);
		uint32_t slot;
		for (slot = 0; slot < ANX_FRONTIER_MAX; slot++) if (!records[slot]) break;
		if (slot == ANX_FRONTIER_MAX || sequence == ~(uint64_t)0) ret = ANX_EFULL;
		else {
			r->view.id = ++sequence; r->view.epoch = 1; r->view.phase_epoch = r->phase.epoch;
			r->view.count = r->spec.count; r->view.mode = r->spec.mode; records[slot] = r; *out = r->view;
		}
		anx_spin_unlock_irqrestore(&frontier_lock, flags);
	}
	if (ret != ANX_OK) { if (r->owner) anx_cell_store_release(r->owner); anx_free(r); }
	return ret;
}
int anx_frontier_restore(uint64_t id, uint64_t epoch, struct anx_frontier_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&frontier_lock, &flags);
	struct frontier_record *r = find(id);
	int ret = change_check(r, epoch); uint32_t i = 0;
	if (ret == ANX_OK) {
		for (i = r->view.next; i < r->spec.count; i++) if (!(r->view.resident & (1U << i))) break;
		if (i == r->spec.count) ret = ANX_ENOENT;
		else if (r->view.restored_bytes > ~(uint64_t)0 - sizeof(struct anx_adapter_image)) ret = ANX_EFULL;
	}
	if (ret == ANX_OK) {
		uint32_t pages = 0;
		for (uint32_t j = 0; j < ANX_FRONTIER_MAX; j++) if (records[j] && !anx_uuid_compare(&records[j]->view.owner, &r->view.owner))
			pages += records[j]->view.physical_pages;
		if ((uint64_t)(pages + 1) * ANX_PAGE_SIZE > r->phase.memory_bytes) ret = ANX_ENOMEM;
	}
	struct anx_adapter_image image;
	anx_oid_t pool = ANX_UUID_NIL, handle = ANX_UUID_NIL;
	if (ret == ANX_OK) ret = use_current(r, i, false);
	if (ret == ANX_OK) ret = image_read(r, i, &image);
	if (ret == ANX_OK) ret = anx_resource_pool_create(&r->view.owner, 1, &pool);
	if (ret == ANX_OK) ret = anx_resource_view_insert(&pool, &image, sizeof(image), &handle);
	if (ret == ANX_OK) {
		r->pools[i] = pool; r->handles[i] = handle; r->view.resident |= 1U << i;
		r->view.physical_pages++; r->view.restored_bytes += sizeof(image); r->view.epoch++; *out = r->view;
	} else if (!anx_uuid_is_nil(&pool)) anx_resource_pool_destroy(&pool);
	anx_memset(&image, 0, sizeof(image));
	anx_spin_unlock_irqrestore(&frontier_lock, flags); return ret;
}
int anx_frontier_reclaim(uint64_t id, uint64_t epoch, struct anx_frontier_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&frontier_lock, &flags);
	struct frontier_record *r = find(id);
	int ret = change_check(r, epoch);
	if (ret == ANX_OK) {
		ret = ANX_EBUSY;
		for (uint32_t i = r->spec.count; i-- > r->view.next + 1;) if (r->view.resident & (1U << i)) {
			ret = drop(r, i); break;
		}
		if (ret == ANX_OK) { r->view.epoch++; *out = r->view; }
	}
	anx_spin_unlock_irqrestore(&frontier_lock, flags); return ret;
}
int anx_frontier_step(uint64_t id, uint64_t epoch, struct anx_anxml_response *response, struct anx_frontier_view *out)
{
	if (!id || !epoch || !response || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&frontier_lock, &flags);
	struct frontier_record *r = find(id);
	int ret = change_check(r, epoch); uint32_t i = 0;
	if (ret == ANX_OK) {
		i = r->view.next;
		if (i == r->spec.count) ret = ANX_ENOENT;
		else if (!(r->view.resident & (1U << i)) ||
		    (r->spec.nodes[i].dependencies & r->view.completed) != r->spec.nodes[i].dependencies) ret = ANX_EBUSY;
		else if (r->spec.mode == ANX_FRONTIER_FULL &&
		    (r->view.resident | r->view.completed) != ((1U << r->spec.count) - 1)) ret = ANX_EBUSY;
	}
	if (ret == ANX_OK) ret = use_current(r, i, false);
	struct anx_anxml_response *result = NULL;
	if (ret == ANX_OK) { result = anx_zalloc(sizeof(*result)); if (!result) ret = ANX_ENOMEM; }
	if (ret == ANX_OK) r->busy = true;
	anx_spin_unlock_irqrestore(&frontier_lock, flags);
	if (ret != ANX_OK) return ret;
	struct anx_model_use_view use;
	ret = anx_model_use_execute_view(r->uses[i].id, r->uses[i].epoch, &r->handles[i], result, &use);
	if (ret == ANX_OK) ret = current(r);
	anx_spin_lock_irqsave(&frontier_lock, &flags);
	if (ret == ANX_OK) {
		if (!r->view.completed) {
			r->view.bytes_before_first_work = r->view.restored_bytes;
			r->view.pages_before_first_work = r->view.physical_pages;
		}
		r->uses[i] = use; r->view.completed |= 1U << i; r->view.next++; r->view.epoch++;
		*response = *result; *out = r->view;
	}
	r->busy = false;
	anx_spin_unlock_irqrestore(&frontier_lock, flags);
	anx_memset(result, 0, sizeof(*result)); anx_free(result); return ret;
}
int anx_frontier_get(uint64_t id, struct anx_frontier_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&frontier_lock, &flags);
	struct frontier_record *r = find(id); int ret = access(r);
	if (ret == ANX_OK) *out = r->view;
	anx_spin_unlock_irqrestore(&frontier_lock, flags); return ret;
}
int anx_frontier_read(uint64_t id, uint32_t node, struct anx_anxml_response *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&frontier_lock, &flags);
	struct frontier_record *r = find(id); int ret = access(r);
	if (ret == ANX_OK && (node >= r->spec.count || !(r->view.completed & (1U << node)))) ret = ANX_EPERM;
	if (ret == ANX_OK) ret = use_current(r, node, true);
	if (ret == ANX_OK) ret = anx_model_use_read(r->uses[node].id, out);
	anx_spin_unlock_irqrestore(&frontier_lock, flags); return ret;
}
int anx_frontier_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&frontier_lock, &flags);
	struct frontier_record *r = find(id);
	int ret = !r ? ANX_ENOENT : r->busy ? ANX_EBUSY : ANX_OK;
	for (uint32_t i = 0; ret == ANX_OK && i < r->spec.count; i++) ret = drop(r, i);
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_FRONTIER_MAX; i++) if (records[i] == r) records[i] = NULL;
		anx_cell_store_release(r->owner); anx_memset(r, 0, sizeof(*r)); anx_free(r);
	}
	anx_spin_unlock_irqrestore(&frontier_lock, flags); return ret;
}
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
int anx_frontier_test_corrupt(uint64_t id, uint32_t node)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	bool flags; anx_spin_lock_irqsave(&frontier_lock, &flags);
	struct frontier_record *r = find(id); int ret = access(r);
	if (ret == ANX_OK && (node >= r->spec.count || !(r->view.resident & (1U << node)))) ret = ANX_ENOENT;
	if (ret == ANX_OK) ret = anx_resource_view_test_corrupt(&r->handles[node]);
	anx_spin_unlock_irqrestore(&frontier_lock, flags); return ret;
}
#endif
