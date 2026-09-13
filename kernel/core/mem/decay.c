/*
 * decay.c — Memory freshness decay logic.
 *
 * A sweep advances each entry's score and expires bounded retention hints.
 * Freshness classes determine the increment; recorded accesses reduce scores.
 * The controller invokes eviction separately from decay.
 */

#include <anx/types.h>
#include <anx/memplane.h>
#include <anx/hashtable.h>
#include <anx/arch.h>

/* Decay increment per sweep, scaled by freshness class */
static uint32_t decay_rate(enum anx_freshness_class fc)
{
	switch (fc) {
	case ANX_FRESH_VOLATILE:	return 50;
	case ANX_FRESH_SHORT_HORIZON:	return 20;
	case ANX_FRESH_MEDIUM_HORIZON:	return 10;
	case ANX_FRESH_LONG_HORIZON:	return 5;
	case ANX_FRESH_ARCHIVAL:	return 1;
	}
	return 10;
}

/* Threshold at which an entry becomes stale */
#define DECAY_STALE_THRESHOLD	500

/*
 * External: the memory table. We need access to walk all entries.
 * In a real kernel this would be a proper internal interface.
 */
extern struct anx_htable mem_table;

int anx_memplane_decay_entry(struct anx_mem_entry *entry)
{
	uint32_t rate;
	if (!entry)
		return ANX_EINVAL;
	anx_spin_lock(&entry->lock);
	rate = decay_rate(entry->freshness);
	if (entry->decay_score >= 1000 - rate)
		entry->decay_score = 1000;
	else
		entry->decay_score += rate;
	if (entry->retention.sweeps && --entry->retention.sweeps == 0)
		entry->retention.priority = 0;
	if (entry->decay_score >= DECAY_STALE_THRESHOLD && entry->validation == ANX_MEMVAL_VALIDATED)
		entry->validation = ANX_MEMVAL_STALE;
	anx_spin_unlock(&entry->lock);
	return ANX_OK;
}

int anx_memplane_decay_sweep(void)
{
	uint32_t bucket_count;
	uint32_t i;

	bucket_count = 1U << mem_table.bits;

	for (i = 0; i < bucket_count; i++) {
		struct anx_list_head *head = &mem_table.buckets[i];
		struct anx_list_head *pos;

		ANX_LIST_FOR_EACH(pos, head) {
			struct anx_mem_entry *entry;

			entry = ANX_LIST_ENTRY(pos, struct anx_mem_entry,
					       store_link);

			anx_memplane_decay_entry(entry);
		}
	}

	return ANX_OK;
}
