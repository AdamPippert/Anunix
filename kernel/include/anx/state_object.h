/*
 * anx/state_object.h — State Object Model (RFC-0002).
 *
 * The State Object is the foundational primitive of Anunix, replacing
 * the POSIX file with a self-describing, policy-governed unit of state.
 */

#ifndef ANX_STATE_OBJECT_H
#define ANX_STATE_OBJECT_H

#include <anx/types.h>
#include <anx/list.h>
#include <anx/spinlock.h>
#include <anx/meta.h>
#include <anx/provenance.h>
#include <anx/access.h>
#include <anx/retention.h>

/* --- Object types (Section 5) --- */

enum anx_object_type {
	ANX_OBJ_BYTE_DATA,
	ANX_OBJ_STRUCTURED_DATA,
	ANX_OBJ_EMBEDDING,
	ANX_OBJ_GRAPH_NODE,
	ANX_OBJ_MODEL_OUTPUT,
	ANX_OBJ_EXECUTION_TRACE,
	ANX_OBJ_CAPABILITY,		/* RFC-0007 */
	ANX_OBJ_CREDENTIAL,		/* RFC-0008 */
	ANX_OBJ_SURFACE,		/* RFC-0012: Interface Plane surface */
	ANX_OBJ_EVENT,			/* RFC-0012: Interface Plane event */
	ANX_OBJ_TENSOR,			/* RFC-0013: multi-dimensional array */
	ANX_OBJ_TYPE_COUNT,
};

/* --- Object lifecycle states (Section 10) --- */

enum anx_object_state {
	ANX_OBJ_CREATING,
	ANX_OBJ_ACTIVE,
	ANX_OBJ_SEALED,
	ANX_OBJ_EXPIRED,
	ANX_OBJ_DELETED,
	ANX_OBJ_TOMBSTONE,
};

/* --- The State Object --- */

struct anx_state_object {
	/* Identity (immutable) */
	anx_oid_t oid;
	struct anx_hash content_hash;
	uint64_t version;

	/* Type & Schema (immutable after creation) */
	enum anx_object_type object_type;
	char schema_uri[256];
	char schema_version[32];

	/* Payload */
	void *payload;
	uint64_t payload_size;

	/* Metadata */
	struct anx_meta_store *system_meta;
	struct anx_meta_store *user_meta;

	/* Governance */
	struct anx_prov_log *provenance;
	struct anx_access_policy access_policy;
	struct anx_retention_policy retention;

	/* Lifecycle */
	enum anx_object_state state;
	anx_cid_t creator_cell;
	anx_oid_t *parent_oids;
	uint32_t parent_count;

	/* Kernel bookkeeping */
	struct anx_spinlock lock;
	uint32_t refcount;
	struct anx_list_head oid_link;		/* objstore hash chain */
	struct anx_list_head content_link;	/* content hash index */
};

/* --- Object Store API (Section 12) --- */

/* Parameters for creating a state object */
struct anx_so_create_params {
	enum anx_object_type object_type;
	const char *schema_uri;		/* NULL for byte_data */
	const char *schema_version;	/* NULL if no schema */
	const void *payload;
	uint64_t payload_size;
	const anx_oid_t *parent_oids;	/* derivation parents */
	uint32_t parent_count;
	anx_cid_t creator_cell;
};

/* Open modes */
enum anx_open_mode {
	ANX_OPEN_READ,
	ANX_OPEN_WRITE,
	ANX_OPEN_READWRITE,
};

/* Object handle (returned by open) */
struct anx_object_handle {
	struct anx_state_object *obj;
	enum anx_open_mode mode;
	uint64_t open_version;
};

/* Initialize the global object store */
void anx_objstore_init(void);

/* Create a new state object */
int anx_so_create(const struct anx_so_create_params *params,
		  struct anx_state_object **out);

/* Open an existing object by OID */
int anx_so_open(const anx_oid_t *oid, enum anx_open_mode mode,
		struct anx_object_handle *handle);

/* Close a handle */
void anx_so_close(struct anx_object_handle *handle);

/* Seal an object (payload becomes immutable) */
int anx_so_seal(const anx_oid_t *oid);

/* Delete an object */
int anx_so_delete(const anx_oid_t *oid, bool force);

/* Read payload bytes */
int anx_so_read_payload(struct anx_object_handle *handle,
			uint64_t offset, void *buf, uint64_t len);

/* Write payload bytes */
int anx_so_write_payload(struct anx_object_handle *handle,
			 uint64_t offset, const void *data, uint64_t len);

/* Replace entire payload */
int anx_so_replace_payload(struct anx_object_handle *handle,
			   const void *data, uint64_t len);

/* Look up an object by OID (internal, increments refcount) */
struct anx_state_object *anx_objstore_lookup(const anx_oid_t *oid);

/* Release a reference (decrements refcount) */
void anx_objstore_release(struct anx_state_object *obj);

/* Iterate all objects in the store. Callback returns 0 to continue, non-zero to stop. */
typedef int (*anx_objstore_iter_fn)(struct anx_state_object *obj, void *arg);
int anx_objstore_iterate(anx_objstore_iter_fn cb, void *arg);

#endif /* ANX_STATE_OBJECT_H */
