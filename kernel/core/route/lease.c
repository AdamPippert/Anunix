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
		if (l->mem_tier == tier)
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
		if (l->accel == accel)
			total += l->accel_pct;
	}
	return total;
}

int anx_lease_grant(const anx_eid_t *engine_id,
		    enum anx_mem_tier tier,
		    uint64_t mem_bytes,
		    enum anx_accel_type accel,
		    uint32_t accel_pct,
		    struct anx_engine_lease **out)
{
	struct anx_engine_lease *lease;
	uint64_t mem_avail;
	uint32_t accel_avail;

	if (!engine_id || !out)
		return ANX_EINVAL;
	if ((int)tier < 0 || tier >= ANX_MEM_TIER_COUNT)
		return ANX_EINVAL;
	if ((int)accel < 0 || accel >= ANX_ACCEL_COUNT)
		return ANX_EINVAL;

	anx_spin_lock(&lease_lock);

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

	lease = anx_zalloc(sizeof(*lease));
	if (!lease) {
		anx_spin_unlock(&lease_lock);
		return ANX_ENOMEM;
	}

	lease->engine_id = *engine_id;
	lease->mem_tier = tier;
	lease->mem_reserved_bytes = mem_bytes;
	lease->mem_used_bytes = 0;
	lease->accel = accel;
	lease->accel_pct = accel_pct;
	lease->granted_at = arch_time_now();
	lease->expires_at = 0;

	anx_spin_init(&lease->lock);
	anx_list_init(&lease->lease_link);
	anx_list_add_tail(&lease->lease_link, &lease_list);

	anx_spin_unlock(&lease_lock);

	*out = lease;
	return ANX_OK;
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
		if (anx_uuid_compare(&l->engine_id, engine_id) == 0) {
			anx_spin_unlock(&lease_lock);
			return l;
		}
	}

	anx_spin_unlock(&lease_lock);
	return NULL;
}

int anx_lease_release(struct anx_engine_lease *lease)
{
	if (!lease)
		return ANX_EINVAL;

	anx_spin_lock(&lease_lock);
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
