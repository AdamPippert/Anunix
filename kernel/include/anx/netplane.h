/*
 * anx/netplane.h — Network Plane (RFC-0006).
 *
 * The Network Plane extends the operating environment across
 * network boundaries. This is a stub for the initial kernel —
 * local-only node registry with trust zone classification.
 */

#ifndef ANX_NETPLANE_H
#define ANX_NETPLANE_H

#include <anx/types.h>
#include <anx/list.h>
#include <anx/spinlock.h>

/* --- Node types (RFC-0006 Section 6.1) --- */

enum anx_node_type {
	ANX_NODE_PERSONAL,
	ANX_NODE_EDGE,
	ANX_NODE_TEAM_SERVER,
	ANX_NODE_CLOUD,
	ANX_NODE_INFERENCE_ENDPOINT,
};

/* --- Trust zones (RFC-0006 Section 4.4) --- */

enum anx_trust_zone {
	ANX_TRUST_LOCAL,	/* full trust, local node */
	ANX_TRUST_LAN,		/* trusted peers on local network */
	ANX_TRUST_EDGE,		/* edge resources, moderate trust */
	ANX_TRUST_REMOTE,	/* remote/cloud, policy-gated trust */
	ANX_TRUST_UNTRUSTED,	/* no trust — requires full verification */
};

/* --- Node status --- */

enum anx_node_status {
	ANX_NODE_ONLINE,
	ANX_NODE_DEGRADED,
	ANX_NODE_OFFLINE,
	ANX_NODE_PARTITIONED,
};

/* --- Network node --- */

struct anx_net_node {
	anx_nid_t nid;
	char name[64];
	enum anx_node_type node_type;
	enum anx_trust_zone trust_zone;
	enum anx_node_status status;

	/* Bookkeeping */
	struct anx_spinlock lock;
	struct anx_list_head registry_link;
};

/* --- Network Plane API --- */

/* Initialize the network plane (registers local node) */
void anx_netplane_init(void);

/* Get the local node */
struct anx_net_node *anx_netplane_local_node(void);

/* Register a peer node */
int anx_netplane_register_peer(const char *name,
			       enum anx_node_type type,
			       enum anx_trust_zone trust,
			       struct anx_net_node **out);

/* Look up a node by NID */
struct anx_net_node *anx_netplane_lookup(const anx_nid_t *nid);

/* Update node status */
int anx_netplane_set_status(struct anx_net_node *node,
			    enum anx_node_status status);

#endif /* ANX_NETPLANE_H */
