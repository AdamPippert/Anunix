/*
 * netplane.c — Network Plane stub implementation.
 *
 * Local-only node registry. Registers the local node at init
 * and supports peer registration for future network extension.
 */

#include <anx/types.h>
#include <anx/netplane.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/hashtable.h>
#include <anx/string.h>

#define NETPLANE_BITS	4	/* 16 buckets (few nodes expected) */

static struct anx_htable node_table;
static struct anx_net_node *local_node;

void anx_netplane_init(void)
{
	anx_htable_init(&node_table, NETPLANE_BITS);

	/* Register the local node */
	local_node = anx_zalloc(sizeof(*local_node));
	if (!local_node)
		return;

	anx_uuid_generate(&local_node->nid);
	anx_strlcpy(local_node->name, "local", sizeof(local_node->name));
	local_node->node_type = ANX_NODE_PERSONAL;
	local_node->trust_zone = ANX_TRUST_LOCAL;
	local_node->status = ANX_NODE_ONLINE;

	anx_spin_init(&local_node->lock);
	anx_list_init(&local_node->registry_link);

	uint64_t hash = anx_uuid_hash(&local_node->nid);
	anx_htable_add(&node_table, &local_node->registry_link, hash);
}

struct anx_net_node *anx_netplane_local_node(void)
{
	return local_node;
}

int anx_netplane_register_peer(const char *name,
			       enum anx_node_type type,
			       enum anx_trust_zone trust,
			       struct anx_net_node **out)
{
	struct anx_net_node *node;

	if (!name)
		return ANX_EINVAL;

	node = anx_zalloc(sizeof(*node));
	if (!node)
		return ANX_ENOMEM;

	anx_uuid_generate(&node->nid);
	anx_strlcpy(node->name, name, sizeof(node->name));
	node->node_type = type;
	node->trust_zone = trust;
	node->status = ANX_NODE_ONLINE;

	anx_spin_init(&node->lock);
	anx_list_init(&node->registry_link);

	uint64_t hash = anx_uuid_hash(&node->nid);
	anx_htable_add(&node_table, &node->registry_link, hash);

	if (out)
		*out = node;
	return ANX_OK;
}

struct anx_net_node *anx_netplane_lookup(const anx_nid_t *nid)
{
	uint64_t hash = anx_uuid_hash(nid);
	struct anx_list_head *pos;

	ANX_HTABLE_FOR_BUCKET(pos, &node_table, hash) {
		struct anx_net_node *node;

		node = ANX_LIST_ENTRY(pos, struct anx_net_node, registry_link);
		if (anx_uuid_compare(&node->nid, nid) == 0)
			return node;
	}
	return NULL;
}

int anx_netplane_set_status(struct anx_net_node *node,
			    enum anx_node_status status)
{
	if (!node)
		return ANX_EINVAL;

	anx_spin_lock(&node->lock);
	node->status = status;
	anx_spin_unlock(&node->lock);

	return ANX_OK;
}
