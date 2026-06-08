/*
 * worldgraph.c — Clustered World Graph Runtime (RFC-0026).
 *
 * Models never mutate world state directly. They fork a speculative branch off
 * the canonical graph, propose a patch of claims, and the patch must clear a
 * commit gate — provider authority plus registered validators — before it
 * merges. On merge every newly added node, edge, constraint and prediction is
 * materialized as a State Object (RFC-0002), so provenance and policy apply
 * uniformly and the result is ready for trust-zoned replication.
 *
 * Provisional node ids live only inside an uncommitted branch; at commit each
 * new node's identity becomes its State Object oid, preserving the invariant
 * that a world entity's identity is the oid. See anx/worldgraph.h and RFC-0026.
 */

#include <anx/worldgraph.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/spinlock.h>
#include <anx/provenance.h>
#include <anx/netplane.h>

/* --- Internal graph representation --- */

struct world_node {
	anx_oid_t id;			/* provisional in a branch, oid once committed */
	char domain[ANX_WORLD_DOMAIN_MAX];
	char label[ANX_WORLD_LABEL_MAX];
	struct {
		char key[ANX_WORLD_KEY_MAX];
		char val[ANX_WORLD_VAL_MAX];
	} props[ANX_WORLD_NODE_PROPS_MAX];
	uint32_t prop_count;
	bool conflict;
	uint32_t origin_patch;		/* branch patch slot that added it */
};

struct world_edge {
	anx_oid_t id;
	anx_oid_t from;
	anx_oid_t to;
	char relation[ANX_WORLD_REL_MAX];
	uint32_t origin_patch;
};

struct anx_world_graph {
	char name[ANX_WORLD_LABEL_MAX];
	struct world_node nodes[ANX_WORLD_MAX_NODES];
	struct world_edge edges[ANX_WORLD_MAX_EDGES];
	uint32_t node_count;
	uint32_t edge_count;
	uint64_t version;
};

struct anx_world_branch {
	struct anx_world_graph *canonical;
	struct anx_world_graph snap;		/* working copy at fork */
	uint64_t base_version;
	uint32_t base_node_count;		/* boundary: new nodes are >= this */
	uint32_t base_edge_count;
	struct anx_world_patch *patches[ANX_WORLD_BRANCH_MAX_PATCHES];
	uint32_t patch_count;
};

/* One staged operation. The string fields carry op-specific text:
 *   ADD_NODE:           s1=domain, s2=label
 *   ADD_EDGE:           s1=relation
 *   UPDATE_PROPERTY:    s1=key,    s2=value
 *   ATTACH_CONSTRAINT:  s2=expr
 *   ATTACH_PREDICTION:  s2=expr,   conf=confidence_milli
 *   MARK_CONFLICT:      s2=reason
 */
struct world_op {
	enum anx_world_op_type type;
	struct anx_world_ref a;		/* primary node ref */
	struct anx_world_ref b;		/* edge target */
	char s1[ANX_WORLD_REL_MAX];
	char s2[ANX_WORLD_VAL_MAX];
	int64_t conf;
};

struct anx_world_patch {
	char provider_id[ANX_WORLD_PROVIDER_ID_MAX];
	struct world_op ops[ANX_WORLD_PATCH_MAX_OPS];
	uint32_t op_count;
	/* Provisional oids for ADD_NODE ops, by patch-local index. Filled when
	 * the patch is proposed onto a branch. */
	anx_oid_t local_oids[ANX_WORLD_PATCH_MAX_OPS];
	uint32_t local_count;
};

/* --- Provider registry (global) --- */

struct provider_slot {
	bool used;
	struct anx_world_manifest manifest;
	anx_world_validate_fn validate;
	void *ctx;
};

static struct provider_slot providers[ANX_WORLD_MAX_PROVIDERS];
static struct anx_spinlock provider_lock = ANX_SPINLOCK_INIT;

void anx_world_runtime_init(void)
{
	anx_spin_lock(&provider_lock);
	anx_memset(providers, 0, sizeof(providers));
	anx_spin_unlock(&provider_lock);
}

/* True if comma-separated list `list` contains the token `want`. An empty want
 * matches nothing here (authority must be explicit). Leading spaces tolerated. */
static bool list_contains(const char *list, const char *want)
{
	size_t wlen = anx_strlen(want);
	const char *p = list;

	if (wlen == 0 || !list)
		return false;

	while (*p) {
		const char *start = p;
		size_t seg;

		while (*p && *p != ',')
			p++;
		seg = (size_t)(p - start);
		if (seg > 0 && *start == ' ') {
			start++;
			seg--;
		}
		if (seg == wlen && anx_strncmp(start, want, wlen) == 0)
			return true;
		if (*p == ',')
			p++;
	}
	return false;
}

/* --- Graph & branches --- */

struct anx_world_graph *anx_world_graph_create(const char *name)
{
	struct anx_world_graph *g = anx_zalloc(sizeof(*g));

	if (!g)
		return NULL;
	anx_strlcpy(g->name, name ? name : "world", sizeof(g->name));
	g->version = 1;
	return g;
}

void anx_world_graph_destroy(struct anx_world_graph *g)
{
	if (g)
		anx_free(g);
}

uint32_t anx_world_graph_node_count(const struct anx_world_graph *g)
{
	return g ? g->node_count : 0;
}

uint32_t anx_world_graph_edge_count(const struct anx_world_graph *g)
{
	return g ? g->edge_count : 0;
}

uint64_t anx_world_graph_version(const struct anx_world_graph *g)
{
	return g ? g->version : 0;
}

struct anx_world_branch *anx_world_branch_fork(struct anx_world_graph *g)
{
	struct anx_world_branch *b;

	if (!g)
		return NULL;
	b = anx_zalloc(sizeof(*b));
	if (!b)
		return NULL;

	b->canonical = g;
	b->snap = *g;			/* consistent snapshot at this version */
	b->base_version = g->version;
	b->base_node_count = g->node_count;
	b->base_edge_count = g->edge_count;
	return b;
}

void anx_world_branch_abandon(struct anx_world_branch *b)
{
	if (b)
		anx_free(b);
}

uint32_t anx_world_branch_node_count(const struct anx_world_branch *b)
{
	return b ? b->snap.node_count : 0;
}

uint32_t anx_world_branch_edge_count(const struct anx_world_branch *b)
{
	return b ? b->snap.edge_count : 0;
}

/* --- Patch construction --- */

struct anx_world_patch *anx_world_patch_create(const char *provider_id)
{
	struct anx_world_patch *p;

	if (!provider_id || provider_id[0] == '\0')
		return NULL;
	p = anx_zalloc(sizeof(*p));
	if (!p)
		return NULL;
	anx_strlcpy(p->provider_id, provider_id, sizeof(p->provider_id));
	return p;
}

void anx_world_patch_destroy(struct anx_world_patch *p)
{
	if (p)
		anx_free(p);
}

uint32_t anx_world_patch_op_count(const struct anx_world_patch *p)
{
	return p ? p->op_count : 0;
}

/* Append a blank op, returning it, or NULL if the patch is full. */
static struct world_op *op_append(struct anx_world_patch *p,
				  enum anx_world_op_type type)
{
	struct world_op *op;

	if (p->op_count >= ANX_WORLD_PATCH_MAX_OPS)
		return NULL;
	op = &p->ops[p->op_count++];
	anx_memset(op, 0, sizeof(*op));
	op->type = type;
	return op;
}

int anx_world_patch_add_node(struct anx_world_patch *p, const char *domain,
			     const char *label, struct anx_world_ref *out_ref)
{
	struct world_op *op;
	uint32_t idx;

	if (!p || !domain || !label || !out_ref)
		return ANX_EINVAL;
	if (p->local_count >= ANX_WORLD_PATCH_MAX_OPS)
		return ANX_EFULL;
	op = op_append(p, ANX_WOP_ADD_NODE);
	if (!op)
		return ANX_EFULL;

	idx = p->local_count++;
	anx_strlcpy(op->s1, domain, sizeof(op->s1));
	anx_strlcpy(op->s2, label, sizeof(op->s2));
	op->a = anx_world_ref_local(idx);	/* op records the node it creates */
	*out_ref = op->a;
	return ANX_OK;
}

int anx_world_patch_add_edge(struct anx_world_patch *p,
			     struct anx_world_ref from,
			     struct anx_world_ref to, const char *relation)
{
	struct world_op *op;

	if (!p || !relation)
		return ANX_EINVAL;
	op = op_append(p, ANX_WOP_ADD_EDGE);
	if (!op)
		return ANX_EFULL;
	op->a = from;
	op->b = to;
	anx_strlcpy(op->s1, relation, sizeof(op->s1));
	return ANX_OK;
}

int anx_world_patch_update_property(struct anx_world_patch *p,
				    struct anx_world_ref node,
				    const char *key, const char *value)
{
	struct world_op *op;

	if (!p || !key || !value)
		return ANX_EINVAL;
	op = op_append(p, ANX_WOP_UPDATE_PROPERTY);
	if (!op)
		return ANX_EFULL;
	op->a = node;
	anx_strlcpy(op->s1, key, sizeof(op->s1));
	anx_strlcpy(op->s2, value, sizeof(op->s2));
	return ANX_OK;
}

int anx_world_patch_attach_constraint(struct anx_world_patch *p,
				      struct anx_world_ref node,
				      const char *expr)
{
	struct world_op *op;

	if (!p || !expr)
		return ANX_EINVAL;
	op = op_append(p, ANX_WOP_ATTACH_CONSTRAINT);
	if (!op)
		return ANX_EFULL;
	op->a = node;
	anx_strlcpy(op->s2, expr, sizeof(op->s2));
	return ANX_OK;
}

int anx_world_patch_attach_prediction(struct anx_world_patch *p,
				      struct anx_world_ref node,
				      const char *expr,
				      int64_t confidence_milli)
{
	struct world_op *op;

	if (!p || !expr)
		return ANX_EINVAL;
	if (confidence_milli < 0 || confidence_milli > 1000)
		return ANX_EINVAL;
	op = op_append(p, ANX_WOP_ATTACH_PREDICTION);
	if (!op)
		return ANX_EFULL;
	op->a = node;
	anx_strlcpy(op->s2, expr, sizeof(op->s2));
	op->conf = confidence_milli;
	return ANX_OK;
}

int anx_world_patch_mark_conflict(struct anx_world_patch *p,
				  struct anx_world_ref node, const char *reason)
{
	struct world_op *op;

	if (!p || !reason)
		return ANX_EINVAL;
	op = op_append(p, ANX_WOP_MARK_CONFLICT);
	if (!op)
		return ANX_EFULL;
	op->a = node;
	anx_strlcpy(op->s2, reason, sizeof(op->s2));
	return ANX_OK;
}

int anx_world_patch_resolve_conflict(struct anx_world_patch *p,
				     struct anx_world_ref node)
{
	struct world_op *op;

	if (!p)
		return ANX_EINVAL;
	op = op_append(p, ANX_WOP_RESOLVE_CONFLICT);
	if (!op)
		return ANX_EFULL;
	op->a = node;
	return ANX_OK;
}

/* --- Proposing a patch onto a branch --- */

/* Find a node in the snapshot by oid. Returns index or -1. */
static int snap_find_node(const struct anx_world_graph *g, const anx_oid_t *id)
{
	uint32_t i;

	for (i = 0; i < g->node_count; i++)
		if (anx_uuid_compare(&g->nodes[i].id, id) == 0)
			return (int)i;
	return -1;
}

/*
 * Resolve a ref to a snapshot node index during propose. A local ref uses the
 * patch's local_oids table (which must already hold the referenced index); an
 * existing ref is looked up by oid. Returns the index or -1 on failure.
 */
static int resolve_ref_index(const struct anx_world_graph *g,
			     const struct anx_world_patch *p,
			     const struct anx_world_ref *ref)
{
	if (ref->local) {
		if (ref->index >= p->local_count)
			return -1;
		return snap_find_node(g, &p->local_oids[ref->index]);
	}
	return snap_find_node(g, &ref->oid);
}

int anx_world_branch_propose(struct anx_world_branch *b,
			     struct anx_world_patch *p)
{
	uint32_t i;
	uint32_t slot;
	uint32_t local_seen = 0;

	if (!b || !p)
		return ANX_EINVAL;
	if (b->patch_count >= ANX_WORLD_BRANCH_MAX_PATCHES)
		return ANX_EFULL;

	slot = b->patch_count;

	/* Single forward pass. ADD_NODE assigns a provisional oid before any
	 * later op can reference it, so references never point forward. */
	for (i = 0; i < p->op_count; i++) {
		struct world_op *op = &p->ops[i];
		int ni;

		switch (op->type) {
		case ANX_WOP_ADD_NODE: {
			struct world_node *n;

			if (b->snap.node_count >= ANX_WORLD_MAX_NODES)
				return ANX_EFULL;
			n = &b->snap.nodes[b->snap.node_count++];
			anx_memset(n, 0, sizeof(*n));
			anx_uuid_generate(&n->id);
			anx_strlcpy(n->domain, op->s1, sizeof(n->domain));
			anx_strlcpy(n->label, op->s2, sizeof(n->label));
			n->origin_patch = slot;
			/* record provisional oid for local-ref resolution */
			p->local_oids[local_seen++] = n->id;
			break;
		}
		case ANX_WOP_ADD_EDGE: {
			int fi = resolve_ref_index(&b->snap, p, &op->a);
			int ti = resolve_ref_index(&b->snap, p, &op->b);
			struct world_edge *e;

			if (fi < 0 || ti < 0)
				return ANX_EINVAL;
			if (b->snap.edge_count >= ANX_WORLD_MAX_EDGES)
				return ANX_EFULL;
			e = &b->snap.edges[b->snap.edge_count++];
			anx_memset(e, 0, sizeof(*e));
			anx_uuid_generate(&e->id);
			e->from = b->snap.nodes[fi].id;
			e->to = b->snap.nodes[ti].id;
			anx_strlcpy(e->relation, op->s1, sizeof(e->relation));
			e->origin_patch = slot;
			break;
		}
		case ANX_WOP_UPDATE_PROPERTY: {
			struct world_node *n;
			uint32_t k;

			ni = resolve_ref_index(&b->snap, p, &op->a);
			if (ni < 0)
				return ANX_EINVAL;
			n = &b->snap.nodes[ni];
			for (k = 0; k < n->prop_count; k++) {
				if (anx_strcmp(n->props[k].key, op->s1) == 0)
					break;
			}
			if (k == n->prop_count) {
				if (n->prop_count >= ANX_WORLD_NODE_PROPS_MAX)
					return ANX_EFULL;
				n->prop_count++;
				anx_strlcpy(n->props[k].key, op->s1,
					    sizeof(n->props[k].key));
			}
			anx_strlcpy(n->props[k].val, op->s2,
				    sizeof(n->props[k].val));
			break;
		}
		case ANX_WOP_MARK_CONFLICT:
			ni = resolve_ref_index(&b->snap, p, &op->a);
			if (ni < 0)
				return ANX_EINVAL;
			b->snap.nodes[ni].conflict = true;
			break;
		case ANX_WOP_RESOLVE_CONFLICT:
			ni = resolve_ref_index(&b->snap, p, &op->a);
			if (ni < 0)
				return ANX_EINVAL;
			b->snap.nodes[ni].conflict = false;
			break;
		case ANX_WOP_ATTACH_CONSTRAINT:
		case ANX_WOP_ATTACH_PREDICTION:
			/* Validate the target resolves now; the claim is
			 * materialized as its own object at commit. */
			ni = resolve_ref_index(&b->snap, p, &op->a);
			if (ni < 0)
				return ANX_EINVAL;
			break;
		default:
			return ANX_EINVAL;
		}
	}

	b->patches[slot] = p;
	b->patch_count++;
	return ANX_OK;
}

/* --- Provider registry --- */

static struct provider_slot *find_provider_locked(const char *id)
{
	uint32_t i;

	for (i = 0; i < ANX_WORLD_MAX_PROVIDERS; i++)
		if (providers[i].used &&
		    anx_strcmp(providers[i].manifest.id, id) == 0)
			return &providers[i];
	return NULL;
}

int anx_world_provider_register(const struct anx_world_manifest *manifest,
				anx_world_validate_fn validate, void *ctx)
{
	struct provider_slot *slot;
	uint32_t i;
	int ret = ANX_EFULL;

	if (!manifest || manifest->id[0] == '\0')
		return ANX_EINVAL;

	anx_spin_lock(&provider_lock);

	slot = find_provider_locked(manifest->id);
	if (!slot) {
		for (i = 0; i < ANX_WORLD_MAX_PROVIDERS; i++) {
			if (!providers[i].used) {
				slot = &providers[i];
				break;
			}
		}
	}
	if (slot) {
		slot->used = true;
		slot->manifest = *manifest;
		slot->validate = validate;
		slot->ctx = ctx;
		ret = ANX_OK;
	}

	anx_spin_unlock(&provider_lock);
	return ret;
}

int anx_world_provider_unregister(const char *id)
{
	struct provider_slot *slot;
	int ret = ANX_ENOENT;

	if (!id)
		return ANX_EINVAL;

	anx_spin_lock(&provider_lock);
	slot = find_provider_locked(id);
	if (slot) {
		anx_memset(slot, 0, sizeof(*slot));
		ret = ANX_OK;
	}
	anx_spin_unlock(&provider_lock);
	return ret;
}

const struct anx_world_manifest *anx_world_provider_get(const char *id)
{
	struct provider_slot *slot;
	const struct anx_world_manifest *out = NULL;

	if (!id)
		return NULL;
	anx_spin_lock(&provider_lock);
	slot = find_provider_locked(id);
	if (slot)
		out = &slot->manifest;
	anx_spin_unlock(&provider_lock);
	return out;
}

uint32_t anx_world_provider_count(void)
{
	uint32_t i, n = 0;

	anx_spin_lock(&provider_lock);
	for (i = 0; i < ANX_WORLD_MAX_PROVIDERS; i++)
		if (providers[i].used)
			n++;
	anx_spin_unlock(&provider_lock);
	return n;
}

/* --- Commit gate --- */

/* Does the provider's manifest grant write authority over `domain`? Either the
 * manifest's home domain matches, or the domain appears in its writes list. */
static bool manifest_may_write(const struct anx_world_manifest *m,
			       const char *domain)
{
	if (anx_strcmp(m->domain, domain) == 0)
		return true;
	return list_contains(m->writes, domain);
}

/* Authority check: every patch's provider must be registered and hold write
 * authority over each slice (node domain) the patch creates. */
static int check_authority(struct anx_world_branch *b, char *reason, size_t len)
{
	uint32_t pi, oi;

	for (pi = 0; pi < b->patch_count; pi++) {
		struct anx_world_patch *p = b->patches[pi];
		struct provider_slot *slot;

		anx_spin_lock(&provider_lock);
		slot = find_provider_locked(p->provider_id);
		if (!slot) {
			anx_spin_unlock(&provider_lock);
			anx_snprintf(reason, len,
				     "provider %s not registered",
				     p->provider_id);
			return ANX_EPERM;
		}

		for (oi = 0; oi < p->op_count; oi++) {
			struct world_op *op = &p->ops[oi];

			if (op->type != ANX_WOP_ADD_NODE)
				continue;
			if (!manifest_may_write(&slot->manifest, op->s1)) {
				anx_snprintf(reason, len,
					     "provider %s lacks write authority for %s",
					     p->provider_id, op->s1);
				anx_spin_unlock(&provider_lock);
				return ANX_EPERM;
			}
		}
		anx_spin_unlock(&provider_lock);
	}
	return ANX_OK;
}

/* Run every registered validator against every staged patch. The first
 * negative return rejects the commit. */
static int run_validators(struct anx_world_branch *b, char *reason, size_t len)
{
	uint32_t i, pi;

	for (i = 0; i < ANX_WORLD_MAX_PROVIDERS; i++) {
		anx_world_validate_fn fn;
		void *ctx;
		char vid[ANX_WORLD_PROVIDER_ID_MAX];

		anx_spin_lock(&provider_lock);
		if (!providers[i].used || !providers[i].validate) {
			anx_spin_unlock(&provider_lock);
			continue;
		}
		fn = providers[i].validate;
		ctx = providers[i].ctx;
		anx_strlcpy(vid, providers[i].manifest.id, sizeof(vid));
		anx_spin_unlock(&provider_lock);

		for (pi = 0; pi < b->patch_count; pi++) {
			char why[96];
			int rc;

			why[0] = '\0';
			rc = fn(b->patches[pi], b, why, sizeof(why), ctx);
			if (rc != ANX_OK) {
				anx_snprintf(reason, len, "validator %s: %s",
					     vid, why[0] ? why : "rejected");
				return rc;
			}
		}
	}
	return ANX_OK;
}

/* --- Materialization --- */

/* Append a provenance event naming the authoring provider. */
static void record_provider(struct anx_state_object *obj,
			    enum anx_prov_type type, const char *provider)
{
	struct anx_prov_event ev;

	if (!obj || !obj->provenance)
		return;
	anx_memset(&ev, 0, sizeof(ev));
	ev.event_type = type;
	ev.actor_cell = ANX_UUID_NIL;
	ev.reproducible = true;
	anx_snprintf(ev.description, sizeof(ev.description),
		     "provider:%s", provider);
	anx_prov_log_append(obj->provenance, &ev);
}

/* Create a state object of `type` with a text payload and one parent. Returns
 * the new oid (nil on failure). */
static anx_oid_t materialize(enum anx_object_type type, const char *payload,
			     const anx_oid_t *parent, const char *provider)
{
	struct anx_so_create_params params;
	struct anx_state_object *obj;
	anx_oid_t nil = ANX_UUID_NIL;

	anx_memset(&params, 0, sizeof(params));
	params.object_type = type;
	params.payload = payload;
	params.payload_size = anx_strlen(payload);
	if (parent) {
		params.parent_oids = parent;
		params.parent_count = 1;
	}
	params.creator_cell = ANX_UUID_NIL;

	if (anx_so_create(&params, &obj) != ANX_OK)
		return nil;
	if (provider)
		record_provider(obj, ANX_PROV_DERIVED_FROM, provider);
	return obj->oid;
}

/* Map from a node's provisional oid to its committed object oid, built as new
 * nodes are materialized so edges and claims can be retargeted. */
struct oid_map {
	anx_oid_t from[ANX_WORLD_MAX_NODES];
	anx_oid_t to[ANX_WORLD_MAX_NODES];
	uint32_t count;
};

static void map_put(struct oid_map *m, anx_oid_t from, anx_oid_t to)
{
	if (m->count < ANX_WORLD_MAX_NODES) {
		m->from[m->count] = from;
		m->to[m->count] = to;
		m->count++;
	}
}

static anx_oid_t map_get(const struct oid_map *m, anx_oid_t id)
{
	uint32_t i;

	for (i = 0; i < m->count; i++)
		if (anx_uuid_compare(&m->from[i], &id) == 0)
			return m->to[i];
	return id;	/* pre-existing node: already an object oid */
}

/* Resolve a patch ref to a committed object oid using the provisional->object
 * map. Returns true on success. */
static bool resolve_ref_object(const struct anx_world_patch *p,
			       const struct anx_world_ref *ref,
			       const struct oid_map *m, anx_oid_t *out)
{
	if (ref->local) {
		if (ref->index >= p->local_count)
			return false;
		*out = map_get(m, p->local_oids[ref->index]);
		return true;
	}
	*out = ref->oid;	/* existing node, already an object oid */
	return true;
}

static void serialize_node(const struct world_node *n, char *buf, size_t len)
{
	anx_snprintf(buf, len, "node domain=%s label=%s conflict=%d props=%u",
		     n->domain, n->label, n->conflict ? 1 : 0, n->prop_count);
}

/* Materialize newly added nodes, edges, and attached claims as State Objects.
 * Mutates new-node ids in the snapshot to their object oids and retargets new
 * edges, so the merged canonical graph keys entities by oid. */
static void materialize_branch(struct anx_world_branch *b,
			       const anx_oid_t *patch_oids,
			       struct anx_world_commit_report *report)
{
	struct oid_map map;
	uint32_t i;

	map.count = 0;

	for (i = b->base_node_count; i < b->snap.node_count; i++) {
		struct world_node *n = &b->snap.nodes[i];
		const char *prov = b->patches[n->origin_patch]->provider_id;
		const anx_oid_t *parent = &patch_oids[n->origin_patch];
		char buf[256];
		anx_oid_t obj;

		serialize_node(n, buf, sizeof(buf));
		obj = materialize(ANX_OBJ_WORLD_NODE, buf, parent, prov);
		map_put(&map, n->id, obj);
		n->id = obj;		/* identity becomes the oid */
		if (report)
			report->nodes_committed++;
	}

	for (i = b->base_edge_count; i < b->snap.edge_count; i++) {
		struct world_edge *e = &b->snap.edges[i];
		const char *prov = b->patches[e->origin_patch]->provider_id;
		const anx_oid_t *parent = &patch_oids[e->origin_patch];
		char buf[128];
		anx_oid_t obj;

		e->from = map_get(&map, e->from);
		e->to = map_get(&map, e->to);
		anx_snprintf(buf, sizeof(buf), "edge rel=%s", e->relation);
		obj = materialize(ANX_OBJ_WORLD_EDGE, buf, parent, prov);
		e->id = obj;
		if (report)
			report->edges_committed++;
	}

	/* Attached constraints and predictions become their own objects,
	 * parented to the node they qualify. */
	for (i = 0; i < b->patch_count; i++) {
		struct anx_world_patch *p = b->patches[i];
		uint32_t oi;

		for (oi = 0; oi < p->op_count; oi++) {
			struct world_op *op = &p->ops[oi];
			anx_oid_t node_oid;
			char buf[160];

			if (op->type != ANX_WOP_ATTACH_CONSTRAINT &&
			    op->type != ANX_WOP_ATTACH_PREDICTION)
				continue;
			if (!resolve_ref_object(p, &op->a, &map, &node_oid))
				continue;
			if (op->type == ANX_WOP_ATTACH_CONSTRAINT) {
				anx_snprintf(buf, sizeof(buf),
					     "constraint %s", op->s2);
				materialize(ANX_OBJ_CONSTRAINT, buf, &node_oid,
					    p->provider_id);
			} else {
				anx_snprintf(buf, sizeof(buf),
					     "prediction %s conf=%lld", op->s2,
					     (long long)op->conf);
				materialize(ANX_OBJ_PREDICTION, buf, &node_oid,
					    p->provider_id);
			}
		}
	}
}

int anx_world_branch_commit(struct anx_world_branch *b,
			    struct anx_world_commit_report *report)
{
	struct anx_world_commit_report local;
	anx_oid_t patch_oids[ANX_WORLD_BRANCH_MAX_PATCHES];
	uint32_t i;
	int rc;

	if (!report)
		report = &local;
	anx_memset(report, 0, sizeof(*report));
	report->patch_oid = ANX_UUID_NIL;

	if (!b) {
		report->reason_code = ANX_EINVAL;
		return ANX_EINVAL;
	}

	/* 1. Optimistic concurrency: refuse a stale branch. */
	if (b->base_version != b->canonical->version) {
		report->reason_code = ANX_EBUSY;
		anx_strlcpy(report->reason, "branch is stale; rebase required",
			    sizeof(report->reason));
		return ANX_EBUSY;
	}

	/* 2. Provider write authority. */
	rc = check_authority(b, report->reason, sizeof(report->reason));
	if (rc != ANX_OK) {
		report->reason_code = rc;
		return rc;
	}

	/* 3. Registered validators (the commit gate proper). */
	rc = run_validators(b, report->reason, sizeof(report->reason));
	if (rc != ANX_OK) {
		report->reason_code = rc;
		return rc;
	}

	/* Accepted. Materialize the audit record for each patch first so nodes
	 * and edges can be parented to it. */
	for (i = 0; i < b->patch_count; i++) {
		struct anx_world_patch *p = b->patches[i];
		char buf[160];

		anx_snprintf(buf, sizeof(buf), "patch provider=%s ops=%u",
			     p->provider_id, p->op_count);
		patch_oids[i] = materialize(ANX_OBJ_WORLD_PATCH, buf, NULL,
					    p->provider_id);
	}
	if (b->patch_count > 0)
		report->patch_oid = patch_oids[b->patch_count - 1];

	materialize_branch(b, patch_oids, report);

	/* 4. Merge: the materialized snapshot becomes canonical. */
	anx_memcpy(b->canonical->nodes, b->snap.nodes,
		   sizeof(b->canonical->nodes));
	anx_memcpy(b->canonical->edges, b->snap.edges,
		   sizeof(b->canonical->edges));
	b->canonical->node_count = b->snap.node_count;
	b->canonical->edge_count = b->snap.edge_count;
	b->canonical->version = b->base_version + 1;

	report->accepted = true;
	report->reason_code = ANX_OK;
	return ANX_OK;
}

/* --- Federated replication gate --- */

int anx_world_replicate(const anx_oid_t *committed, const anx_nid_t *peer)
{
	struct anx_net_node *node;
	struct anx_state_object *obj;
	struct anx_prov_event ev;
	char name[64];

	if (!committed || !peer)
		return ANX_EINVAL;

	node = anx_netplane_lookup(peer);
	if (!node)
		return ANX_ENOENT;
	if (node->trust_zone == ANX_TRUST_UNTRUSTED)
		return ANX_EPERM;

	obj = anx_objstore_lookup(committed);
	if (!obj)
		return ANX_ENOENT;

	anx_strlcpy(name, node->name, sizeof(name));
	anx_memset(&ev, 0, sizeof(ev));
	ev.event_type = ANX_PROV_MIGRATED;
	ev.actor_cell = ANX_UUID_NIL;
	ev.reproducible = true;
	anx_snprintf(ev.description, sizeof(ev.description),
		     "replicate->%s", name);
	if (obj->provenance)
		anx_prov_log_append(obj->provenance, &ev);

	anx_objstore_release(obj);
	return ANX_OK;
}
