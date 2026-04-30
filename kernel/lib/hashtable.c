/*
 * hashtable.c — Hash table implementation.
 */

#include <anx/types.h>
#include <anx/hashtable.h>
#include <anx/alloc.h>

/* FNV-1a constants for 64-bit */
#define FNV_OFFSET	0xcbf29ce484222325ULL
#define FNV_PRIME	0x100000001b3ULL

uint64_t anx_hash_bytes(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint64_t hash = FNV_OFFSET;

	for (size_t i = 0; i < len; i++) {
		hash ^= p[i];
		hash *= FNV_PRIME;
	}
	return hash;
}

uint64_t anx_hash_u64(uint64_t val)
{
	/* Murmur-style finalization mix */
	val ^= val >> 33;
	val *= 0xff51afd7ed558ccdULL;
	val ^= val >> 33;
	val *= 0xc4ceb9fe1a85ec53ULL;
	val ^= val >> 33;
	return val;
}

int anx_htable_init(struct anx_htable *ht, uint32_t bits)
{
	uint32_t nbuckets = 1U << bits;
	uint32_t i;

	ht->buckets = anx_alloc(nbuckets * sizeof(struct anx_list_head));
	if (!ht->buckets)
		return ANX_ENOMEM;

	ht->bits = bits;
	ht->count = 0;

	for (i = 0; i < nbuckets; i++)
		anx_list_init(&ht->buckets[i]);

	return ANX_OK;
}

void anx_htable_destroy(struct anx_htable *ht)
{
	if (ht->buckets) {
		anx_free(ht->buckets);
		ht->buckets = NULL;
	}
	ht->count = 0;
}

void anx_htable_add(struct anx_htable *ht, struct anx_list_head *node,
		    uint64_t hash)
{
	struct anx_list_head *bucket = anx_htable_bucket(ht, hash);

	anx_list_add(node, bucket);
	ht->count++;
}

void anx_htable_del(struct anx_htable *ht, struct anx_list_head *node)
{
	anx_list_del(node);
	ht->count--;
}
