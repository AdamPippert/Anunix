#include <anx/route_catalog.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/sched_domain.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>
struct catalog_record {
	struct anx_route_catalog_view view;
	struct anx_cell *owner;
	anx_cid_t parent;
	anx_oid_t identity;
};
static struct catalog_record *catalogs[ANX_ROUTE_CATALOG_MAX];
static struct anx_spinlock catalog_lock = ANX_SPINLOCK_INIT;
static uint64_t sequence;
static struct catalog_record *find(uint64_t id)
{
	for (uint32_t i = 0; i < ANX_ROUTE_CATALOG_MAX; i++) if (catalogs[i] && catalogs[i]->view.id == id) return catalogs[i];
	return NULL;
}
static int access(struct catalog_record *r)
{
	if (!r) return ANX_ENOENT;
	const anx_cid_t *caller = anx_cell_current_id();
	return caller && anx_uuid_compare(caller, &r->view.owner) ? ANX_EPERM : ANX_OK;
}
static int owner_current(struct catalog_record *r)
{
	if (anx_cell_status_terminal(r->owner->status)) return ANX_EBUSY;
	if (anx_uuid_compare(&r->owner->parent_cid, &r->parent)) return ANX_EPERM;
	anx_oid_t identity;
	int ret = anx_cell_check_scope(r->owner);
	if (ret == ANX_OK) ret = anx_cell_check_contract(r->owner);
	if (ret == ANX_OK) ret = anx_sched_domain_check(r->owner);
	if (ret == ANX_OK) ret = anx_identity_admit(r->owner, &identity);
	if (ret == ANX_OK && anx_uuid_compare(&identity, &r->identity)) ret = ANX_EBUSY;
	if (ret == ANX_OK) ret = anx_effect_fence_check(r->owner, NULL, NULL);
	return ret;
}
static int validate_entries(struct catalog_record *r, const struct anx_route_catalog_view *v)
{
	struct anx_route_tuning_state current;
	struct anx_route_weight_policy chosen;
	int ret = owner_current(r);
	if (ret == ANX_OK) ret = anx_route_tuning_snapshot(&current);
	for (uint32_t i = 0; ret == ANX_OK && i < v->count; i++) {
		if (anx_uuid_is_nil(&v->profiles[i])) return ANX_EINVAL;
		for (uint32_t j = 0; j < i; j++) if (!anx_uuid_compare(&v->profiles[i], &v->profiles[j])) return ANX_EEXIST;
		ret = anx_route_profile_choose(&v->profiles[i], r->owner, &current, &chosen);
	}
	return ret;
}
int anx_route_catalog_create(const anx_cid_t *owner, const anx_oid_t *profiles, uint32_t count, struct anx_route_catalog_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!owner || anx_uuid_is_nil(owner) || !profiles || !out || !count || count > ANX_ROUTE_CATALOG_ENTRIES) return ANX_EINVAL;
	struct catalog_record *r = anx_zalloc(sizeof(*r));
	if (!r) return ANX_ENOMEM;
	r->view.owner = *owner; r->view.count = count; r->view.epoch = 1;
	anx_memcpy(r->view.profiles, profiles, count * sizeof(*profiles));
	r->owner = anx_cell_store_lookup(owner);
	int ret = r->owner ? ANX_OK : ANX_ENOENT;
	if (ret == ANX_OK) { r->parent = r->owner->parent_cid; ret = anx_identity_admit(r->owner, &r->identity); }
	bool flags; anx_spin_lock_irqsave(&catalog_lock, &flags);
	uint32_t slot = ANX_ROUTE_CATALOG_MAX;
	for (uint32_t i = 0; ret == ANX_OK && i < ANX_ROUTE_CATALOG_MAX; i++) {
		if (!catalogs[i]) { if (slot == ANX_ROUTE_CATALOG_MAX) slot = i; }
		else if (!anx_uuid_compare(&catalogs[i]->view.owner, owner)) ret = ANX_EEXIST;
	}
	if (ret == ANX_OK && (slot == ANX_ROUTE_CATALOG_MAX || sequence == ~(uint64_t)0)) ret = ANX_EFULL;
	if (ret == ANX_OK) ret = validate_entries(r, &r->view);
	if (ret == ANX_OK) { r->view.id = ++sequence; catalogs[slot] = r; *out = r->view; }
	anx_spin_unlock_irqrestore(&catalog_lock, flags);
	if (ret != ANX_OK) { if (r->owner) anx_cell_store_release(r->owner); anx_free(r); }
	return ret;
}
int anx_route_catalog_replace(uint64_t id, uint64_t epoch, const anx_oid_t *profiles, uint32_t count, struct anx_route_catalog_view *out)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || !epoch || !profiles || !out || !count || count > ANX_ROUTE_CATALOG_ENTRIES) return ANX_EINVAL;
	struct anx_route_catalog_view next = {0};
	next.count = count; anx_memcpy(next.profiles, profiles, count * sizeof(*profiles));
	bool flags; anx_spin_lock_irqsave(&catalog_lock, &flags);
	struct catalog_record *r = find(id);
	int ret = r ? ANX_OK : ANX_ENOENT;
	if (ret == ANX_OK && r->view.epoch != epoch) ret = ANX_EBUSY;
	if (ret == ANX_OK && epoch == ~(uint64_t)0) ret = ANX_EFULL;
	if (ret == ANX_OK) ret = validate_entries(r, &next);
	if (ret == ANX_OK) { next.id = id; next.owner = r->view.owner; next.epoch = epoch + 1; r->view = next; *out = next; }
	anx_spin_unlock_irqrestore(&catalog_lock, flags); return ret;
}
int anx_route_catalog_choose(uint64_t id, uint64_t epoch, uint32_t index, const struct anx_cell *cell,
	const struct anx_route_tuning_state *incumbent, struct anx_route_weight_policy *out)
{
	if (!id || !epoch || !cell || !incumbent || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&catalog_lock, &flags);
	struct catalog_record *r = find(id);
	int ret = access(r);
	if (ret == ANX_OK && r->owner != cell) ret = ANX_EPERM;
	if (ret == ANX_OK && r->view.epoch != epoch) ret = ANX_EBUSY;
	if (ret == ANX_OK && index >= r->view.count) ret = ANX_EINVAL;
	if (ret == ANX_OK) ret = owner_current(r);
	if (ret == ANX_OK) ret = anx_route_profile_choose(&r->view.profiles[index], cell, incumbent, out);
	anx_spin_unlock_irqrestore(&catalog_lock, flags); return ret;
}
int anx_route_catalog_get(uint64_t id, struct anx_route_catalog_view *out)
{
	if (!id || !out) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&catalog_lock, &flags);
	struct catalog_record *r = find(id); int ret = access(r);
	if (ret == ANX_OK) *out = r->view;
	anx_spin_unlock_irqrestore(&catalog_lock, flags); return ret;
}
int anx_route_catalog_destroy(uint64_t id)
{
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id) return ANX_EINVAL;
	bool flags; anx_spin_lock_irqsave(&catalog_lock, &flags);
	struct catalog_record *r = find(id); int ret = r ? ANX_OK : ANX_ENOENT;
	if (ret == ANX_OK) {
		for (uint32_t i = 0; i < ANX_ROUTE_CATALOG_MAX; i++) if (catalogs[i] == r) catalogs[i] = NULL;
		anx_cell_store_release(r->owner); anx_memset(r, 0, sizeof(*r)); anx_free(r);
	}
	anx_spin_unlock_irqrestore(&catalog_lock, flags); return ret;
}
