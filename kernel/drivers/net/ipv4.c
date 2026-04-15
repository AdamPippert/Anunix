/*
 * ipv4.c — IPv4 send and receive with header checksum.
 *
 * All outbound packets are routed via the configured gateway.
 * No fragmentation support — assumes MTU 1500 is sufficient.
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/string.h>
#include <anx/kprintf.h>

static struct anx_net_config net_cfg;
static uint16_t ip_id_counter;

void anx_ipv4_init(const struct anx_net_config *cfg)
{
	net_cfg = *cfg;
	ip_id_counter = 1;
}

uint16_t anx_ip_checksum(const void *data, uint32_t len)
{
	const uint16_t *p = (const uint16_t *)data;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *p++;
		len -= 2;
	}
	if (len == 1)
		sum += *(const uint8_t *)p;

	sum = (sum >> 16) + (sum & 0xFFFF);
	sum += (sum >> 16);
	return (uint16_t)~sum;
}

void anx_ipv4_recv(const void *data, uint32_t len)
{
	const struct anx_ipv4_hdr *ip = (const struct anx_ipv4_hdr *)data;
	uint8_t ihl;
	uint16_t total_len;
	uint32_t src_ip;
	const uint8_t *payload;
	uint32_t payload_len;

	if (len < sizeof(struct anx_ipv4_hdr))
		return;

	/* Check version */
	if ((ip->ver_ihl >> 4) != 4)
		return;

	ihl = (ip->ver_ihl & 0x0F) * 4;
	if (ihl < 20 || len < ihl)
		return;

	total_len = anx_ntohs(ip->total_len);
	if (total_len > len)
		return;

	src_ip = anx_ntohl(ip->src_ip);
	payload = (const uint8_t *)data + ihl;
	payload_len = total_len - ihl;

	switch (ip->protocol) {
	case ANX_IP_PROTO_ICMP:
		anx_icmp_recv(payload, payload_len, src_ip);
		break;
	case ANX_IP_PROTO_UDP:
		/* TODO: add UDP dispatch */
		break;
	case ANX_IP_PROTO_TCP:
		/* TODO: add TCP dispatch */
		break;
	default:
		break;
	}
}

int anx_ipv4_send(uint32_t dst_ip, uint8_t proto,
		   const void *payload, uint32_t len)
{
	uint8_t pkt[ANX_ETH_MTU];
	struct anx_ipv4_hdr *ip = (struct anx_ipv4_hdr *)pkt;
	uint32_t total = sizeof(struct anx_ipv4_hdr) + len;
	uint32_t next_hop;
	uint8_t gw_mac[ANX_ETH_ALEN];
	int ret;

	if (total > ANX_ETH_MTU)
		return ANX_EINVAL;

	/* Build IP header */
	ip->ver_ihl = 0x45;		/* IPv4, IHL=5 (20 bytes) */
	ip->tos = 0;
	ip->total_len = anx_htons((uint16_t)total);
	ip->id = anx_htons(ip_id_counter++);
	ip->frag_off = 0;
	ip->ttl = 64;
	ip->protocol = proto;
	ip->checksum = 0;
	ip->src_ip = anx_htonl(net_cfg.ip);
	ip->dst_ip = anx_htonl(dst_ip);

	/* Compute header checksum */
	ip->checksum = anx_ip_checksum(ip, sizeof(struct anx_ipv4_hdr));

	/* Copy payload */
	anx_memcpy(pkt + sizeof(struct anx_ipv4_hdr), payload, len);

	/* Route: if on same subnet, send direct; otherwise via gateway */
	if ((dst_ip & net_cfg.netmask) == (net_cfg.ip & net_cfg.netmask))
		next_hop = dst_ip;
	else
		next_hop = net_cfg.gateway;

	/* Resolve next-hop MAC via ARP */
	ret = anx_arp_resolve(next_hop, gw_mac);
	if (ret != ANX_OK)
		return ret;

	return anx_eth_send(gw_mac, ANX_ETH_P_IP, pkt, total);
}

/* Accessor for other layers that need the config */
uint32_t anx_ipv4_local_ip(void)
{
	return net_cfg.ip;
}

uint32_t anx_ipv4_dns(void)
{
	return net_cfg.dns;
}
