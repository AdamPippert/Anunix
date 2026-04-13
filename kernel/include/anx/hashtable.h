/*
 * anx/hashtable.h — Hash table with chaining.
 *
 * Uses intrusive list heads for chaining. Callers embed an
 * anx_list_head in their struct and provide a hash key.
 */

#ifndef ANX_HASHTABLE_H
#define ANX_HASHTABLE_H

#include <anx/types.h>
#include <anx/list.h>

struct anx_htable {
	struct anx_list_head *buckets;
	uint32_t bits;		/* log2(bucket count) */
	uint32_t count;		/* number of entries */
};

/* FNV-1a hash for arbitrary bytes */
uint64_t anx_hash_bytes(const void *data, size_t len);

/* Integer hash (for UUIDs stored as two uint64_t) */
uint64_t anx_hash_u64(uint64_t val);

/* Initialize a hash table with 2^bits buckets. Returns 0 on success. */
int anx_htable_init(struct anx_htable *ht, uint32_t bits);

/* Destroy a hash table (frees bucket array, does not free entries) */
void anx_htable_destroy(struct anx_htable *ht);

/* Add an entry at the given hash */
void anx_htable_add(struct anx_htable *ht, struct anx_list_head *node,
		    uint64_t hash);

/* Remove an entry */
void anx_htable_del(struct anx_htable *ht, struct anx_list_head *node);

/* Get the bucket list head for a hash value */
static inline struct anx_list_head *anx_htable_bucket(struct anx_htable *ht,
						      uint64_t hash)
{
	return &ht->buckets[hash & ((1U << ht->bits) - 1)];
}

/* Iterate over all entries in a bucket matching a hash */
#define ANX_HTABLE_FOR_BUCKET(pos, ht, hash) \
	ANX_LIST_FOR_EACH(pos, anx_htable_bucket(ht, hash))

#endif /* ANX_HASHTABLE_H */
