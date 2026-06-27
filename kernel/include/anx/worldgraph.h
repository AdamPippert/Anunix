/*
 * anx/worldgraph.h — Clustered World Graph Runtime (RFC-0026).
 *
 * The world graph makes Anunix's implicit graph-of-state explicit. Models do
 * not mutate world state directly; they observe a branch, propose a patch, and
 * the patch passes a commit gate of registered validators before it merges into
 * the canonical graph. Every committed node, edge, constraint and prediction is
 * materialized as a State Object (RFC-0002) so provenance, policy and (later)
 * federated replication apply uniformly.
 *
 * Pipeline:  observe -> propose patch -> validate -> commit / reject -> provenance
 *
 * This header covers the host-testable core: the object model, the patch
 * protocol, the speculative branch manager, the provider/commit-gate registry,
 * and the trust-zoned replication gate. The cross-node wire protocol is left to
 * RFC-0006 / RFC-0015; this runtime defines what crosses it.
 */

#ifndef ANX_WORLDGRAPH_H
#define ANX_WORLDGRAPH_H

#include <anx/types.h>
#include <anx/uuid.h>
#include <anx/state_object.h>

/* --- Bounds (no hidden growth; every limit is auditable) --- */

/*
 * Conservative initial caps. The canonical graph and every branch snapshot are
 * single bounded allocations, so these keep each one page-cheap; raise them once
 * the runtime is backed by a dedicated arena rather than the shared kernel heap.
 */
#define ANX_WORLD_MAX_NODES		16
#define ANX_WORLD_MAX_EDGES		16
#define ANX_WORLD_MAX_PROVIDERS		32
#define ANX_WORLD_PATCH_MAX_OPS		24
#define ANX_WORLD_NODE_PROPS_MAX	2
#define ANX_WORLD_BRANCH_MAX_PATCHES	8

#define ANX_WORLD_DOMAIN_MAX		32
#define ANX_WORLD_LABEL_MAX		32
#define ANX_WORLD_PROVIDER_ID_MAX	48
#define ANX_WORLD_REL_MAX		32
#define ANX_WORLD_KEY_MAX		24
#define ANX_WORLD_VAL_MAX		48

/* --- Patch operations (RFC-0026 Section 4) --- */

enum anx_world_op_type {
	ANX_WOP_ADD_NODE,
	ANX_WOP_ADD_EDGE,
	ANX_WOP_UPDATE_PROPERTY,
	ANX_WOP_ATTACH_CONSTRAINT,
	ANX_WOP_ATTACH_PREDICTION,
	ANX_WOP_MARK_CONFLICT,
	ANX_WOP_RESOLVE_CONFLICT,
};

/*
 * A reference to a node within a patch. A patch that adds a node may reference
 * it later by its patch-local index; a patch acting on an existing canonical
 * node references it by oid. Forward references (using an index before the
 * matching add) are rejected.
 */
struct anx_world_ref {
	bool local;		/* true: index into this patch's added nodes */
	uint32_t index;		/* valid when local */
	anx_oid_t oid;		/* valid when !local */
};

static inline struct anx_world_ref anx_world_ref_local(uint32_t index)
{
	struct anx_world_ref r = { .local = true, .index = index,
				   .oid = ANX_UUID_NIL };
	return r;
}

static inline struct anx_world_ref anx_world_ref_oid(anx_oid_t oid)
{
	struct anx_world_ref r = { .local = false, .index = 0, .oid = oid };
	return r;
}

/* Opaque runtime types. Graphs and branches are caller-owned heap objects and
 * are not internally locked; the global provider registry is. */
struct anx_world_graph;
struct anx_world_branch;
struct anx_world_patch;

/* --- Provider manifest (RFC-0026 Section 5) --- */

/*
 * A provider declares its semantic authority: which world slices it may read,
 * which it may write, and whether it may emit action-bearing patches. The
 * commit gate uses this to reason about a model the way the scheduler reasons
 * about a process. `reads`/`writes` are comma-separated slice lists, e.g.
 * "world.physical,world.spatial". A `writes` list containing the single token
 * "*" grants write authority over every slice (used by operator/shell
 * providers); narrow it for real models.
 */
struct anx_world_manifest {
	char id[ANX_WORLD_PROVIDER_ID_MAX];
	char domain[ANX_WORLD_DOMAIN_MAX];
	char reads[128];
	char writes[128];
	bool can_execute_actions;
	char network_zone[ANX_WORLD_DOMAIN_MAX];
};

/*
 * A commit-gate validator. Runs against every staged patch on a branch during
 * commit. Returns ANX_OK to accept, or a negative error to reject the commit.
 * On rejection it may write a human-readable reason into `reason` (<= len).
 */
typedef int (*anx_world_validate_fn)(const struct anx_world_patch *patch,
				     const struct anx_world_branch *branch,
				     char *reason, size_t len, void *ctx);

/* --- Commit report --- */

struct anx_world_commit_report {
	bool accepted;
	int reason_code;		/* ANX_OK or the rejecting error */
	char reason[128];		/* which validator rejected, and why */
	uint32_t nodes_committed;
	uint32_t edges_committed;
	anx_oid_t patch_oid;		/* materialized WORLD_PATCH, nil if none */
};

/* --- Runtime lifecycle --- */

/* Reset the global provider registry. Call once at startup (and in tests). */
void anx_world_runtime_init(void);

/* --- Graph & branches (RFC-0026 Section 3, 6) --- */

/* Create an empty canonical graph. Returns NULL on allocation failure. */
struct anx_world_graph *anx_world_graph_create(const char *name);

/* Destroy a graph and any of its outstanding branches' snapshots. */
void anx_world_graph_destroy(struct anx_world_graph *g);

/* Live node / edge counts on the canonical graph. */
uint32_t anx_world_graph_node_count(const struct anx_world_graph *g);
uint32_t anx_world_graph_edge_count(const struct anx_world_graph *g);

/* Monotonic version; bumped on every committed merge (optimistic concurrency). */
uint64_t anx_world_graph_version(const struct anx_world_graph *g);

/* Read-only copies of canonical entities, so internals stay private. */
struct anx_world_node_info {
	anx_oid_t id;
	char domain[ANX_WORLD_DOMAIN_MAX];
	char label[ANX_WORLD_LABEL_MAX];
	bool conflict;
	uint32_t prop_count;
};

struct anx_world_edge_info {
	anx_oid_t id;
	anx_oid_t from;
	anx_oid_t to;
	char relation[ANX_WORLD_REL_MAX];
};

/* Copy canonical node/edge `index` into `out`. ANX_OK, or ANX_ENOENT if out of
 * range. */
int anx_world_graph_get_node(const struct anx_world_graph *g, uint32_t index,
			     struct anx_world_node_info *out);
int anx_world_graph_get_edge(const struct anx_world_graph *g, uint32_t index,
			     struct anx_world_edge_info *out);

/* Copy one property (key/value) of canonical node `node_index`. ANX_OK or
 * ANX_ENOENT. Either buffer may be NULL to skip it. */
int anx_world_graph_get_prop(const struct anx_world_graph *g,
			     uint32_t node_index, uint32_t prop_index,
			     char *key, size_t key_len,
			     char *val, size_t val_len);

/*
 * Process-wide default world graph, created lazily on first use. Gives shell
 * tools and the HTTP API a shared graph to inspect across commands within a
 * boot. Returns NULL only on allocation failure.
 */
struct anx_world_graph *anx_world_default_graph(void);

/*
 * Boot hook: register the built-in "shell.operator" provider and seed the
 * default graph with a tiny demo so a freshly booted system has something to
 * inspect. Logs a one-line summary. Idempotent enough for a single boot.
 */
void anx_world_boot_seed(void);

/*
 * Fork a speculative branch off the canonical graph. The branch holds a
 * consistent snapshot at the current version; proposing patches mutates only
 * the branch until it is committed. Returns NULL on failure.
 */
struct anx_world_branch *anx_world_branch_fork(struct anx_world_graph *g);

/* Discard a branch without committing. */
void anx_world_branch_abandon(struct anx_world_branch *b);

/* Node / edge counts on the branch's speculative snapshot. */
uint32_t anx_world_branch_node_count(const struct anx_world_branch *b);
uint32_t anx_world_branch_edge_count(const struct anx_world_branch *b);

/*
 * Provider read interface (RFC-0026 §7): read the branch's speculative state,
 * which already reflects every proposed patch. These let a validator evaluate a
 * patch against the world it would produce.
 */
int anx_world_branch_get_node(const struct anx_world_branch *b, uint32_t index,
			      struct anx_world_node_info *out);
int anx_world_branch_get_prop(const struct anx_world_branch *b,
			      uint32_t node_index, uint32_t prop_index,
			      char *key, size_t key_len,
			      char *val, size_t val_len);

/*
 * Resolve a patch ref (patch-local index or existing oid) to a node index in
 * the branch snapshot. ANX_OK and *index set, or ANX_ENOENT if it does not
 * resolve. `patch` must be one proposed onto this branch.
 */
int anx_world_branch_resolve_ref(const struct anx_world_branch *b,
				 const struct anx_world_patch *patch,
				 const struct anx_world_ref *ref,
				 uint32_t *index);

/* --- Patch protocol (RFC-0026 Section 4) --- */

/*
 * Create a patch authored by `provider_id`. The provider need not be registered
 * yet, but commit will reject a patch whose provider is unregistered or lacks
 * write authority for a touched slice. Returns NULL on failure.
 */
struct anx_world_patch *anx_world_patch_create(const char *provider_id);

/* Destroy a patch. Safe to call after commit; committed state is independent. */
void anx_world_patch_destroy(struct anx_world_patch *p);

/* Number of ops staged in a patch. */
uint32_t anx_world_patch_op_count(const struct anx_world_patch *p);

/*
 * Read-only copy of one staged op, so a provider's validator can inspect a
 * patch without touching internals. String fields carry op-specific text:
 *   ADD_NODE:          s1=domain, s2=label
 *   ADD_EDGE:          s1=relation
 *   UPDATE_PROPERTY:   s1=key,    s2=value
 *   ATTACH_CONSTRAINT: s2=expr
 *   ATTACH_PREDICTION: s2=expr,   conf=confidence_milli
 *   MARK_CONFLICT:     s2=reason
 */
struct anx_world_op_info {
	enum anx_world_op_type type;
	struct anx_world_ref a;		/* primary node ref */
	struct anx_world_ref b;		/* edge target */
	char s1[ANX_WORLD_REL_MAX];
	char s2[ANX_WORLD_VAL_MAX];
	int64_t conf;
};

/* Copy staged op `index` into `out`. ANX_OK, or ANX_ENOENT if out of range. */
int anx_world_patch_get_op(const struct anx_world_patch *p, uint32_t index,
			   struct anx_world_op_info *out);

/*
 * Stage an add-node op. On success `*out_ref` is filled with a patch-local
 * reference usable by later ops in the same patch. Returns ANX_OK or a negative
 * error (ANX_EFULL if the patch is full, ANX_EINVAL on bad args).
 */
int anx_world_patch_add_node(struct anx_world_patch *p, const char *domain,
			     const char *label, struct anx_world_ref *out_ref);

/* Stage an add-edge op between two node refs with a relation label. */
int anx_world_patch_add_edge(struct anx_world_patch *p,
			     struct anx_world_ref from,
			     struct anx_world_ref to, const char *relation);

/* Stage a property update on a node ref. */
int anx_world_patch_update_property(struct anx_world_patch *p,
				    struct anx_world_ref node,
				    const char *key, const char *value);

/* Attach a constraint expression to a node ref. */
int anx_world_patch_attach_constraint(struct anx_world_patch *p,
				      struct anx_world_ref node,
				      const char *expr);

/*
 * Attach a prediction to a node ref. `confidence_milli` is a fixed-point
 * confidence in [0, 1000] (no floating point in the kernel).
 */
int anx_world_patch_attach_prediction(struct anx_world_patch *p,
				      struct anx_world_ref node,
				      const char *expr,
				      int64_t confidence_milli);

/* Mark a node ref as conflicting, with a reason. */
int anx_world_patch_mark_conflict(struct anx_world_patch *p,
				  struct anx_world_ref node,
				  const char *reason);

/* Clear the conflict flag on a node ref. */
int anx_world_patch_resolve_conflict(struct anx_world_patch *p,
				     struct anx_world_ref node);

/*
 * Propose a patch onto a branch: validates references, applies the ops to the
 * branch snapshot, and stages the patch for the commit gate. The branch now
 * reflects the patch for speculative reads, but the canonical graph is
 * untouched. Returns ANX_OK, or a negative error (ANX_EINVAL on an unresolved
 * reference, ANX_EFULL if the branch is full).
 */
int anx_world_branch_propose(struct anx_world_branch *b,
			     struct anx_world_patch *p);

/* --- Provider / commit-gate registry (RFC-0026 Section 5) --- */

/*
 * Register a provider with its manifest and an optional commit-gate validator.
 * Re-registering the same id replaces the prior entry. `validate` may be NULL
 * for a provider that only authors patches. Returns ANX_OK, ANX_EFULL if the
 * registry is full, or ANX_EINVAL on bad args.
 */
int anx_world_provider_register(const struct anx_world_manifest *manifest,
				anx_world_validate_fn validate, void *ctx);

/* Remove a provider by id. Returns ANX_OK or ANX_ENOENT. */
int anx_world_provider_unregister(const char *id);

/* Look up a provider's manifest by id, or NULL if not registered. */
const struct anx_world_manifest *anx_world_provider_get(const char *id);

/* Number of registered providers. */
uint32_t anx_world_provider_count(void);

/*
 * Visitor over registered providers. `has_validator` is true if the provider
 * registered a commit-gate validator. Returning non-zero stops iteration and is
 * propagated as the return value.
 */
typedef int (*anx_world_provider_iter_fn)(const struct anx_world_manifest *m,
					  bool has_validator, void *arg);
int anx_world_provider_iterate(anx_world_provider_iter_fn cb, void *arg);

/*
 * Run the commit gate on a branch and, if it passes, merge the branch into the
 * canonical graph. The gate, in order:
 *   1. rejects if the canonical graph advanced since the fork (stale branch);
 *   2. rejects any patch whose provider is unregistered or lacks write
 *      authority for a slice it modifies;
 *   3. runs every registered validator against every staged patch.
 * On acceptance the branch snapshot becomes canonical, the version is bumped,
 * and each newly added node/edge/constraint/prediction is materialized as a
 * State Object with provenance naming the authoring provider. `report` (may be
 * NULL) is filled with the outcome. Returns ANX_OK on commit, or the negative
 * rejection code; on rejection the branch is left intact for revision.
 */
int anx_world_branch_commit(struct anx_world_branch *b,
			    struct anx_world_commit_report *report);

/* --- Built-in providers (RFC-0026 Section 7) --- */

/*
 * Register the constraint validator: a commit-gate provider ("constraint.
 * validator") that rejects any patch attaching a numeric constraint the
 * resulting node state violates. Constraint syntax is "<key><op><number>",
 * with op one of >  <  >=  <=  ==  != and number an integer or decimal
 * (e.g. "mass>0", "temp<=37.5"). A constraint whose property is absent, or
 * whose expression does not parse as this mini-language, is reported as a
 * violation and an accepted no-op respectively. Idempotent. Returns ANX_OK or
 * a negative error.
 */
int anx_world_constraint_validator_register(void);

/* --- Federated replication gate (RFC-0026 Section 7; seeds RFC-0006) --- */

/*
 * Replicate a committed world object to a trusted peer. Local writes are
 * authoritative; replication is policy-bound. Resolves `peer` via the Network
 * Plane node registry and refuses untrusted peers (ANX_EPERM) and unknown peers
 * (ANX_ENOENT). On success it records a PROV_MIGRATED provenance event naming
 * the peer and returns ANX_OK. The actual byte transfer belongs to the network
 * data plane; this gate decides whether it may happen and leaves the audit
 * trail.
 */
int anx_world_replicate(const anx_oid_t *committed, const anx_nid_t *peer);

#endif /* ANX_WORLDGRAPH_H */
