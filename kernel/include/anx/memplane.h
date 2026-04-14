/*
 * anx/memplane.h — Memory Control Plane (RFC-0004).
 *
 * The Memory Control Plane manages tiered memory: admission,
 * placement, promotion, demotion, decay, and forgetting of
 * memory-bearing State Objects.
 */

#ifndef ANX_MEMPLANE_H
#define ANX_MEMPLANE_H

#include <anx/types.h>
#include <anx/list.h>
#include <anx/spinlock.h>

/* --- Memory tiers (RFC-0004 Section 7) --- */

enum anx_mem_tier {
	ANX_MEM_L0,	/* active execution-local working set */
	ANX_MEM_L1,	/* local transient cache */
	ANX_MEM_L2,	/* local durable object store */
	ANX_MEM_L3,	/* local semantic retrieval tier */
	ANX_MEM_L4,	/* long-term structured memory */
	ANX_MEM_L5,	/* remote or federated memory extension */
	ANX_MEM_TIER_COUNT,
};

/* --- Admission profiles (RFC-0004 Section 10) --- */

enum anx_admission_profile {
	ANX_ADMIT_EPHEMERAL_ONLY,
	ANX_ADMIT_CACHEABLE,
	ANX_ADMIT_RETRIEVAL_CANDIDATE,
	ANX_ADMIT_LONG_TERM_CANDIDATE,
	ANX_ADMIT_GRAPH_CANDIDATE,
	ANX_ADMIT_QUARANTINED,
};

/* --- Memory validation states (RFC-0004 Section 16) --- */

enum anx_mem_validation_state {
	ANX_MEMVAL_UNVALIDATED,
	ANX_MEMVAL_PROVISIONAL,
	ANX_MEMVAL_VALIDATED,
	ANX_MEMVAL_CONTESTED,
	ANX_MEMVAL_SUPERSEDED,
	ANX_MEMVAL_STALE,
	ANX_MEMVAL_QUARANTINED,
};

/* --- Freshness classes (RFC-0004 Section 17) --- */

enum anx_freshness_class {
	ANX_FRESH_VOLATILE,
	ANX_FRESH_SHORT_HORIZON,
	ANX_FRESH_MEDIUM_HORIZON,
	ANX_FRESH_LONG_HORIZON,
	ANX_FRESH_ARCHIVAL,
};

/* --- Forgetting modes (RFC-0004 Section 17) --- */

enum anx_forget_mode {
	ANX_FORGET_HARD_DELETE,
	ANX_FORGET_TOMBSTONE,
	ANX_FORGET_ARCHIVE,
	ANX_FORGET_REDERIVE,
};

/* --- Memory placement record --- */

struct anx_mem_entry {
	anx_oid_t oid;				/* State Object reference */

	/* Tier placement (bitmask: object may reside in multiple tiers) */
	uint8_t tier_mask;			/* bit N = present in tier N */

	enum anx_admission_profile profile;
	enum anx_mem_validation_state validation;
	enum anx_freshness_class freshness;

	/* Trust tracking */
	uint32_t confidence_pct;		/* 0-100 */
	uint32_t contradiction_count;
	anx_time_t last_validated_at;
	anx_time_t last_accessed_at;

	/* Decay scoring (higher = more likely to be demoted/forgotten) */
	uint32_t decay_score;			/* 0-1000 */
	uint32_t access_count;

	/* Bookkeeping */
	struct anx_spinlock lock;
	struct anx_list_head store_link;	/* memplane hash chain */
};

/* Tier mask helpers */
#define ANX_TIER_BIT(tier)	(1U << (tier))

static inline bool anx_mem_in_tier(const struct anx_mem_entry *e,
				   enum anx_mem_tier tier)
{
	return (e->tier_mask & ANX_TIER_BIT(tier)) != 0;
}

/* --- Memory Control Plane API --- */

/* Initialize the memory control plane */
void anx_memplane_init(void);

/* Admit a State Object into the memory plane */
int anx_memplane_admit(const anx_oid_t *oid,
		       enum anx_admission_profile profile,
		       struct anx_mem_entry **out);

/* Look up a memory entry by OID */
struct anx_mem_entry *anx_memplane_lookup(const anx_oid_t *oid);

/* Release a memory entry reference */
void anx_memplane_release(struct anx_mem_entry *entry);

/* Promote an entry to a higher tier */
int anx_memplane_promote(struct anx_mem_entry *entry,
			 enum anx_mem_tier target_tier);

/* Demote an entry from a tier */
int anx_memplane_demote(struct anx_mem_entry *entry,
			enum anx_mem_tier tier);

/* Update validation state */
int anx_memplane_set_validation(struct anx_mem_entry *entry,
				enum anx_mem_validation_state state);

/* Record a contradiction against an entry */
int anx_memplane_add_contradiction(struct anx_mem_entry *entry);

/* Record an access (updates access_count and last_accessed_at) */
void anx_memplane_record_access(struct anx_mem_entry *entry);

/* Forget a memory entry */
int anx_memplane_forget(struct anx_mem_entry *entry,
			enum anx_forget_mode mode);

/* --- Decay API --- */

/* Run one decay sweep over all entries (called periodically) */
int anx_memplane_decay_sweep(void);

#endif /* ANX_MEMPLANE_H */
