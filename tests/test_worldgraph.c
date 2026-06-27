/*
 * test_worldgraph.c — Clustered World Graph Runtime (RFC-0026).
 *
 * Exercises the full pipeline: fork a speculative branch, propose a model patch
 * of claims, clear the commit gate (provider authority + validators), merge,
 * and replicate a committed object to a trusted peer — verifying provider
 * authority, validator veto, optimistic-concurrency rejection, materialization
 * into State Objects, and the trust-zoned replication gate.
 */

#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/worldgraph.h>
#include <anx/netplane.h>
#include <anx/uuid.h>
#include <anx/string.h>

/* Count objects of a given type in the store. */
struct typecount {
	enum anx_object_type type;
	int n;
};

static int count_type_cb(struct anx_state_object *obj, void *arg)
{
	struct typecount *c = arg;

	if (obj->object_type == c->type)
		c->n++;
	return 0;
}

static int count_objects(enum anx_object_type type)
{
	struct typecount c = { .type = type, .n = 0 };

	anx_objstore_iterate(count_type_cb, &c);
	return c.n;
}

/* Count providers during iteration. */
static int count_provider_cb(const struct anx_world_manifest *m,
			     bool has_validator, void *arg)
{
	(void)m;
	(void)has_validator;
	(*(int *)arg)++;
	return 0;
}

/* A validator that always rejects, to prove the commit gate vetoes. */
static int veto_validator(const struct anx_world_patch *p,
			  const struct anx_world_branch *b,
			  char *reason, size_t len, void *ctx)
{
	(void)p;
	(void)b;
	(void)ctx;
	anx_strlcpy(reason, "always veto", len);
	return ANX_EINVAL;
}

/* Register the physics provider with write authority over physical/spatial. */
static int register_physics(anx_world_validate_fn validate)
{
	struct anx_world_manifest m;

	anx_memset(&m, 0, sizeof(m));
	anx_strlcpy(m.id, "physics.lnn", sizeof(m.id));
	anx_strlcpy(m.domain, "physics", sizeof(m.domain));
	anx_strlcpy(m.reads, "world.physical,world.spatial", sizeof(m.reads));
	anx_strlcpy(m.writes, "world.physical,world.spatial", sizeof(m.writes));
	anx_strlcpy(m.network_zone, "physics.zone", sizeof(m.network_zone));
	m.can_execute_actions = false;
	return anx_world_provider_register(&m, validate, NULL);
}

/* Build a patch that adds two nodes, links them, sets a property, and attaches
 * a constraint and a prediction. */
static struct anx_world_patch *build_patch(void)
{
	struct anx_world_patch *p = anx_world_patch_create("physics.lnn");
	struct anx_world_ref a, b;

	if (!p)
		return NULL;
	if (anx_world_patch_add_node(p, "world.physical", "ball", &a) != ANX_OK)
		return NULL;
	if (anx_world_patch_add_node(p, "world.spatial", "floor", &b) != ANX_OK)
		return NULL;
	if (anx_world_patch_add_edge(p, a, b, "rests_on") != ANX_OK)
		return NULL;
	if (anx_world_patch_update_property(p, a, "mass", "1.0") != ANX_OK)
		return NULL;
	if (anx_world_patch_attach_constraint(p, a, "mass>0") != ANX_OK)
		return NULL;
	if (anx_world_patch_attach_prediction(p, a, "falls", 900) != ANX_OK)
		return NULL;
	return p;
}

int test_worldgraph(void)
{
	struct anx_world_graph *g;
	struct anx_world_branch *br;
	struct anx_world_patch *p;
	struct anx_world_commit_report rep;
	const struct anx_world_manifest *man;

	anx_objstore_init();
	anx_world_runtime_init();
	anx_netplane_init();

	/* --- Graph and branch lifecycle --- */
	g = anx_world_graph_create("physics-world");
	if (!g)
		return -1;
	if (anx_world_graph_version(g) != 1)
		return -2;
	if (anx_world_graph_node_count(g) != 0)
		return -3;

	br = anx_world_branch_fork(g);
	if (!br)
		return -4;

	/* --- Propose a patch onto the branch --- */
	p = build_patch();
	if (!p)
		return -5;
	if (anx_world_patch_op_count(p) != 6)
		return -6;
	if (anx_world_branch_propose(br, p) != ANX_OK)
		return -7;
	/* Branch reflects the patch; canonical does not yet. */
	if (anx_world_branch_node_count(br) != 2)
		return -8;
	if (anx_world_branch_edge_count(br) != 1)
		return -9;
	if (anx_world_graph_node_count(g) != 0)
		return -10;

	/* --- Commit gate: provider not yet registered → rejected --- */
	if (anx_world_branch_commit(br, &rep) != ANX_EPERM)
		return -11;
	if (rep.accepted)
		return -12;
	/* Branch survives a rejection for revision. */
	if (anx_world_branch_node_count(br) != 2)
		return -13;

	/* --- Register the provider; commit gate now passes --- */
	if (register_physics(NULL) != ANX_OK)
		return -14;
	if (anx_world_provider_count() != 1)
		return -15;
	man = anx_world_provider_get("physics.lnn");
	if (!man || anx_strcmp(man->domain, "physics") != 0)
		return -16;

	if (anx_world_branch_commit(br, &rep) != ANX_OK)
		return -17;
	if (!rep.accepted || rep.reason_code != ANX_OK)
		return -18;
	if (rep.nodes_committed != 2 || rep.edges_committed != 1)
		return -19;
	if (anx_uuid_is_nil(&rep.patch_oid))
		return -20;

	/* Canonical graph advanced. */
	if (anx_world_graph_version(g) != 2)
		return -21;
	if (anx_world_graph_node_count(g) != 2)
		return -22;
	if (anx_world_graph_edge_count(g) != 1)
		return -23;

	/* Materialization: nodes, edges, patch, constraint, prediction objects. */
	if (count_objects(ANX_OBJ_WORLD_NODE) != 2)
		return -24;
	if (count_objects(ANX_OBJ_WORLD_EDGE) != 1)
		return -25;
	if (count_objects(ANX_OBJ_WORLD_PATCH) != 1)
		return -26;
	if (count_objects(ANX_OBJ_CONSTRAINT) != 1)
		return -27;
	if (count_objects(ANX_OBJ_PREDICTION) != 1)
		return -28;

	anx_world_branch_abandon(br);
	anx_world_patch_destroy(p);

	/* --- Provider write-authority enforcement --- */
	{
		struct anx_world_branch *b2 = anx_world_branch_fork(g);
		struct anx_world_patch *bad = anx_world_patch_create("physics.lnn");
		struct anx_world_ref r;

		if (!b2 || !bad)
			return -29;
		/* "world.chemical" is outside the manifest's writes. */
		if (anx_world_patch_add_node(bad, "world.chemical", "atom", &r)
		    != ANX_OK)
			return -30;
		if (anx_world_branch_propose(b2, bad) != ANX_OK)
			return -31;
		if (anx_world_branch_commit(b2, &rep) != ANX_EPERM)
			return -32;
		if (rep.accepted)
			return -33;
		if (anx_world_graph_version(g) != 2)	/* unchanged */
			return -34;
		anx_world_branch_abandon(b2);
		anx_world_patch_destroy(bad);
	}

	/* --- Validator veto --- */
	{
		struct anx_world_manifest vm;
		struct anx_world_branch *b3;
		struct anx_world_patch *p3;
		struct anx_world_ref r;

		anx_memset(&vm, 0, sizeof(vm));
		anx_strlcpy(vm.id, "gatekeeper", sizeof(vm.id));
		anx_strlcpy(vm.domain, "validation", sizeof(vm.domain));
		if (anx_world_provider_register(&vm, veto_validator, NULL)
		    != ANX_OK)
			return -35;

		b3 = anx_world_branch_fork(g);
		p3 = anx_world_patch_create("physics.lnn");
		if (!b3 || !p3)
			return -36;
		if (anx_world_patch_add_node(p3, "world.physical", "cube", &r)
		    != ANX_OK)
			return -37;
		if (anx_world_branch_propose(b3, p3) != ANX_OK)
			return -38;
		if (anx_world_branch_commit(b3, &rep) != ANX_EINVAL)
			return -39;
		if (rep.accepted)
			return -40;
		if (anx_world_graph_version(g) != 2)	/* veto blocked merge */
			return -41;

		anx_world_branch_abandon(b3);
		anx_world_patch_destroy(p3);
		if (anx_world_provider_unregister("gatekeeper") != ANX_OK)
			return -42;
	}

	/* --- Optimistic concurrency: stale branch is refused --- */
	{
		struct anx_world_branch *x = anx_world_branch_fork(g);
		struct anx_world_branch *y = anx_world_branch_fork(g);
		struct anx_world_patch *px = anx_world_patch_create("physics.lnn");
		struct anx_world_patch *py = anx_world_patch_create("physics.lnn");
		struct anx_world_ref rx, ry;

		if (!x || !y || !px || !py)
			return -43;
		if (anx_world_patch_add_node(px, "world.physical", "x", &rx)
		    != ANX_OK)
			return -44;
		if (anx_world_patch_add_node(py, "world.physical", "y", &ry)
		    != ANX_OK)
			return -45;
		if (anx_world_branch_propose(x, px) != ANX_OK)
			return -46;
		if (anx_world_branch_propose(y, py) != ANX_OK)
			return -47;

		/* x commits first and wins. */
		if (anx_world_branch_commit(x, &rep) != ANX_OK)
			return -48;
		if (anx_world_graph_version(g) != 3)
			return -49;
		/* y forked at version 2 → now stale → refused. */
		if (anx_world_branch_commit(y, &rep) != ANX_EBUSY)
			return -50;
		if (anx_world_graph_version(g) != 3)
			return -51;

		anx_world_branch_abandon(x);
		anx_world_branch_abandon(y);
		anx_world_patch_destroy(px);
		anx_world_patch_destroy(py);
	}

	/* --- Trust-zoned replication gate --- */
	{
		struct anx_net_node *trusted = NULL, *untrusted = NULL;
		anx_oid_t obj_oid;

		if (anx_netplane_register_peer("peer-trusted", ANX_NODE_TEAM_SERVER,
					       ANX_TRUST_LAN, &trusted) != ANX_OK)
			return -52;
		if (anx_netplane_register_peer("peer-untrusted", ANX_NODE_CLOUD,
					       ANX_TRUST_UNTRUSTED, &untrusted)
		    != ANX_OK)
			return -53;
		if (!trusted || !untrusted)
			return -54;

		/* Commit a fresh patch to obtain a known committed object oid. */
		{
			struct anx_world_branch *cb = anx_world_branch_fork(g);
			struct anx_world_patch *cp =
				anx_world_patch_create("physics.lnn");
			struct anx_world_ref cr;

			if (!cb || !cp)
				return -55;
			if (anx_world_patch_add_node(cp, "world.physical",
						     "repl", &cr) != ANX_OK)
				return -56;
			if (anx_world_branch_propose(cb, cp) != ANX_OK)
				return -57;
			if (anx_world_branch_commit(cb, &rep) != ANX_OK)
				return -58;
			obj_oid = rep.patch_oid;	/* a real committed object */
			anx_world_branch_abandon(cb);
			anx_world_patch_destroy(cp);
		}
		if (anx_uuid_is_nil(&obj_oid))
			return -59;

		/* Trusted peer: allowed. Untrusted: refused. Unknown: ENOENT. */
		if (anx_world_replicate(&obj_oid, &trusted->nid) != ANX_OK)
			return -60;
		if (anx_world_replicate(&obj_oid, &untrusted->nid) != ANX_EPERM)
			return -61;
		{
			anx_nid_t bogus;

			anx_uuid_generate(&bogus);
			if (anx_world_replicate(&obj_oid, &bogus) != ANX_ENOENT)
				return -62;
		}
	}

	/* --- Read accessors and provider iteration (RFC-0026 tooling) --- */
	{
		struct anx_world_node_info ninfo;
		struct anx_world_edge_info einfo;
		int seen_providers = 0;
		uint32_t nn = anx_world_graph_node_count(g);

		/* Node 0 is the first committed node ("ball"). */
		if (nn < 1)
			return -63;
		if (anx_world_graph_get_node(g, 0, &ninfo) != ANX_OK)
			return -64;
		if (anx_strcmp(ninfo.domain, "world.physical") != 0)
			return -65;
		if (anx_strcmp(ninfo.label, "ball") != 0)
			return -66;
		if (anx_uuid_is_nil(&ninfo.id))	/* identity == committed oid */
			return -67;
		/* Out-of-range node is ENOENT, not a crash. */
		if (anx_world_graph_get_node(g, nn + 100, &ninfo) != ANX_ENOENT)
			return -68;

		/* Edge 0 endpoints are real committed oids. */
		if (anx_world_graph_edge_count(g) >= 1) {
			if (anx_world_graph_get_edge(g, 0, &einfo) != ANX_OK)
				return -69;
			if (anx_uuid_is_nil(&einfo.from) ||
			    anx_uuid_is_nil(&einfo.to))
				return -70;
		}

		/* Provider iteration visits the registered physics provider. */
		{
			int n = anx_world_provider_iterate(
				count_provider_cb, &seen_providers);
			if (n != ANX_OK)
				return -71;
		}
		if (seen_providers != (int)anx_world_provider_count())
			return -72;
	}

	/* --- Wildcard write authority ("*") --- */
	{
		struct anx_world_manifest op;
		struct anx_world_branch *wb;
		struct anx_world_patch *wp;
		struct anx_world_ref wr;

		anx_memset(&op, 0, sizeof(op));
		anx_strlcpy(op.id, "op", sizeof(op.id));
		anx_strlcpy(op.domain, "shell", sizeof(op.domain));
		anx_strlcpy(op.writes, "*", sizeof(op.writes));
		if (anx_world_provider_register(&op, NULL, NULL) != ANX_OK)
			return -73;

		wb = anx_world_branch_fork(g);
		wp = anx_world_patch_create("op");
		if (!wb || !wp)
			return -74;
		/* A slice no narrow provider could write — wildcard allows it. */
		if (anx_world_patch_add_node(wp, "world.anything", "x", &wr)
		    != ANX_OK)
			return -75;
		if (anx_world_branch_propose(wb, wp) != ANX_OK)
			return -76;
		if (anx_world_branch_commit(wb, &rep) != ANX_OK)
			return -77;
		if (!rep.accepted)
			return -78;
		anx_world_branch_abandon(wb);
		anx_world_patch_destroy(wp);
	}

	/* --- Default graph singleton --- */
	{
		struct anx_world_graph *d1 = anx_world_default_graph();
		struct anx_world_graph *d2 = anx_world_default_graph();

		if (!d1 || d1 != d2)	/* same instance every call */
			return -79;
		if (d1 == g)		/* distinct from our local graph */
			return -80;
	}

	/* --- Constraint validator (RFC-0026 §7 concrete provider) --- */
	{
		struct anx_world_branch *cb;
		struct anx_world_patch *cp;
		struct anx_world_ref node;
		uint64_t v_before;

		if (anx_world_constraint_validator_register() != ANX_OK)
			return -81;

		/* Satisfied: mass=2.0 with mass>0 commits. */
		cb = anx_world_branch_fork(g);
		cp = anx_world_patch_create("op");	/* wildcard provider */
		if (!cb || !cp)
			return -82;
		anx_world_patch_add_node(cp, "world.physical", "rock", &node);
		anx_world_patch_update_property(cp, node, "mass", "2.0");
		anx_world_patch_attach_constraint(cp, node, "mass>0");
		if (anx_world_branch_propose(cb, cp) != ANX_OK)
			return -83;
		if (anx_world_branch_commit(cb, &rep) != ANX_OK)
			return -84;
		if (!rep.accepted)
			return -85;
		anx_world_branch_abandon(cb);
		anx_world_patch_destroy(cp);

		/* Violated: mass=-5 with mass>0 is rejected, graph unchanged. */
		v_before = anx_world_graph_version(g);
		cb = anx_world_branch_fork(g);
		cp = anx_world_patch_create("op");
		if (!cb || !cp)
			return -86;
		anx_world_patch_add_node(cp, "world.physical", "ghost", &node);
		anx_world_patch_update_property(cp, node, "mass", "-5");
		anx_world_patch_attach_constraint(cp, node, "mass>0");
		if (anx_world_branch_propose(cb, cp) != ANX_OK)
			return -87;
		if (anx_world_branch_commit(cb, &rep) != ANX_EINVAL)
			return -88;
		if (rep.accepted)
			return -89;
		if (anx_world_graph_version(g) != v_before)
			return -90;
		anx_world_branch_abandon(cb);
		anx_world_patch_destroy(cp);

		/* Missing property: constraint on an absent key is rejected. */
		cb = anx_world_branch_fork(g);
		cp = anx_world_patch_create("op");
		if (!cb || !cp)
			return -91;
		anx_world_patch_add_node(cp, "world.physical", "void", &node);
		anx_world_patch_attach_constraint(cp, node, "mass>0");
		if (anx_world_branch_propose(cb, cp) != ANX_OK)
			return -92;
		if (anx_world_branch_commit(cb, &rep) != ANX_EINVAL)
			return -93;
		anx_world_branch_abandon(cb);
		anx_world_patch_destroy(cp);

		/* Decimal comparison: temp<=37.5 satisfied by 37.0. */
		cb = anx_world_branch_fork(g);
		cp = anx_world_patch_create("op");
		if (!cb || !cp)
			return -94;
		anx_world_patch_add_node(cp, "world.physical", "body", &node);
		anx_world_patch_update_property(cp, node, "temp", "37.0");
		anx_world_patch_attach_constraint(cp, node, "temp<=37.5");
		if (anx_world_branch_propose(cb, cp) != ANX_OK)
			return -95;
		if (anx_world_branch_commit(cb, &rep) != ANX_OK)
			return -96;
		if (!rep.accepted)
			return -97;
		anx_world_branch_abandon(cb);
		anx_world_patch_destroy(cp);
	}

	anx_world_graph_destroy(g);
	return 0;
}
