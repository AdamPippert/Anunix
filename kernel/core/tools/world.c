/*
 * world.c — ansh frontend for the clustered world graph runtime (RFC-0026).
 *
 *   world status                       graph version, node/edge/provider counts
 *   world ls                           list canonical nodes and edges
 *   world show <idx>                   detail one node (oid, props, conflict)
 *   world providers                    list registered providers + authority
 *   world add <domain> <label>         commit a new node (one-shot patch)
 *   world link <from> <to> <relation>  commit an edge between two node indices
 *   world demo                         run the full propose->validate->commit
 *                                      pipeline and print the commit report
 *
 * Every mutation goes through the same path a model would: fork a branch,
 * propose a patch, clear the commit gate, merge. The shell speaks as the
 * built-in "shell.operator" provider, which holds wildcard write authority.
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/worldgraph.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>

static void short_oid(const anx_oid_t *id, char *buf, size_t len)
{
	char full[40];

	anx_uuid_to_string(id, full, sizeof(full));
	/* Skip the first group: in a UUIDv7 it is a millisecond timestamp,
	 * which reads all-zero before the clock is set, so the entropy that
	 * distinguishes objects at a glance lives just past the first dash. */
	anx_strlcpy(buf, full + 9, len < 10 ? len : 10);
}

/* Resolve a canonical node oid to its list index, or -1. */
static int index_of_oid(struct anx_world_graph *g, const anx_oid_t *id)
{
	uint32_t i, n = anx_world_graph_node_count(g);
	struct anx_world_node_info info;

	for (i = 0; i < n; i++) {
		if (anx_world_graph_get_node(g, i, &info) != ANX_OK)
			continue;
		if (anx_uuid_compare(&info.id, id) == 0)
			return (int)i;
	}
	return -1;
}

static bool parse_u32(const char *s, uint32_t *out)
{
	uint32_t v = 0;

	if (!s || !*s)
		return false;
	while (*s) {
		if (*s < '0' || *s > '9')
			return false;
		v = v * 10 + (uint32_t)(*s - '0');
		s++;
	}
	*out = v;
	return true;
}

/* Ensure the operator provider exists (boot seeds it, but be defensive). */
static void ensure_operator(void)
{
	struct anx_world_manifest m;

	if (anx_world_provider_get("shell.operator"))
		return;
	anx_memset(&m, 0, sizeof(m));
	anx_strlcpy(m.id, "shell.operator", sizeof(m.id));
	anx_strlcpy(m.domain, "shell", sizeof(m.domain));
	anx_strlcpy(m.writes, "*", sizeof(m.writes));
	anx_world_provider_register(&m, NULL, NULL);
}

static void print_report(const struct anx_world_commit_report *rep)
{
	char oid[40];

	if (rep->accepted) {
		anx_uuid_to_string(&rep->patch_oid, oid, sizeof(oid));
		kprintf("committed: +%u nodes +%u edges (patch %s)\n",
			rep->nodes_committed, rep->edges_committed, oid);
	} else {
		kprintf("rejected (%d): %s\n", rep->reason_code,
			rep->reason[0] ? rep->reason : "no reason");
	}
}

static void world_status(struct anx_world_graph *g)
{
	kprintf("world graph: %s\n", "anx:world/default");
	kprintf("  version:   %u\n",
		(unsigned)anx_world_graph_version(g));
	kprintf("  nodes:     %u\n", anx_world_graph_node_count(g));
	kprintf("  edges:     %u\n", anx_world_graph_edge_count(g));
	kprintf("  providers: %u\n", anx_world_provider_count());
}

static void world_ls(struct anx_world_graph *g)
{
	uint32_t i, nn = anx_world_graph_node_count(g);
	uint32_t ne = anx_world_graph_edge_count(g);

	kprintf("nodes (%u):\n", nn);
	for (i = 0; i < nn; i++) {
		struct anx_world_node_info info;
		char oid[12];

		if (anx_world_graph_get_node(g, i, &info) != ANX_OK)
			continue;
		short_oid(&info.id, oid, sizeof(oid));
		kprintf("  [%u] %s  %s/%s%s\n", i, oid, info.domain,
			info.label, info.conflict ? "  (conflict)" : "");
	}

	kprintf("edges (%u):\n", ne);
	for (i = 0; i < ne; i++) {
		struct anx_world_edge_info e;
		int fi, ti;

		if (anx_world_graph_get_edge(g, i, &e) != ANX_OK)
			continue;
		fi = index_of_oid(g, &e.from);
		ti = index_of_oid(g, &e.to);
		kprintf("  [%u] %d --%s--> %d\n", i, fi, e.relation, ti);
	}
}

static void world_show(struct anx_world_graph *g, const char *arg)
{
	struct anx_world_node_info info;
	uint32_t idx, k;
	char oid[40];

	if (!arg || !parse_u32(arg, &idx)) {
		kprintf("usage: world show <node-index>\n");
		return;
	}
	if (anx_world_graph_get_node(g, idx, &info) != ANX_OK) {
		kprintf("world: no node at index %u\n", idx);
		return;
	}
	anx_uuid_to_string(&info.id, oid, sizeof(oid));
	kprintf("node [%u]\n", idx);
	kprintf("  oid:      %s\n", oid);
	kprintf("  domain:   %s\n", info.domain);
	kprintf("  label:    %s\n", info.label);
	kprintf("  conflict: %s\n", info.conflict ? "yes" : "no");
	kprintf("  props:    %u\n", info.prop_count);
	for (k = 0; k < info.prop_count; k++) {
		char key[32], val[64];

		if (anx_world_graph_get_prop(g, idx, k, key, sizeof(key),
					     val, sizeof(val)) == ANX_OK)
			kprintf("    %s = %s\n", key, val);
	}
}

static int provider_print_cb(const struct anx_world_manifest *m,
			     bool has_validator, void *arg)
{
	(void)arg;
	kprintf("  %s  domain=%s  writes=%s%s%s\n", m->id, m->domain,
		m->writes[0] ? m->writes : "-",
		has_validator ? "  [validator]" : "",
		m->can_execute_actions ? "  [actions]" : "");
	return 0;
}

static void world_providers(void)
{
	kprintf("providers (%u):\n", anx_world_provider_count());
	anx_world_provider_iterate(provider_print_cb, NULL);
}

static void world_add(struct anx_world_graph *g, const char *domain,
		    const char *label)
{
	struct anx_world_branch *b;
	struct anx_world_patch *p;
	struct anx_world_commit_report rep;
	struct anx_world_ref ref;

	if (!domain || !label) {
		kprintf("usage: world add <domain> <label>\n");
		return;
	}
	ensure_operator();
	b = anx_world_branch_fork(g);
	p = anx_world_patch_create("shell.operator");
	if (!b || !p) {
		kprintf("world: out of memory\n");
		goto out;
	}
	if (anx_world_patch_add_node(p, domain, label, &ref) != ANX_OK ||
	    anx_world_branch_propose(b, p) != ANX_OK) {
		kprintf("world: could not stage node\n");
		goto out;
	}
	anx_world_branch_commit(b, &rep);
	print_report(&rep);
out:
	anx_world_branch_abandon(b);
	anx_world_patch_destroy(p);
}

static void world_link(struct anx_world_graph *g, const char *from_s,
		     const char *to_s, const char *relation)
{
	struct anx_world_branch *b;
	struct anx_world_patch *p;
	struct anx_world_commit_report rep;
	struct anx_world_node_info fi, ti;
	uint32_t from, to;

	if (!from_s || !to_s || !relation ||
	    !parse_u32(from_s, &from) || !parse_u32(to_s, &to)) {
		kprintf("usage: world link <from-index> <to-index> <relation>\n");
		return;
	}
	if (anx_world_graph_get_node(g, from, &fi) != ANX_OK ||
	    anx_world_graph_get_node(g, to, &ti) != ANX_OK) {
		kprintf("world: bad node index\n");
		return;
	}
	ensure_operator();
	b = anx_world_branch_fork(g);
	p = anx_world_patch_create("shell.operator");
	if (!b || !p) {
		kprintf("world: out of memory\n");
		goto out;
	}
	if (anx_world_patch_add_edge(p, anx_world_ref_oid(fi.id),
				     anx_world_ref_oid(ti.id), relation)
	    != ANX_OK ||
	    anx_world_branch_propose(b, p) != ANX_OK) {
		kprintf("world: could not stage edge\n");
		goto out;
	}
	anx_world_branch_commit(b, &rep);
	print_report(&rep);
out:
	anx_world_branch_abandon(b);
	anx_world_patch_destroy(p);
}

/* One-shot patch wrapping a single op on an existing node, by index. */
static void commit_one(struct anx_world_graph *g, struct anx_world_patch *p)
{
	struct anx_world_branch *b = anx_world_branch_fork(g);
	struct anx_world_commit_report rep;

	if (!b) {
		kprintf("world: out of memory\n");
		return;
	}
	if (anx_world_branch_propose(b, p) != ANX_OK)
		kprintf("world: could not stage op\n");
	else {
		anx_world_branch_commit(b, &rep);
		print_report(&rep);
	}
	anx_world_branch_abandon(b);
}

static void world_set(struct anx_world_graph *g, const char *idx_s,
		      const char *key, const char *val)
{
	struct anx_world_node_info info;
	struct anx_world_patch *p;
	uint32_t idx;

	if (!idx_s || !key || !val || !parse_u32(idx_s, &idx)) {
		kprintf("usage: world set <node-index> <key> <value>\n");
		return;
	}
	if (anx_world_graph_get_node(g, idx, &info) != ANX_OK) {
		kprintf("world: no node at index %u\n", idx);
		return;
	}
	ensure_operator();
	p = anx_world_patch_create("shell.operator");
	if (!p) {
		kprintf("world: out of memory\n");
		return;
	}
	if (anx_world_patch_update_property(p, anx_world_ref_oid(info.id),
					    key, val) == ANX_OK)
		commit_one(g, p);
	anx_world_patch_destroy(p);
}

static void world_constrain(struct anx_world_graph *g, const char *idx_s,
			    const char *expr)
{
	struct anx_world_node_info info;
	struct anx_world_patch *p;
	uint32_t idx;

	if (!idx_s || !expr || !parse_u32(idx_s, &idx)) {
		kprintf("usage: world constrain <node-index> <expr>\n");
		kprintf("  e.g. world constrain 2 mass>0\n");
		return;
	}
	if (anx_world_graph_get_node(g, idx, &info) != ANX_OK) {
		kprintf("world: no node at index %u\n", idx);
		return;
	}
	ensure_operator();
	p = anx_world_patch_create("shell.operator");
	if (!p) {
		kprintf("world: out of memory\n");
		return;
	}
	/* The constraint validator (if registered) evaluates this at commit. */
	if (anx_world_patch_attach_constraint(p, anx_world_ref_oid(info.id),
					      expr) == ANX_OK)
		commit_one(g, p);
	anx_world_patch_destroy(p);
}

static void world_demo(struct anx_world_graph *g)
{
	struct anx_world_branch *b;
	struct anx_world_patch *p;
	struct anx_world_commit_report rep;
	struct anx_world_ref ball, floor;

	ensure_operator();
	b = anx_world_branch_fork(g);
	p = anx_world_patch_create("shell.operator");
	if (!b || !p) {
		kprintf("world: out of memory\n");
		goto out;
	}
	kprintf("demo: fork @ v%u, propose patch...\n",
		(unsigned)anx_world_graph_version(g));
	anx_world_patch_add_node(p, "world.physical", "ball", &ball);
	anx_world_patch_add_node(p, "world.spatial", "floor", &floor);
	anx_world_patch_add_edge(p, ball, floor, "rests_on");
	anx_world_patch_update_property(p, ball, "mass", "1.0");
	anx_world_patch_attach_constraint(p, ball, "mass>0");
	anx_world_patch_attach_prediction(p, ball, "falls", 900);
	if (anx_world_branch_propose(b, p) != ANX_OK) {
		kprintf("demo: propose failed\n");
		goto out;
	}
	kprintf("demo: branch has %u nodes, %u edges; commit...\n",
		anx_world_branch_node_count(b),
		anx_world_branch_edge_count(b));
	anx_world_branch_commit(b, &rep);
	print_report(&rep);
	kprintf("demo: canonical now v%u, %u nodes\n",
		(unsigned)anx_world_graph_version(g),
		anx_world_graph_node_count(g));
out:
	anx_world_branch_abandon(b);
	anx_world_patch_destroy(p);
}

static void usage(void)
{
	kputc('\n');
	kprintf("usage: world <command>\n");
	kprintf("  status                       graph + provider counts\n");
	kprintf("  ls                           list nodes and edges\n");
	kprintf("  show <idx>                   detail one node\n");
	kprintf("  providers                    list registered providers\n");
	kprintf("  add <domain> <label>         commit a new node\n");
	kprintf("  link <from> <to> <relation>  commit an edge\n");
	kprintf("  set <idx> <key> <value>      set a node property\n");
	kprintf("  constrain <idx> <expr>       attach a constraint (gate-checked)\n");
	kprintf("  demo                         run the full commit pipeline\n");
}

void cmd_world(int argc, char **argv)
{
	struct anx_world_graph *g = anx_world_default_graph();

	if (!g) {
		kprintf("world: runtime unavailable\n");
		return;
	}
	if (argc < 2 || anx_strcmp(argv[1], "help") == 0) {
		usage();
		return;
	}

	if (anx_strcmp(argv[1], "status") == 0)
		world_status(g);
	else if (anx_strcmp(argv[1], "ls") == 0)
		world_ls(g);
	else if (anx_strcmp(argv[1], "show") == 0)
		world_show(g, argc >= 3 ? argv[2] : NULL);
	else if (anx_strcmp(argv[1], "providers") == 0)
		world_providers();
	else if (anx_strcmp(argv[1], "add") == 0)
		world_add(g, argc >= 3 ? argv[2] : NULL,
			argc >= 4 ? argv[3] : NULL);
	else if (anx_strcmp(argv[1], "link") == 0)
		world_link(g, argc >= 3 ? argv[2] : NULL,
			 argc >= 4 ? argv[3] : NULL,
			 argc >= 5 ? argv[4] : NULL);
	else if (anx_strcmp(argv[1], "set") == 0)
		world_set(g, argc >= 3 ? argv[2] : NULL,
			  argc >= 4 ? argv[3] : NULL,
			  argc >= 5 ? argv[4] : NULL);
	else if (anx_strcmp(argv[1], "constrain") == 0)
		world_constrain(g, argc >= 3 ? argv[2] : NULL,
				argc >= 4 ? argv[3] : NULL);
	else if (anx_strcmp(argv[1], "demo") == 0)
		world_demo(g);
	else
		kprintf("world: unknown subcommand '%s' (try 'world help')\n",
			argv[1]);
}
