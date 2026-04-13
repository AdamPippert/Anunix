/*
 * meta.c — Key-value metadata store for State Objects.
 */

#include <anx/types.h>
#include <anx/meta.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/hashtable.h>

#define META_HASH_BITS	6	/* 64 buckets */

struct anx_meta_store {
	struct anx_htable ht;
};

static uint64_t meta_key_hash(const char *key)
{
	return anx_hash_bytes(key, anx_strlen(key));
}

static struct anx_meta_entry *meta_find(struct anx_meta_store *store,
					const char *key)
{
	uint64_t hash = meta_key_hash(key);
	struct anx_list_head *pos;

	ANX_HTABLE_FOR_BUCKET(pos, &store->ht, hash) {
		struct anx_meta_entry *entry =
			ANX_LIST_ENTRY(pos, struct anx_meta_entry, link);
		if (anx_strcmp(entry->key, key) == 0)
			return entry;
	}
	return NULL;
}

static void meta_free_value(struct anx_meta_value *val)
{
	if (val->type == ANX_META_STRING && val->v.str.data)
		anx_free(val->v.str.data);
	else if (val->type == ANX_META_BYTES && val->v.bytes.data)
		anx_free(val->v.bytes.data);
}

struct anx_meta_store *anx_meta_create(void)
{
	struct anx_meta_store *store = anx_zalloc(sizeof(*store));

	if (!store)
		return NULL;

	if (anx_htable_init(&store->ht, META_HASH_BITS) != ANX_OK) {
		anx_free(store);
		return NULL;
	}
	return store;
}

int anx_meta_set_str(struct anx_meta_store *store, const char *key,
		     const char *val)
{
	struct anx_meta_entry *entry = meta_find(store, key);

	if (entry) {
		meta_free_value(&entry->value);
	} else {
		entry = anx_zalloc(sizeof(*entry));
		if (!entry)
			return ANX_ENOMEM;
		anx_strlcpy(entry->key, key, ANX_META_KEY_MAX);
		anx_htable_add(&store->ht, &entry->link,
			       meta_key_hash(key));
	}

	size_t len = anx_strlen(val);
	entry->value.type = ANX_META_STRING;
	entry->value.v.str.data = anx_alloc(len + 1);
	if (!entry->value.v.str.data)
		return ANX_ENOMEM;
	anx_memcpy(entry->value.v.str.data, val, len + 1);
	entry->value.v.str.len = len;
	return ANX_OK;
}

int anx_meta_set_i64(struct anx_meta_store *store, const char *key,
		     int64_t val)
{
	struct anx_meta_entry *entry = meta_find(store, key);

	if (entry) {
		meta_free_value(&entry->value);
	} else {
		entry = anx_zalloc(sizeof(*entry));
		if (!entry)
			return ANX_ENOMEM;
		anx_strlcpy(entry->key, key, ANX_META_KEY_MAX);
		anx_htable_add(&store->ht, &entry->link,
			       meta_key_hash(key));
	}

	entry->value.type = ANX_META_INT64;
	entry->value.v.i64 = val;
	return ANX_OK;
}

int anx_meta_set_bool(struct anx_meta_store *store, const char *key,
		      bool val)
{
	struct anx_meta_entry *entry = meta_find(store, key);

	if (entry) {
		meta_free_value(&entry->value);
	} else {
		entry = anx_zalloc(sizeof(*entry));
		if (!entry)
			return ANX_ENOMEM;
		anx_strlcpy(entry->key, key, ANX_META_KEY_MAX);
		anx_htable_add(&store->ht, &entry->link,
			       meta_key_hash(key));
	}

	entry->value.type = ANX_META_BOOL;
	entry->value.v.boolean = val;
	return ANX_OK;
}

const struct anx_meta_value *anx_meta_get(struct anx_meta_store *store,
					  const char *key)
{
	struct anx_meta_entry *entry = meta_find(store, key);

	return entry ? &entry->value : NULL;
}

int anx_meta_delete(struct anx_meta_store *store, const char *key)
{
	struct anx_meta_entry *entry = meta_find(store, key);

	if (!entry)
		return ANX_ENOENT;

	anx_htable_del(&store->ht, &entry->link);
	meta_free_value(&entry->value);
	anx_free(entry);
	return ANX_OK;
}

void anx_meta_destroy(struct anx_meta_store *store)
{
	if (!store)
		return;

	uint32_t nbuckets = 1U << store->ht.bits;

	for (uint32_t i = 0; i < nbuckets; i++) {
		struct anx_list_head *pos, *tmp;

		ANX_LIST_FOR_EACH_SAFE(pos, tmp, &store->ht.buckets[i]) {
			struct anx_meta_entry *entry =
				ANX_LIST_ENTRY(pos, struct anx_meta_entry, link);
			anx_list_del(&entry->link);
			meta_free_value(&entry->value);
			anx_free(entry);
		}
	}

	anx_htable_destroy(&store->ht);
	anx_free(store);
}
