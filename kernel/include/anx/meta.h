/*
 * anx/meta.h — Key-value metadata store for State Objects.
 */

#ifndef ANX_META_H
#define ANX_META_H

#include <anx/types.h>
#include <anx/list.h>

/* Metadata value types */
enum anx_meta_vtype {
	ANX_META_STRING,
	ANX_META_INT64,
	ANX_META_FLOAT64,
	ANX_META_BOOL,
	ANX_META_BYTES,
};

/* A metadata value (tagged union) */
struct anx_meta_value {
	enum anx_meta_vtype type;
	union {
		struct {
			char *data;
			uint32_t len;
		} str;
		int64_t i64;
		uint64_t f64_bits;	/* stored as bits, reinterpreted */
		bool boolean;
		struct {
			uint8_t *data;
			uint32_t len;
		} bytes;
	} v;
};

/* Maximum key length */
#define ANX_META_KEY_MAX	256

/* A single metadata entry */
struct anx_meta_entry {
	char key[ANX_META_KEY_MAX];
	struct anx_meta_value value;
	struct anx_list_head link;	/* for chaining in hash bucket */
};

/* Metadata store */
struct anx_meta_store;

/* Create an empty metadata store */
struct anx_meta_store *anx_meta_create(void);

/* Set a string value */
int anx_meta_set_str(struct anx_meta_store *store, const char *key,
		     const char *val);

/* Set an integer value */
int anx_meta_set_i64(struct anx_meta_store *store, const char *key,
		     int64_t val);

/* Set a boolean value */
int anx_meta_set_bool(struct anx_meta_store *store, const char *key,
		      bool val);

/* Get a value by key. Returns NULL if not found. */
const struct anx_meta_value *anx_meta_get(struct anx_meta_store *store,
					  const char *key);

/* Delete a key. Returns 0 on success, ANX_ENOENT if not found. */
int anx_meta_delete(struct anx_meta_store *store, const char *key);

/* Destroy a metadata store and all entries */
void anx_meta_destroy(struct anx_meta_store *store);

#endif /* ANX_META_H */
