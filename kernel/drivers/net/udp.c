/*
 * udp.c — Minimal UDP send/receive with port dispatch.
 *
 * Stateless datagram service. Drivers bind to ports and receive
 * callbacks when packets arrive. Checksum is set to 0 (optional
 * for IPv4 UDP).
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/string.h>

#define UDP_MAX_BINDS	8

static struct {
	uint16_t port;
	anx_udp_recv_fn handler;
	void *arg;
} udp_binds[UDP_MAX_BINDS];

void anx_udp_init(void)
{
	anx_memset(udp_binds, 0, sizeof(udp_binds));
}

int anx_udp_bind(uint16_t port, anx_udp_recv_fn handler, void *arg)
{
	int i;

	for (i = 0; i < UDP_MAX_BINDS; i++) {
		if (udp_binds[i].port == 0) {
			udp_binds[i].port = port;
			udp_binds[i].handler = handler;
			udp_binds[i].arg = arg;
			return ANX_OK;
		}
	}
	return ANX_ENOMEM;
}

void anx_udp_unbind(uint16_t port)
{
	int i;

	for (i = 0; i < UDP_MAX_BINDS; i++) {
		if (udp_binds[i].port == port) {
			udp_binds[i].port = 0;
			udp_binds[i].handler = NULL;
			udp_binds[i].arg = NULL;
			return;
		}
	}
}

void anx_udp_recv(const void *data, uint32_t len, uint32_t src_ip)
{
	const struct anx_udp_hdr *udp = (const struct anx_udp_hdr *)data;
	uint16_t dst_port, src_port, payload_len;
	const uint8_t *payload;
	int i;

	if (len < sizeof(struct anx_udp_hdr))
		return;

	dst_port = anx_ntohs(udp->dst_port);
	src_port = anx_ntohs(udp->src_port);
	payload_len = anx_ntohs(udp->length);

	if (payload_len < sizeof(struct anx_udp_hdr) || payload_len > len)
		return;

	payload = (const uint8_t *)data + sizeof(struct anx_udp_hdr);
	payload_len -= (uint16_t)sizeof(struct anx_udp_hdr);

	for (i = 0; i < UDP_MAX_BINDS; i++) {
		if (udp_binds[i].port == dst_port &&
		    udp_binds[i].handler) {
			udp_binds[i].handler(payload, payload_len,
					     src_ip, src_port,
					     udp_binds[i].arg);
			return;
		}
	}
}

int anx_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
		  const void *data, uint32_t len)
{
	uint8_t pkt[ANX_ETH_MTU];
	struct anx_udp_hdr *udp = (struct anx_udp_hdr *)pkt;
	uint16_t total = (uint16_t)(sizeof(struct anx_udp_hdr) + len);

	if (total > ANX_ETH_MTU - sizeof(struct anx_ipv4_hdr))
		return ANX_EINVAL;

	udp->src_port = anx_htons(src_port);
	udp->dst_port = anx_htons(dst_port);
	udp->length = anx_htons(total);
	udp->checksum = 0;	/* optional for IPv4 */

	anx_memcpy(pkt + sizeof(struct anx_udp_hdr), data, len);

	return anx_ipv4_send(dst_ip, ANX_IP_PROTO_UDP, pkt, total);
}
