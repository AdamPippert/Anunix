#include <anx/memplane.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/uuid.h>

static bool registered(struct anx_mem_entry *entry)
{
	return entry && anx_memplane_lookup(&entry->oid) == entry;
}

int anx_memplane_hint(struct anx_mem_entry *entry, const struct anx_mem_retention_hint *hint)
{
	struct anx_state_object *object;
	int ret;
	if (!registered(entry) || !hint || hint->priority > ANX_MEM_RETENTION_PRIORITY_MAX ||
	    hint->sweeps > ANX_MEM_RETENTION_SWEEPS_MAX || (!hint->sweeps && hint->priority))
		return ANX_EINVAL;
	object = anx_objstore_lookup(&entry->oid);
	if (!object)
		return ANX_ENOENT;
	if (object->state != ANX_OBJ_ACTIVE && object->state != ANX_OBJ_SEALED)
		ret = ANX_EINVAL;
	else
		ret = anx_access_evaluate(&object->access_policy, anx_cell_current_id(),
					 &object->creator_cell, ANX_ACCESS_WRITE_META);
	anx_objstore_release(object);
	if (ret != ANX_OK)
		return ret;
	anx_spin_lock(&entry->lock);
	entry->retention = *hint;
	anx_spin_unlock(&entry->lock);
	return ANX_OK;
}

int anx_memplane_protect(struct anx_mem_entry *entry, uint32_t tiers)
{
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!registered(entry) || (tiers & ~((1U << ANX_MEM_TIER_COUNT) - 1)))
		return ANX_EINVAL;
	anx_spin_lock(&entry->lock);
	if (tiers & ~entry->tier_mask) {
		anx_spin_unlock(&entry->lock);
		return ANX_EINVAL;
	}
	entry->protected_tiers = (uint8_t)tiers;
	anx_spin_unlock(&entry->lock);
	return ANX_OK;
}

int anx_memplane_evict(const anx_oid_t *candidates, uint32_t count,
		       enum anx_mem_tier tier, anx_oid_t *victim_out)
{
	struct anx_mem_entry *entries[ANX_MEM_EVICTION_CANDIDATES_MAX];
	struct anx_mem_entry *victim = NULL;
	int32_t best_score = 0;
	int ret;
	if (anx_cell_current_id())
		return ANX_EPERM;
	if (!candidates || !victim_out || !count || count > ANX_MEM_EVICTION_CANDIDATES_MAX ||
	    (tier != ANX_MEM_L0 && tier != ANX_MEM_L1 && tier != ANX_MEM_L3))
		return ANX_EINVAL;
	/* Validate the complete pool before changing any placement. */
	for (uint32_t i = 0; i < count; i++) {
		for (uint32_t j = 0; j < i; j++)
			if (!anx_uuid_compare(&candidates[i], &candidates[j]))
				return ANX_EINVAL;
		entries[i] = anx_memplane_lookup(&candidates[i]);
		if (!entries[i])
			return ANX_ENOENT;
	}
	for (uint32_t i = 0; i < count; i++) {
		struct anx_mem_entry *entry = entries[i];
		uint32_t decay, priority;
		int32_t score;
		if (!anx_mem_in_tier(entry, tier) || (entry->protected_tiers & ANX_TIER_BIT(tier)))
			continue;
		decay = entry->decay_score > 1000 ? 1000 : entry->decay_score;
		priority = entry->retention.sweeps ? entry->retention.priority : 0;
		if (priority > ANX_MEM_RETENTION_PRIORITY_MAX)
			priority = ANX_MEM_RETENTION_PRIORITY_MAX;
		score = (int32_t)decay - (int32_t)(priority * 10);
		if (!victim || score > best_score ||
		    (score == best_score && anx_uuid_compare(&entry->oid, &victim->oid) < 0)) {
			victim = entry;
			best_score = score;
		}
	}
	if (!victim)
		return ANX_ENOENT;
	ret = anx_memplane_demote(victim, tier);
	if (ret == ANX_OK)
		*victim_out = victim->oid;
	return ret;
}
