/*
 * federation.c — federated world graph sync (RFC-0026 Section 7).
 *
 * Local authority by default: a node's own writes are authoritative and commit
 * immediately. A claim from a peer travels as a signed envelope and is purely
 * advisory — on arrival it must pass signature verification, a trust-zone check
 * against the Network Plane, and the local commit gate (provider authority plus
 * validators) before it changes anything. A remote peer cannot bypass any rule
 * a local model is held to.
 *
 * The envelope carries the patch in the runtime's in-memory op form; a
 * byte-level wire encoding for a real socket belongs to RFC-0006 / RFC-0015.
 * This module is the policy and trust layer that decides what may cross.
 */

#include <anx/worldgraph.h>
#include <anx/netplane.h>
#include <anx/crypto.h>
#include <anx/string.h>

/* --- Peer keyring --- */

struct peer_key {
	bool used;
	anx_nid_t nid;
	uint8_t pubkey[32];
};

static struct peer_key peers[ANX_WORLD_MAX_PEERS];

static struct peer_key *find_peer(const anx_nid_t *nid)
{
	uint32_t i;

	for (i = 0; i < ANX_WORLD_MAX_PEERS; i++)
		if (peers[i].used &&
		    anx_uuid_compare(&peers[i].nid, nid) == 0)
			return &peers[i];
	return NULL;
}

int anx_world_peer_set_key(const anx_nid_t *nid, const uint8_t pubkey[32])
{
	struct peer_key *slot;
	uint32_t i;

	if (!nid || !pubkey)
		return ANX_EINVAL;

	slot = find_peer(nid);
	if (!slot) {
		for (i = 0; i < ANX_WORLD_MAX_PEERS; i++) {
			if (!peers[i].used) {
				slot = &peers[i];
				break;
			}
		}
	}
	if (!slot)
		return ANX_EFULL;

	slot->used = true;
	slot->nid = *nid;
	anx_memcpy(slot->pubkey, pubkey, 32);
	return ANX_OK;
}

int anx_world_peer_get_key(const anx_nid_t *nid, uint8_t out[32])
{
	struct peer_key *slot;

	if (!nid || !out)
		return ANX_EINVAL;
	slot = find_peer(nid);
	if (!slot)
		return ANX_ENOENT;
	anx_memcpy(out, slot->pubkey, 32);
	return ANX_OK;
}

uint32_t anx_world_peer_count(void)
{
	uint32_t i, n = 0;

	for (i = 0; i < ANX_WORLD_MAX_PEERS; i++)
		if (peers[i].used)
			n++;
	return n;
}

/* --- Envelopes --- */

/* The signed region is every byte of the envelope up to the signature. The
 * struct is zeroed before filling so padding is deterministic. */
static uint32_t signed_len(void)
{
	return (uint32_t)__builtin_offsetof(struct anx_world_envelope, sig);
}

int anx_world_envelope_seal(const struct anx_world_patch *p,
			    const uint8_t priv[64], const anx_nid_t *signer,
			    struct anx_world_envelope *out)
{
	uint32_t i, n;

	if (!p || !priv || !signer || !out)
		return ANX_EINVAL;

	anx_memset(out, 0, sizeof(*out));
	n = anx_world_patch_op_count(p);
	if (n > ANX_WORLD_PATCH_MAX_OPS)
		return ANX_EINVAL;
	for (i = 0; i < n; i++) {
		if (anx_world_patch_get_op(p, i, &out->ops[i]) != ANX_OK)
			return ANX_EINVAL;
	}
	out->op_count = n;
	out->signer = *signer;
	/* The provider id travels with the envelope so the receiver's gate can
	 * check the remote author's write authority. */
	anx_world_patch_provider_id(p, out->provider_id,
				    sizeof(out->provider_id));

	anx_ed25519_sign(out->sig, out, signed_len(), priv);
	return ANX_OK;
}

int anx_world_envelope_verify(const struct anx_world_envelope *e,
			      const uint8_t pubkey[32])
{
	if (!e || !pubkey)
		return ANX_EINVAL;
	if (anx_ed25519_verify(e->sig, e, signed_len(), pubkey) != 0)
		return ANX_EPERM;
	return ANX_OK;
}

/* Rebuild a patch from an envelope's ops, replaying through the public builder
 * so refs and authorship match the original. Returns a new patch or NULL. */
static struct anx_world_patch *rebuild_patch(const struct anx_world_envelope *e)
{
	struct anx_world_patch *p = anx_world_patch_create(e->provider_id);
	uint32_t i;

	if (!p)
		return NULL;

	for (i = 0; i < e->op_count; i++) {
		const struct anx_world_op_info *op = &e->ops[i];
		struct anx_world_ref ref;
		int rc = ANX_OK;

		switch (op->type) {
		case ANX_WOP_ADD_NODE:
			rc = anx_world_patch_add_node(p, op->s1, op->s2, &ref);
			break;
		case ANX_WOP_ADD_EDGE:
			rc = anx_world_patch_add_edge(p, op->a, op->b, op->s1);
			break;
		case ANX_WOP_UPDATE_PROPERTY:
			rc = anx_world_patch_update_property(p, op->a, op->s1,
							     op->s2);
			break;
		case ANX_WOP_ATTACH_CONSTRAINT:
			rc = anx_world_patch_attach_constraint(p, op->a,
							       op->s2);
			break;
		case ANX_WOP_ATTACH_PREDICTION:
			rc = anx_world_patch_attach_prediction(p, op->a, op->s2,
							       op->conf);
			break;
		case ANX_WOP_MARK_CONFLICT:
			rc = anx_world_patch_mark_conflict(p, op->a, op->s2);
			break;
		case ANX_WOP_RESOLVE_CONFLICT:
			rc = anx_world_patch_resolve_conflict(p, op->a);
			break;
		default:
			rc = ANX_EINVAL;
			break;
		}
		if (rc != ANX_OK) {
			anx_world_patch_destroy(p);
			return NULL;
		}
	}
	return p;
}

static int fail(struct anx_world_commit_report *report, int code,
		const char *why)
{
	if (report) {
		anx_memset(report, 0, sizeof(*report));
		report->patch_oid = ANX_UUID_NIL;
		report->reason_code = code;
		anx_strlcpy(report->reason, why, sizeof(report->reason));
	}
	return code;
}

int anx_world_federation_apply(struct anx_world_graph *g,
			       const struct anx_world_envelope *e,
			       struct anx_world_commit_report *report)
{
	struct anx_net_node *node;
	struct anx_world_branch *b;
	struct anx_world_patch *p;
	uint8_t pubkey[32];
	int rc;

	if (!g || !e)
		return fail(report, ANX_EINVAL, "bad arguments");

	/* 1. Known signer. */
	if (anx_world_peer_get_key(&e->signer, pubkey) != ANX_OK)
		return fail(report, ANX_ENOENT, "unknown peer key");

	/* 2. Authentic signature. */
	if (anx_world_envelope_verify(e, pubkey) != ANX_OK)
		return fail(report, ANX_EPERM, "signature mismatch");

	/* 3. Trusted peer (Network Plane trust zone). */
	node = anx_netplane_lookup(&e->signer);
	if (!node)
		return fail(report, ANX_EPERM, "peer not in node registry");
	if (node->trust_zone == ANX_TRUST_UNTRUSTED)
		return fail(report, ANX_EPERM, "peer is untrusted");

	/* 4. Local commit gate — remote claims earn no exemption. */
	p = rebuild_patch(e);
	if (!p)
		return fail(report, ANX_EINVAL, "malformed patch");
	b = anx_world_branch_fork(g);
	if (!b) {
		anx_world_patch_destroy(p);
		return fail(report, ANX_ENOMEM, "out of memory");
	}
	rc = anx_world_branch_propose(b, p);
	if (rc == ANX_OK)
		rc = anx_world_branch_commit(b, report);
	else if (report)
		fail(report, rc, "propose failed");

	anx_world_branch_abandon(b);
	anx_world_patch_destroy(p);
	return rc;
}
