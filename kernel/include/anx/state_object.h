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
	ANX_OBJ_VM,			/* RFC-0017: virtual machine object */
	ANX_OBJ_WORKFLOW,		/* RFC-0018: workflow object */

	/* JEPA latent-state subsystem */
	ANX_OBJ_JEPA_OBS,		/* system observation snapshot */
	ANX_OBJ_JEPA_LATENT,		/* encoded latent state vector */
	ANX_OBJ_JEPA_TRACE,		/* training trace (obs_t, action, obs_t+1, loss) */
	ANX_OBJ_JEPA_WORLD_PROFILE,	/* world profile config + schema */

	/* RFC-0020 IBAL loop primitives */
	ANX_OBJ_LOOP_SESSION,		/* iterative reasoning session */
	ANX_OBJ_BELIEF_STATE,		/* current working belief at iteration N */
	ANX_OBJ_WORLD_PROPOSAL,	/* candidate world hypothesis */
	ANX_OBJ_SCORE,			/* energy score from one EBM cell */
	ANX_OBJ_PLAN,			/* committed or candidate action plan */
	ANX_OBJ_COUNTEREXAMPLE,	/* rejected hypothesis (negative knowledge) */
	ANX_OBJ_MEMORY_CONSOLIDATION,	/* RFC-0020 Phase 5: cross-session memory */

	/* RFC-0024 media types */
	ANX_OBJ_AUDIO_CLIP,		/* PCM audio + format header */
	ANX_OBJ_VIDEO_CLIP,		/* RGBA frames + format header */

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

/* --- Information-flow labels (RFC-0028 Protected Operation ABI) --- */

/*
 * How sensitive an object's payload is, independent of who may invoke
 * operations on it. A capability grant answers "can this actor act";
 * sensitivity answers "where may this data go" — two orthogonal gates,
 * never merged into one check (see RFC-0028).
 */
enum anx_sensitivity {
	ANX_SENSITIVITY_PUBLIC,
	ANX_SENSITIVITY_INTERNAL,
	ANX_SENSITIVITY_CONFIDENTIAL,
	ANX_SENSITIVITY_RESTRICTED,
};

/* --- Staged mutation (RFC-0003 Execution Contracts extension) --- */

/*
 * A pending payload mutation that has not been made visible to
 * readers. Only one stage may be open on an object at a time —
 * concurrent staging is rejected with ANX_EBUSY rather than
 * attempting multi-version concurrency control.
 */
struct anx_staged_mutation {
	void *shadow_payload;
	uint64_t shadow_size;
	anx_cid_t staging_cell;
	uint64_t base_version;
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

	/*
	 * Pending mutation, or NULL. A closed handle does not auto-abort
	 * a pending stage — seal/delete are rejected while staged != NULL
	 * so a pending effect can only be resolved by explicit commit or
	 * abort, never silently dropped.
	 */
	struct anx_staged_mutation *staged;

	/*
	 * Information-flow label. Defaults to PUBLIC with a nil origin —
	 * zero-initialized objects need no extra work to classify (RFC-0002
	 * DG-8). sensitivity_origin is the OID of the object this
	 * classification was inherited from, if any; nil if the label was
	 * set explicitly rather than derived from a parent.
	 */
	enum anx_sensitivity sensitivity;
	anx_oid_t sensitivity_origin;

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

	/*
	 * Explicit sensitivity override. If left at ANX_SENSITIVITY_PUBLIC
	 * (the zero value) and parent_oids are given, the object inherits
	 * the highest sensitivity among its parents instead — a derived
	 * object never silently defaults to a lower classification than
	 * what it was built from. Because PUBLIC is also the "no override"
	 * sentinel, a caller that truly wants PUBLIC despite sensitive
	 * parents must call anx_object_set_sensitivity() explicitly after
	 * creation rather than relying on this field.
	 */
	enum anx_sensitivity sensitivity;
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

/* --- Staged mutation API --- */

/*
 * Begin a staged mutation on an open handle (ANX_OPEN_WRITE or
 * ANX_OPEN_READWRITE). Once staged, anx_so_write_payload and
 * anx_so_replace_payload on this handle target a private shadow
 * copy instead of the live payload, until resolved by
 * anx_object_commit or anx_object_abort.
 * Returns:
 *   ANX_OK      success
 *   ANX_EINVAL  null handle/obj, or handle opened ANX_OPEN_READ
 *   ANX_EPERM   object is sealed
 *   ANX_EBUSY   object already has a stage in progress
 *   ANX_ENOMEM  allocation failure
 */
int anx_object_stage(struct anx_object_handle *handle, anx_cid_t staging_cell);

/*
 * Atomically publish a staged mutation: the shadow payload becomes
 * the live payload, version increments exactly once, content_hash is
 * recomputed, and one ANX_PROV_MUTATED provenance event is appended.
 * Returns ANX_EINVAL if no stage is in progress.
 */
int anx_object_commit(struct anx_object_handle *handle);

/*
 * Discard a staged mutation. Live payload/version/content_hash are
 * unaffected — the mutation was never visible. The attempt is still
 * recorded as an ANX_PROV_STAGE_ABORTED provenance event so rollback
 * does not erase evidence that it happened.
 * Returns ANX_EINVAL if no stage is in progress.
 */
int anx_object_abort(struct anx_object_handle *handle);

/* --- Information-flow label API --- */

/*
 * Set an object's sensitivity explicitly, recording origin as nil
 * (this is a direct declaration, not an inheritance). Returns
 * ANX_ENOENT if oid does not resolve.
 */
int anx_object_set_sensitivity(const anx_oid_t *oid, enum anx_sensitivity level);

/*
 * Get an object's sensitivity. Returns ANX_SENSITIVITY_PUBLIC (and
 * logs nothing) if oid does not resolve — callers that need to
 * distinguish "unknown object" from "public object" should resolve
 * the OID themselves first via anx_objstore_lookup.
 */
enum anx_sensitivity anx_object_get_sensitivity(const anx_oid_t *oid);

#endif /* ANX_STATE_OBJECT_H */
