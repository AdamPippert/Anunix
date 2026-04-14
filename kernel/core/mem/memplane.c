/*
 * memplane.c — Memory Control Plane implementation.
 *
 * Manages memory entries: admission, placement, promotion,
 * demotion, validation tracking, and forgetting. Uses a hash
 * table keyed by State Object OID.
 */

#include <anx/types.h>
#include <anx/memplane.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/hashtable.h>
#include <anx/arch.h>

#define MEMPLANE_STORE_BITS	8	/* 256 buckets */

/* Non-static: decay.c needs access via extern */
struct anx_htable mem_table;

void anx_memplane_init(void)
{
	anx_htable_init(&mem_table, MEMPLANE_STORE_BITS);
}

/*
 * Determine initial tier placement from admission profile.
 */
static uint8_t profile_to_tiers(enum anx_admission_profile profile)
{
	switch (profile) {
	case ANX_ADMIT_EPHEMERAL_ONLY:
		return ANX_TIER_BIT(ANX_MEM_L0);
	case ANX_ADMIT_CACHEABLE:
		return ANX_TIER_BIT(ANX_MEM_L0) | ANX_TIER_BIT(ANX_MEM_L1);
	case ANX_ADMIT_RETRIEVAL_CANDIDATE:
		return ANX_TIER_BIT(ANX_MEM_L2) | ANX_TIER_BIT(ANX_MEM_L3);
	case ANX_ADMIT_LONG_TERM_CANDIDATE:
		return ANX_TIER_BIT(ANX_MEM_L2);
	case ANX_ADMIT_GRAPH_CANDIDATE:
		return ANX_TIER_BIT(ANX_MEM_L2) | ANX_TIER_BIT(ANX_MEM_L4);
	case ANX_ADMIT_QUARANTINED:
		return ANX_TIER_BIT(ANX_MEM_L1);
	}
	return ANX_TIER_BIT(ANX_MEM_L0);
}

/*
 * Determine initial freshness class from admission profile.
 */
static enum anx_freshness_class profile_to_freshness(
	enum anx_admission_profile profile)
{
	switch (profile) {
	case ANX_ADMIT_EPHEMERAL_ONLY:
		return ANX_FRESH_VOLATILE;
	case ANX_ADMIT_CACHEABLE:
		return ANX_FRESH_SHORT_HORIZON;
	case ANX_ADMIT_RETRIEVAL_CANDIDATE:
		return ANX_FRESH_MEDIUM_HORIZON;
	case ANX_ADMIT_LONG_TERM_CANDIDATE:
		return ANX_FRESH_LONG_HORIZON;
	case ANX_ADMIT_GRAPH_CANDIDATE:
		return ANX_FRESH_LONG_HORIZON;
	case ANX_ADMIT_QUARANTINED:
		return ANX_FRESH_SHORT_HORIZON;
	}
	return ANX_FRESH_VOLATILE;
}

int anx_memplane_admit(const anx_oid_t *oid,
		       enum anx_admission_profile profile,
		       struct anx_mem_entry **out)
{
	struct anx_mem_entry *entry;
	uint64_t hash;

	if (!oid)
		return ANX_EINVAL;

	/* Check if already admitted */
	entry = anx_memplane_lookup(oid);
	if (entry) {
		anx_memplane_release(entry);
		return ANX_EEXIST;
	}

	entry = anx_zalloc(sizeof(*entry));
	if (!entry)
		return ANX_ENOMEM;

	entry->oid = *oid;
	entry->profile = profile;
	entry->tier_mask = profile_to_tiers(profile);
	entry->freshness = profile_to_freshness(profile);
	entry->validation = ANX_MEMVAL_UNVALIDATED;
	entry->confidence_pct = 50;	/* default: unknown confidence */
	entry->last_accessed_at = arch_time_now();

	anx_spin_init(&entry->lock);
	anx_list_init(&entry->store_link);

	hash = anx_uuid_hash(&entry->oid);
	anx_htable_add(&mem_table, &entry->store_link, hash);

	if (out)
		*out = entry;
	return ANX_OK;
}

struct anx_mem_entry *anx_memplane_lookup(const anx_oid_t *oid)
{
	uint64_t hash = anx_uuid_hash(oid);
	struct anx_list_head *pos;

	ANX_HTABLE_FOR_BUCKET(pos, &mem_table, hash) {
		struct anx_mem_entry *entry;

		entry = ANX_LIST_ENTRY(pos, struct anx_mem_entry, store_link);
		if (anx_uuid_compare(&entry->oid, oid) == 0)
			return entry;
	}
	return NULL;
}

void anx_memplane_release(struct anx_mem_entry *entry)
{
	/* Memory entries don't use refcounting yet — placeholder */
	(void)entry;
}

int anx_memplane_promote(struct anx_mem_entry *entry,
			 enum anx_mem_tier target_tier)
{
	if (!entry)
		return ANX_EINVAL;
	if ((int)target_tier < 0 || target_tier >= ANX_MEM_TIER_COUNT)
		return ANX_EINVAL;

	/*
	 * L4 promotion requires at least provisional validation
	 * (RFC-0004 Section 15.5 Rule 1).
	 */
	if (target_tier == ANX_MEM_L4) {
		if (entry->validation < ANX_MEMVAL_PROVISIONAL)
			return ANX_EPERM;
	}

	/*
	 * L5 promotion requires explicit policy check.
	 * Stubbed: always deny for now until network plane exists.
	 */
	if (target_tier == ANX_MEM_L5)
		return ANX_EPERM;

	anx_spin_lock(&entry->lock);
	entry->tier_mask |= ANX_TIER_BIT(target_tier);
	anx_spin_unlock(&entry->lock);

	return ANX_OK;
}

int anx_memplane_demote(struct anx_mem_entry *entry,
			enum anx_mem_tier tier)
{
	if (!entry)
		return ANX_EINVAL;
	if ((int)tier < 0 || tier >= ANX_MEM_TIER_COUNT)
		return ANX_EINVAL;

	anx_spin_lock(&entry->lock);
	entry->tier_mask &= ~ANX_TIER_BIT(tier);
	anx_spin_unlock(&entry->lock);

	return ANX_OK;
}

int anx_memplane_set_validation(struct anx_mem_entry *entry,
				enum anx_mem_validation_state state)
{
	if (!entry)
		return ANX_EINVAL;

	anx_spin_lock(&entry->lock);
	entry->validation = state;
	entry->last_validated_at = arch_time_now();
	anx_spin_unlock(&entry->lock);

	return ANX_OK;
}

int anx_memplane_add_contradiction(struct anx_mem_entry *entry)
{
	if (!entry)
		return ANX_EINVAL;

	anx_spin_lock(&entry->lock);
	entry->contradiction_count++;

	/*
	 * Auto-contest: if contradictions exceed threshold,
	 * mark as contested (RFC-0004 Section 16.3).
	 */
	if (entry->contradiction_count >= 2 &&
	    entry->validation != ANX_MEMVAL_QUARANTINED)
		entry->validation = ANX_MEMVAL_CONTESTED;

	anx_spin_unlock(&entry->lock);
	return ANX_OK;
}

void anx_memplane_record_access(struct anx_mem_entry *entry)
{
	if (!entry)
		return;

	anx_spin_lock(&entry->lock);
	entry->access_count++;
	entry->last_accessed_at = arch_time_now();

	/* Reduce decay score on access (min 0) */
	if (entry->decay_score > 10)
		entry->decay_score -= 10;
	else
		entry->decay_score = 0;

	anx_spin_unlock(&entry->lock);
}

int anx_memplane_forget(struct anx_mem_entry *entry,
			enum anx_forget_mode mode)
{
	if (!entry)
		return ANX_EINVAL;

	switch (mode) {
	case ANX_FORGET_HARD_DELETE:
		anx_htable_del(&mem_table, &entry->store_link);
		anx_free(entry);
		break;

	case ANX_FORGET_TOMBSTONE:
		/* Clear tier placement, keep entry for audit */
		anx_spin_lock(&entry->lock);
		entry->tier_mask = 0;
		entry->validation = ANX_MEMVAL_SUPERSEDED;
		anx_spin_unlock(&entry->lock);
		break;

	case ANX_FORGET_ARCHIVE:
		/* Demote to lowest cost — clear hot tiers */
		anx_spin_lock(&entry->lock);
		entry->tier_mask = ANX_TIER_BIT(ANX_MEM_L2);
		entry->freshness = ANX_FRESH_ARCHIVAL;
		anx_spin_unlock(&entry->lock);
		break;

	case ANX_FORGET_REDERIVE:
		/* Remove from retrieval tiers, keep durable copy */
		anx_spin_lock(&entry->lock);
		entry->tier_mask &= ~(ANX_TIER_BIT(ANX_MEM_L0) |
				      ANX_TIER_BIT(ANX_MEM_L1) |
				      ANX_TIER_BIT(ANX_MEM_L3));
		anx_spin_unlock(&entry->lock);
		break;
	}

	return ANX_OK;
}
