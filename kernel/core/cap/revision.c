#include <anx/revision.h>
#include <anx/identity.h>
#include <anx/effect_fence.h>
#include <anx/uuid.h>
#include <anx/spinlock.h>

static struct anx_revision_lease_view leases[ANX_REVISION_LEASE_MAX];
static uint32_t lease_count;
static struct anx_spinlock lease_lock = ANX_SPINLOCK_INIT;

static bool valid_class(enum anx_revision_class value)
{
	return value == ANX_REVISION_PARAMETERS || value == ANX_REVISION_IMPLEMENTATION;
}

static struct anx_revision_lease_view *find_lease(const anx_oid_t *id)
{
	for (uint32_t i = 0; i < lease_count; i++)
		if (!anx_uuid_compare(id, &leases[i].id)) return &leases[i];
	return NULL;
}

int anx_revision_lease_create(enum anx_revision_class ceiling, struct anx_revision_lease_view *out)
{
	struct anx_revision_lease_view entry = {0};
	bool flags;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!valid_class(ceiling) || !out) return ANX_EINVAL;
	anx_uuid_generate(&entry.id);
	entry.ceiling = ceiling;
	anx_spin_lock_irqsave(&lease_lock, &flags);
	if (lease_count == ANX_REVISION_LEASE_MAX || find_lease(&entry.id)) {
		anx_spin_unlock_irqrestore(&lease_lock, flags);
		return ANX_EFULL;
	}
	leases[lease_count++] = entry;
	*out = entry;
	anx_spin_unlock_irqrestore(&lease_lock, flags);
	return ANX_OK;
}

int anx_revision_lease_get(const anx_oid_t *id, struct anx_revision_lease_view *out)
{
	struct anx_revision_lease_view *entry;
	bool flags;
	if (!id || anx_uuid_is_nil(id) || !out) return ANX_EINVAL;
	anx_spin_lock_irqsave(&lease_lock, &flags);
	entry = find_lease(id);
	if (entry) *out = *entry;
	anx_spin_unlock_irqrestore(&lease_lock, flags);
	return entry ? ANX_OK : ANX_ENOENT;
}

int anx_revision_lease_bind(struct anx_cell *cell, const anx_oid_t *id)
{
	struct anx_revision_lease_view entry;
	struct anx_cell *registered;
	int ret;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!cell) return ANX_EINVAL;
	ret = anx_revision_lease_get(id, &entry);
	if (ret != ANX_OK) return ret;
	if (entry.revoked) return ANX_EPERM;
	registered = anx_cell_store_lookup(&cell->cid);
	if (!registered) return ANX_ENOENT;
	if (registered != cell) { anx_cell_store_release(registered); return ANX_EPERM; }
	anx_spin_lock(&cell->lock);
	if (cell->status != ANX_CELL_CREATED || cell->runtime_active ||
	    !anx_uuid_is_nil(&cell->parent_cid) || !anx_uuid_is_nil(&cell->revision_lease_id))
		ret = ANX_EBUSY;
	else cell->revision_lease_id = *id;
	anx_spin_unlock(&cell->lock);
	anx_cell_store_release(registered);
	return ret;
}

int anx_revision_lease_revoke(const anx_oid_t *id)
{
	struct anx_revision_lease_view *entry;
	bool flags;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!id || anx_uuid_is_nil(id)) return ANX_EINVAL;
	anx_spin_lock_irqsave(&lease_lock, &flags);
	entry = find_lease(id);
	if (entry) entry->revoked = true;
	anx_spin_unlock_irqrestore(&lease_lock, flags);
	return entry ? ANX_OK : ANX_ENOENT;
}

int anx_revision_check(enum anx_revision_class required, bool require_lease)
{
	const anx_cid_t *active = anx_cell_current_id();
	struct anx_cell *cell;
	struct anx_revision_lease_view entry;
	int ret;
	if (!valid_class(required)) return ANX_EINVAL;
	if (!active) return ANX_OK;
	cell = anx_cell_store_lookup(active);
	if (!cell) return ANX_EPERM;
	if (anx_uuid_is_nil(&cell->revision_lease_id))
		ret = require_lease ? ANX_EPERM : ANX_OK;
	else {
		ret = anx_revision_lease_get(&cell->revision_lease_id, &entry);
		if (ret == ANX_OK && (entry.revoked || required > entry.ceiling ||
		    !cell->execution.allow_side_effects || anx_cell_status_terminal(cell->status))) ret = ANX_EPERM;
		if (ret == ANX_OK) ret = anx_identity_admit(cell, NULL);
		if (ret == ANX_OK) ret = anx_effect_fence_check(cell, NULL, NULL);
	}
	anx_cell_store_release(cell);
	return ret;
}
