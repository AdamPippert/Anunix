/*
 * arp.c — ARP table and request/reply handling.
 *
 * Maintains a small static ARP cache. Supports blocking resolution
 * that sends an ARP request and polls the network device until a
 * reply arrives or the timeout expires.
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/virtio_net.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define ARP_TABLE_SIZE	16
#define ARP_TIMEOUT_MS	2000	/* 2 seconds */
#define ARP_RETRIES	3

static struct {
	uint32_t ip;		/* host byte order */
	uint8_t mac[ANX_ETH_ALEN];
	bool valid;
} arp_table[ARP_TABLE_SIZE];

static uint32_t our_ip;		/* host byte order */

void anx_arp_init(void)
{
	anx_memset(arp_table, 0, sizeof(arp_table));
}

/* Look up an IP in the ARP table */
static int arp_lookup(uint32_t ip, uint8_t mac_out[6])
{
	int i;

	for (i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && arp_table[i].ip == ip) {
			anx_memcpy(mac_out, arp_table[i].mac, ANX_ETH_ALEN);
			return ANX_OK;
		}
	}
	return ANX_ENOENT;
}

/* Add or update an ARP table entry */
static void arp_update(uint32_t ip, const uint8_t mac[6])
{
	int i;
	int free_slot = -1;

	for (i = 0; i < ARP_TABLE_SIZE; i++) {
		if (arp_table[i].valid && arp_table[i].ip == ip) {
			anx_memcpy(arp_table[i].mac, mac, ANX_ETH_ALEN);
			return;
		}
		if (!arp_table[i].valid && free_slot < 0)
			free_slot = i;
	}

	if (free_slot >= 0) {
		arp_table[free_slot].ip = ip;
		anx_memcpy(arp_table[free_slot].mac, mac, ANX_ETH_ALEN);
		arp_table[free_slot].valid = true;
	}
}

/* Send an ARP request for the given IP */
static void arp_send_request(uint32_t target_ip)
{
	struct anx_arp_pkt arp;
	uint8_t bcast[ANX_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

	arp.hw_type = anx_htons(ANX_ARP_HW_ETHER);
	arp.proto_type = anx_htons(ANX_ETH_P_IP);
	arp.hw_len = ANX_ETH_ALEN;
	arp.proto_len = 4;
	arp.opcode = anx_htons(ANX_ARP_OP_REQUEST);

	anx_virtio_net_mac(arp.sender_mac);
	arp.sender_ip = anx_htonl(our_ip);
	anx_memset(arp.target_mac, 0, ANX_ETH_ALEN);
	arp.target_ip = anx_htonl(target_ip);

	anx_eth_send(bcast, ANX_ETH_P_ARP, &arp, sizeof(arp));
}

void anx_arp_recv(const void *data, uint32_t len)
{
	const struct anx_arp_pkt *arp = (const struct anx_arp_pkt *)data;
	uint32_t sender_ip;

	if (len < sizeof(struct anx_arp_pkt))
		return;
	if (anx_ntohs(arp->hw_type) != ANX_ARP_HW_ETHER)
		return;
	if (anx_ntohs(arp->proto_type) != ANX_ETH_P_IP)
		return;

	sender_ip = anx_ntohl(arp->sender_ip);

	/* Learn the sender's MAC */
	arp_update(sender_ip, arp->sender_mac);

	/* If it's a request for our IP, send a reply */
	if (anx_ntohs(arp->opcode) == ANX_ARP_OP_REQUEST &&
	    anx_ntohl(arp->target_ip) == our_ip) {
		struct anx_arp_pkt reply;

		reply.hw_type = anx_htons(ANX_ARP_HW_ETHER);
		reply.proto_type = anx_htons(ANX_ETH_P_IP);
		reply.hw_len = ANX_ETH_ALEN;
		reply.proto_len = 4;
		reply.opcode = anx_htons(ANX_ARP_OP_REPLY);

		anx_virtio_net_mac(reply.sender_mac);
		reply.sender_ip = anx_htonl(our_ip);
		anx_memcpy(reply.target_mac, arp->sender_mac, ANX_ETH_ALEN);
		reply.target_ip = arp->sender_ip;

		anx_eth_send(reply.target_mac, ANX_ETH_P_ARP,
			     &reply, sizeof(reply));
	}
}

int anx_arp_resolve(uint32_t ip, uint8_t mac_out[6])
{
	int retry;
	uint64_t start, timeout_ticks;

	/* Check cache first */
	if (arp_lookup(ip, mac_out) == ANX_OK)
		return ANX_OK;

	/* ~100 ticks/sec from PIT */
	timeout_ticks = (ARP_TIMEOUT_MS * 100) / 1000;

	for (retry = 0; retry < ARP_RETRIES; retry++) {
		arp_send_request(ip);
		start = arch_timer_ticks();

		while (arch_timer_ticks() - start < timeout_ticks) {
			anx_net_poll();
			if (arp_lookup(ip, mac_out) == ANX_OK)
				return ANX_OK;
		}
	}

	return ANX_ETIMEDOUT;
}

/* Called by net stack init to set our IP for ARP replies */
void anx_arp_set_ip(uint32_t ip)
{
	our_ip = ip;
}
