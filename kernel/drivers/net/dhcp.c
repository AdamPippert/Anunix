/*
 * dhcp.c — DHCP client for network auto-configuration.
 *
 * Implements the DHCP discover/offer/request/ack handshake to
 * obtain an IP address, gateway, netmask, and DNS server from
 * the network. Falls back to static config if DHCP fails.
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/virtio_net.h>
#include <anx/alloc.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* DHCP ports */
#define DHCP_CLIENT_PORT	68
#define DHCP_SERVER_PORT	67

/* DHCP message types */
#define DHCP_DISCOVER		1
#define DHCP_OFFER		2
#define DHCP_REQUEST		3
#define DHCP_ACK		5

/* DHCP options */
#define OPT_SUBNET_MASK		1
#define OPT_ROUTER		3
#define OPT_DNS			6
#define OPT_REQUESTED_IP	50
#define OPT_MSG_TYPE		53
#define OPT_SERVER_ID		54
#define OPT_END			255

/* DHCP packet (simplified, 576 bytes minimum) */
struct dhcp_packet {
	uint8_t op;		/* 1=request, 2=reply */
	uint8_t htype;		/* 1=ethernet */
	uint8_t hlen;		/* 6 */
	uint8_t hops;
	uint32_t xid;		/* transaction ID */
	uint16_t secs;
	uint16_t flags;
	uint32_t ciaddr;	/* client IP */
	uint32_t yiaddr;	/* your IP (offered) */
	uint32_t siaddr;	/* server IP */
	uint32_t giaddr;	/* gateway IP */
	uint8_t chaddr[16];	/* client MAC (6 bytes used) */
	uint8_t sname[64];
	uint8_t file[128];
	uint32_t magic;		/* 0x63825363 */
	uint8_t options[312];
} __attribute__((packed));

#define DHCP_MAGIC	0x63825363

/* State */
static volatile bool dhcp_got_offer;
static volatile bool dhcp_got_ack;
static uint32_t offered_ip;
static uint32_t offered_gateway;
static uint32_t offered_netmask;
static uint32_t offered_dns;
static uint32_t offered_server_id;
static uint32_t dhcp_xid;

/* --- Option helpers --- */

static uint8_t *add_option(uint8_t *p, uint8_t code, uint8_t len,
			    const void *data)
{
	*p++ = code;
	*p++ = len;
	anx_memcpy(p, data, len);
	return p + len;
}

static uint8_t *add_option_byte(uint8_t *p, uint8_t code, uint8_t val)
{
	return add_option(p, code, 1, &val);
}

static uint8_t *add_option_u32(uint8_t *p, uint8_t code, uint32_t val)
{
	return add_option(p, code, 4, &val);
}

static bool find_option(const uint8_t *options, uint32_t len,
			 uint8_t code, void *out, uint8_t out_len)
{
	uint32_t i = 0;

	while (i < len && options[i] != OPT_END) {
		uint8_t opt_code = options[i++];
		uint8_t opt_len;

		if (opt_code == 0)
			continue;	/* padding */
		if (i >= len)
			break;
		opt_len = options[i++];
		if (opt_code == code && opt_len <= out_len) {
			anx_memcpy(out, &options[i], opt_len);
			return true;
		}
		i += opt_len;
	}
	return false;
}

/* --- Packet construction --- */

static void build_dhcp_packet(struct dhcp_packet *pkt, uint8_t msg_type)
{
	uint8_t *p;

	anx_memset(pkt, 0, sizeof(*pkt));
	pkt->op = 1;		/* BOOTREQUEST */
	pkt->htype = 1;	/* Ethernet */
	pkt->hlen = 6;
	pkt->xid = anx_htonl(dhcp_xid);
	pkt->flags = anx_htons(0x8000);	/* broadcast */
	anx_virtio_net_mac(pkt->chaddr);
	pkt->magic = anx_htonl(DHCP_MAGIC);

	p = pkt->options;
	p = add_option_byte(p, OPT_MSG_TYPE, msg_type);

	if (msg_type == DHCP_REQUEST) {
		p = add_option_u32(p, OPT_REQUESTED_IP,
				    anx_htonl(offered_ip));
		p = add_option_u32(p, OPT_SERVER_ID,
				    anx_htonl(offered_server_id));
	}

	*p = OPT_END;
}

/* --- Receive callback --- */

static void dhcp_recv_cb(const void *data, uint32_t len,
			  uint32_t src_ip, uint16_t src_port, void *arg)
{
	const struct dhcp_packet *pkt = (const struct dhcp_packet *)data;
	uint8_t msg_type = 0;

	(void)src_ip;
	(void)src_port;
	(void)arg;

	if (len < sizeof(struct dhcp_packet) - 312)
		return;
	if (pkt->op != 2)
		return;
	if (anx_ntohl(pkt->xid) != dhcp_xid)
		return;
	if (anx_ntohl(pkt->magic) != DHCP_MAGIC)
		return;

	find_option(pkt->options, sizeof(pkt->options),
		    OPT_MSG_TYPE, &msg_type, 1);

	if (msg_type == DHCP_OFFER && !dhcp_got_offer) {
		offered_ip = anx_ntohl(pkt->yiaddr);
		find_option(pkt->options, sizeof(pkt->options),
			    OPT_SUBNET_MASK, &offered_netmask, 4);
		offered_netmask = anx_ntohl(offered_netmask);
		find_option(pkt->options, sizeof(pkt->options),
			    OPT_ROUTER, &offered_gateway, 4);
		offered_gateway = anx_ntohl(offered_gateway);
		find_option(pkt->options, sizeof(pkt->options),
			    OPT_DNS, &offered_dns, 4);
		offered_dns = anx_ntohl(offered_dns);
		find_option(pkt->options, sizeof(pkt->options),
			    OPT_SERVER_ID, &offered_server_id, 4);
		offered_server_id = anx_ntohl(offered_server_id);
		dhcp_got_offer = true;
	} else if (msg_type == DHCP_ACK && !dhcp_got_ack) {
		/* Update from ACK (may differ from offer) */
		offered_ip = anx_ntohl(pkt->yiaddr);
		find_option(pkt->options, sizeof(pkt->options),
			    OPT_SUBNET_MASK, &offered_netmask, 4);
		offered_netmask = anx_ntohl(offered_netmask);
		find_option(pkt->options, sizeof(pkt->options),
			    OPT_ROUTER, &offered_gateway, 4);
		offered_gateway = anx_ntohl(offered_gateway);
		find_option(pkt->options, sizeof(pkt->options),
			    OPT_DNS, &offered_dns, 4);
		offered_dns = anx_ntohl(offered_dns);
		dhcp_got_ack = true;
	}
}

/* --- Public API --- */

int anx_dhcp_discover(struct anx_net_config *cfg)
{
	struct dhcp_packet *pkt;
	uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	uint64_t start;
	int ret;

	if (!anx_virtio_net_ready())
		return ANX_EIO;

	pkt = anx_zalloc(sizeof(*pkt));
	if (!pkt)
		return ANX_ENOMEM;

	dhcp_xid = (uint32_t)(arch_time_now() & 0xFFFFFFFF);
	dhcp_got_offer = false;
	dhcp_got_ack = false;

	/* Bind to DHCP client port */
	anx_udp_bind(DHCP_CLIENT_PORT, dhcp_recv_cb, NULL);

	/* Send DISCOVER */
	build_dhcp_packet(pkt, DHCP_DISCOVER);
	{
		uint8_t frame[ANX_ETH_FRAME_MAX];
		struct anx_eth_hdr *eth = (struct anx_eth_hdr *)frame;

		anx_memcpy(eth->dst, bcast, 6);
		anx_virtio_net_mac(eth->src);
		eth->ethertype = anx_htons(ANX_ETH_P_IP);

		/* Build IP + UDP + DHCP */
		{
			struct anx_ipv4_hdr *ip;
			struct anx_udp_hdr *udp;
			uint32_t total_len;

			ip = (struct anx_ipv4_hdr *)(frame + ANX_ETH_HLEN);
			udp = (struct anx_udp_hdr *)((uint8_t *)ip + 20);
			total_len = 20 + 8 + sizeof(struct dhcp_packet);

			ip->ver_ihl = 0x45;
			ip->tos = 0;
			ip->total_len = anx_htons((uint16_t)total_len);
			ip->id = 0;
			ip->frag_off = 0;
			ip->ttl = 64;
			ip->protocol = ANX_IP_PROTO_UDP;
			ip->checksum = 0;
			ip->src_ip = 0;
			ip->dst_ip = 0xFFFFFFFF;
			ip->checksum = anx_ip_checksum(ip, 20);

			udp->src_port = anx_htons(DHCP_CLIENT_PORT);
			udp->dst_port = anx_htons(DHCP_SERVER_PORT);
			udp->length = anx_htons((uint16_t)(8 + sizeof(struct dhcp_packet)));
			udp->checksum = 0;

			anx_memcpy((uint8_t *)udp + 8, pkt,
				   sizeof(struct dhcp_packet));

			ret = anx_virtio_net_send(frame,
				ANX_ETH_HLEN + total_len);
		}
	}

	if (ret != ANX_OK) {
		anx_free(pkt);
		anx_udp_unbind(DHCP_CLIENT_PORT);
		return ret;
	}

	/* Wait for OFFER (up to 5 seconds) */
	start = arch_timer_ticks();
	while (!dhcp_got_offer && arch_timer_ticks() - start < 500)
		anx_net_poll();

	if (!dhcp_got_offer) {
		anx_free(pkt);
		anx_udp_unbind(DHCP_CLIENT_PORT);
		return ANX_ETIMEDOUT;
	}

	kprintf("dhcp: offered %u.%u.%u.%u\n",
		(offered_ip >> 24) & 0xFF, (offered_ip >> 16) & 0xFF,
		(offered_ip >> 8) & 0xFF, offered_ip & 0xFF);

	/* Send REQUEST */
	build_dhcp_packet(pkt, DHCP_REQUEST);
	{
		uint8_t frame[ANX_ETH_FRAME_MAX];
		struct anx_eth_hdr *eth = (struct anx_eth_hdr *)frame;

		anx_memcpy(eth->dst, bcast, 6);
		anx_virtio_net_mac(eth->src);
		eth->ethertype = anx_htons(ANX_ETH_P_IP);

		{
			struct anx_ipv4_hdr *ip;
			struct anx_udp_hdr *udp;
			uint32_t total_len;

			ip = (struct anx_ipv4_hdr *)(frame + ANX_ETH_HLEN);
			udp = (struct anx_udp_hdr *)((uint8_t *)ip + 20);
			total_len = 20 + 8 + sizeof(struct dhcp_packet);

			ip->ver_ihl = 0x45;
			ip->tos = 0;
			ip->total_len = anx_htons((uint16_t)total_len);
			ip->id = 0;
			ip->frag_off = 0;
			ip->ttl = 64;
			ip->protocol = ANX_IP_PROTO_UDP;
			ip->checksum = 0;
			ip->src_ip = 0;
			ip->dst_ip = 0xFFFFFFFF;
			ip->checksum = anx_ip_checksum(ip, 20);

			udp->src_port = anx_htons(DHCP_CLIENT_PORT);
			udp->dst_port = anx_htons(DHCP_SERVER_PORT);
			udp->length = anx_htons((uint16_t)(8 + sizeof(struct dhcp_packet)));
			udp->checksum = 0;

			anx_memcpy((uint8_t *)udp + 8, pkt,
				   sizeof(struct dhcp_packet));

			anx_virtio_net_send(frame, ANX_ETH_HLEN + total_len);
		}
	}

	/* Wait for ACK */
	start = arch_timer_ticks();
	while (!dhcp_got_ack && arch_timer_ticks() - start < 500)
		anx_net_poll();

	anx_free(pkt);
	anx_udp_unbind(DHCP_CLIENT_PORT);

	if (!dhcp_got_ack)
		return ANX_ETIMEDOUT;

	/* Fill in the config */
	cfg->ip = offered_ip;
	cfg->netmask = offered_netmask ? offered_netmask : 0xFFFFFF00;
	cfg->gateway = offered_gateway;
	cfg->dns = offered_dns;

	kprintf("dhcp: ip %u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u\n",
		(cfg->ip >> 24) & 0xFF, (cfg->ip >> 16) & 0xFF,
		(cfg->ip >> 8) & 0xFF, cfg->ip & 0xFF,
		(cfg->gateway >> 24) & 0xFF, (cfg->gateway >> 16) & 0xFF,
		(cfg->gateway >> 8) & 0xFF, cfg->gateway & 0xFF,
		(cfg->dns >> 24) & 0xFF, (cfg->dns >> 16) & 0xFF,
		(cfg->dns >> 8) & 0xFF, cfg->dns & 0xFF);

	return ANX_OK;
}
