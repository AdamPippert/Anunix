/*
 * lease.c — Resource lease management for model engines.
 *
 * Tracks memory and accelerator reservations. Models are few
 * (single digits), so a flat linked list is appropriate.
 */

#include <anx/types.h>
#include <anx/engine_lease.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/arch.h>
#include <anx/cell.h>

/* Total capacity (set by hardware probing, defaults for QEMU) */
static uint64_t total_mem_per_tier[ANX_MEM_TIER_COUNT];
static uint32_t total_accel_pct[ANX_ACCEL_COUNT];

/* Active leases */
static struct anx_list_head lease_list;
static struct anx_spinlock lease_lock;

void anx_lease_init(void)
{
	uint32_t i;

	anx_list_init(&lease_list);
	anx_spin_init(&lease_lock);

	/* Default capacity: 16 GiB per tier, 100% per accelerator */
	for (i = 0; i < ANX_MEM_TIER_COUNT; i++)
		total_mem_per_tier[i] = 16ULL * 1024 * 1024 * 1024;
	for (i = 0; i < ANX_ACCEL_COUNT; i++)
		total_accel_pct[i] = 100;
}

/* Sum reserved memory for a tier across all active leases */
static uint64_t sum_reserved_mem(enum anx_mem_tier tier)
{
	struct anx_list_head *pos;
	uint64_t total = 0;

	ANX_LIST_FOR_EACH(pos, &lease_list) {
		struct anx_engine_lease *l;

		l = ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link);
		if (!l->parent && !l->revoked && l->mem_tier == tier)
			total += l->mem_reserved_bytes;
	}
	return total;
}

/* Sum reserved accelerator percentage across all active leases */
static uint32_t sum_reserved_accel(enum anx_accel_type accel)
{
	struct anx_list_head *pos;
	uint32_t total = 0;

	ANX_LIST_FOR_EACH(pos, &lease_list) {
		struct anx_engine_lease *l;

		l = ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link);
		if (!l->parent && !l->revoked && l->accel == accel)
			total += l->accel_pct;
	}
	return total;
}

/* Helpers run under lease_lock. Records remain alive until explicit leaf release. */
static bool registered(const struct anx_engine_lease *lease)
{
	struct anx_list_head *pos;
	ANX_LIST_FOR_EACH(pos, &lease_list)
		if (ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link) == lease) return true;
	return false;
}

static bool engine_leased(const anx_eid_t *id)
{
	struct anx_list_head *pos;
	ANX_LIST_FOR_EACH(pos, &lease_list) {
		struct anx_engine_lease *lease = ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link);
		if (!anx_uuid_compare(&lease->engine_id, id)) return true;
	}
	return false;
}

static int create_lease(const anx_eid_t *id, struct anx_engine_lease *parent, enum anx_mem_tier tier,
			uint64_t bytes, enum anx_accel_type accel, uint32_t pct, struct anx_engine_lease **out)
{
	struct anx_engine_lease *lease = anx_zalloc(sizeof(*lease));
	if (!lease) return ANX_ENOMEM;
	lease->engine_id = *id;
	lease->parent = parent;
	lease->depth = parent ? parent->depth + 1 : 0;
	lease->mem_tier = tier;
	lease->mem_reserved_bytes = bytes;
	lease->accel = accel;
	lease->accel_pct = pct;
	lease->granted_at = arch_time_now();
	anx_spin_init(&lease->lock);
	anx_list_init(&lease->lease_link);
	anx_list_add_tail(&lease->lease_link, &lease_list);
	*out = lease;
	return ANX_OK;
}

int anx_lease_grant(const anx_eid_t *engine_id,
		    enum anx_mem_tier tier,
		    uint64_t mem_bytes,
		    enum anx_accel_type accel,
		    uint32_t accel_pct,
		    struct anx_engine_lease **out)
{
	uint64_t mem_avail;
	uint32_t accel_avail;
	int ret;

	if (!out) return ANX_EINVAL;
	*out = NULL;
	if (!engine_id || anx_uuid_is_nil(engine_id))
		return ANX_EINVAL;
	if ((int)tier < 0 || tier >= ANX_MEM_TIER_COUNT)
		return ANX_EINVAL;
	if ((int)accel < 0 || accel >= ANX_ACCEL_COUNT)
		return ANX_EINVAL;
	if (accel_pct > 100 || (accel == ANX_ACCEL_NONE && accel_pct)) return ANX_EINVAL;

	anx_spin_lock(&lease_lock);
	if (engine_leased(engine_id)) {
		anx_spin_unlock(&lease_lock);
		return ANX_EEXIST;
	}

	/* Check memory availability */
	mem_avail = total_mem_per_tier[tier] - sum_reserved_mem(tier);
	if (mem_bytes > mem_avail) {
		anx_spin_unlock(&lease_lock);
		return ANX_ENOMEM;
	}

	/* Check accelerator availability */
	if (accel != ANX_ACCEL_NONE) {
		accel_avail = total_accel_pct[accel] - sum_reserved_accel(accel);
		if (accel_pct > accel_avail) {
			anx_spin_unlock(&lease_lock);
			return ANX_ENOMEM;
		}
	}

	ret = create_lease(engine_id, NULL, tier, mem_bytes, accel, accel_pct, out);
	anx_spin_unlock(&lease_lock);
	return ret;
}

struct anx_engine_lease *anx_lease_lookup(const anx_eid_t *engine_id)
{
	struct anx_list_head *pos;

	if (!engine_id)
		return NULL;

	anx_spin_lock(&lease_lock);

	ANX_LIST_FOR_EACH(pos, &lease_list) {
		struct anx_engine_lease *l;

		l = ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link);
		if (!l->revoked && anx_uuid_compare(&l->engine_id, engine_id) == 0) {
			anx_spin_unlock(&lease_lock);
			return l;
		}
	}

	anx_spin_unlock(&lease_lock);
	return NULL;
}

int anx_lease_resize(struct anx_engine_lease *lease, uint64_t bytes, uint32_t pct)
{
	struct anx_list_head *pos;
	uint64_t available, spare;
	uint32_t percent, spare_pct;
	int ret = ANX_OK;
	if (anx_cell_current_id()) return ANX_EPERM;
	if (!lease || pct > 100) return ANX_EINVAL;
	anx_spin_lock(&lease_lock);
	if (!registered(lease)) { ret = ANX_ENOENT; goto done; }
	if (lease->revoked) { ret = ANX_EPERM; goto done; }
	if ((int)lease->mem_tier < 0 || lease->mem_tier >= ANX_MEM_TIER_COUNT ||
	    (int)lease->accel < 0 || lease->accel >= ANX_ACCEL_COUNT ||
	    (lease->accel == ANX_ACCEL_NONE && pct)) { ret = ANX_EINVAL; goto done; }
	/* A shrink cannot consume usage or a child's independent reservation. */
	if (lease->mem_used_bytes > bytes) { ret = ANX_EBUSY; goto done; }
	spare = bytes - lease->mem_used_bytes; spare_pct = pct;
	ANX_LIST_FOR_EACH(pos, &lease_list) {
		const struct anx_engine_lease *child = ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link);
		if (child->parent != lease || child->revoked) continue;
		if (child->mem_reserved_bytes > spare || child->accel_pct > spare_pct) { ret = ANX_EBUSY; goto done; }
		spare -= child->mem_reserved_bytes; spare_pct -= child->accel_pct;
	}
	if (lease->parent) {
		if (!registered(lease->parent) || lease->parent->revoked) { ret = ANX_EPERM; goto done; }
		available = lease->parent->mem_reserved_bytes; percent = lease->parent->accel_pct;
		if (lease->parent->mem_used_bytes > available) { ret = ANX_ENOMEM; goto done; }
		available -= lease->parent->mem_used_bytes;
	} else {
		available = total_mem_per_tier[lease->mem_tier]; percent = total_accel_pct[lease->accel];
	}
	ANX_LIST_FOR_EACH(pos, &lease_list) {
		const struct anx_engine_lease *other = ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link);
		if (other == lease || other->revoked || other->parent != lease->parent) continue;
		if (lease->parent || other->mem_tier == lease->mem_tier) {
			if (other->mem_reserved_bytes > available) { ret = ANX_ENOMEM; goto done; }
			available -= other->mem_reserved_bytes;
		}
		if (lease->accel != ANX_ACCEL_NONE && (lease->parent || other->accel == lease->accel)) {
			if (other->accel_pct > percent) { ret = ANX_ENOMEM; goto done; }
			percent -= other->accel_pct;
		}
	}
	if (bytes > available || pct > percent) { ret = ANX_ENOMEM; goto done; }
	lease->mem_reserved_bytes = bytes; lease->accel_pct = pct;
done:
	anx_spin_unlock(&lease_lock);
	return ret;
}

int anx_lease_grant_child(struct anx_engine_lease *parent, const anx_eid_t *engine_id,
			  uint64_t mem_bytes, uint32_t accel_pct, struct anx_engine_lease **out)
{
	struct anx_list_head *pos;
	uint64_t available;
	uint32_t pct;
	int ret = ANX_OK;
	if (!out) return ANX_EINVAL;
	*out = NULL;
	if (!parent || !engine_id || anx_uuid_is_nil(engine_id) || accel_pct > 100) return ANX_EINVAL;
	anx_spin_lock(&lease_lock);
	if (!registered(parent)) { ret = ANX_ENOENT; goto done; }
	if (parent->revoked || parent->depth >= ANX_LEASE_DEPTH_MAX) { ret = ANX_EPERM; goto done; }
	if (parent->accel == ANX_ACCEL_NONE && accel_pct) { ret = ANX_EINVAL; goto done; }
	if (engine_leased(engine_id)) { ret = ANX_EEXIST; goto done; }
	available = parent->mem_reserved_bytes;
	pct = parent->accel_pct;
	if (parent->mem_used_bytes > available) { ret = ANX_ENOMEM; goto done; }
	available -= parent->mem_used_bytes;
	ANX_LIST_FOR_EACH(pos, &lease_list) {
		struct anx_engine_lease *child = ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link);
		if (child->parent != parent || child->revoked) continue;
		if (child->mem_reserved_bytes > available || child->accel_pct > pct) { ret = ANX_ENOMEM; goto done; }
		available -= child->mem_reserved_bytes;
		pct -= child->accel_pct;
	}
	if (mem_bytes > available || accel_pct > pct) { ret = ANX_ENOMEM; goto done; }
	ret = create_lease(engine_id, parent, parent->mem_tier, mem_bytes, parent->accel, accel_pct, out);
done:
	anx_spin_unlock(&lease_lock);
	return ret;
}

static bool in_subtree(const struct anx_engine_lease *candidate, const struct anx_engine_lease *root)
{
	for (uint32_t i = 0; candidate && i <= ANX_LEASE_DEPTH_MAX; i++, candidate = candidate->parent)
		if (candidate == root) return true;
	return false;
}

int anx_lease_revoke(struct anx_engine_lease *lease)
{
	struct anx_list_head *pos;
	if (!lease) return ANX_EINVAL;
	anx_spin_lock(&lease_lock);
	if (!registered(lease)) { anx_spin_unlock(&lease_lock); return ANX_ENOENT; }
	/* Validate the whole subtree before changing any reservation. */
	ANX_LIST_FOR_EACH(pos, &lease_list) {
		struct anx_engine_lease *child = ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link);
		if (in_subtree(child, lease) && child->mem_used_bytes) {
			anx_spin_unlock(&lease_lock);
			return ANX_EBUSY;
		}
	}
	ANX_LIST_FOR_EACH(pos, &lease_list) {
		struct anx_engine_lease *child = ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link);
		if (in_subtree(child, lease)) child->revoked = true;
	}
	anx_spin_unlock(&lease_lock);
	return ANX_OK;
}

int anx_lease_release(struct anx_engine_lease *lease)
{
	struct anx_list_head *pos;
	if (!lease)
		return ANX_EINVAL;

	anx_spin_lock(&lease_lock);
	if (!registered(lease)) { anx_spin_unlock(&lease_lock); return ANX_ENOENT; }
	if (lease->mem_used_bytes) { anx_spin_unlock(&lease_lock); return ANX_EBUSY; }
	ANX_LIST_FOR_EACH(pos, &lease_list) {
		struct anx_engine_lease *child = ANX_LIST_ENTRY(pos, struct anx_engine_lease, lease_link);
		if (child->parent == lease) { anx_spin_unlock(&lease_lock); return ANX_EBUSY; }
	}
	anx_list_del(&lease->lease_link);
	anx_spin_unlock(&lease_lock);

	anx_free(lease);
	return ANX_OK;
}

int anx_lease_avail_mem(enum anx_mem_tier tier, uint64_t *avail_out)
{
	if (!avail_out)
		return ANX_EINVAL;
	if ((int)tier < 0 || tier >= ANX_MEM_TIER_COUNT)
		return ANX_EINVAL;

	anx_spin_lock(&lease_lock);
	*avail_out = total_mem_per_tier[tier] - sum_reserved_mem(tier);
	anx_spin_unlock(&lease_lock);

	return ANX_OK;
}

int anx_lease_avail_accel(enum anx_accel_type accel, uint32_t *pct_out)
{
	if (!pct_out)
		return ANX_EINVAL;
	if ((int)accel < 0 || accel >= ANX_ACCEL_COUNT)
		return ANX_EINVAL;

	anx_spin_lock(&lease_lock);
	*pct_out = total_accel_pct[accel] - sum_reserved_accel(accel);
	anx_spin_unlock(&lease_lock);

	return ANX_OK;
}
