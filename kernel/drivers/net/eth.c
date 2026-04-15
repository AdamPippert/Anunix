/*
 * eth.c — Ethernet frame dispatch and construction.
 *
 * Receives raw frames from the virtio-net driver, parses the
 * Ethernet header, and dispatches to ARP or IPv4 handlers.
 * Provides frame construction for outbound packets.
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/virtio_net.h>
#include <anx/string.h>

/* Callback registered with virtio-net poll */
static void eth_recv_cb(const void *frame, uint32_t len, void *arg)
{
	(void)arg;
	anx_eth_recv(frame, len);
}

void anx_eth_recv(const void *frame, uint32_t len)
{
	const struct anx_eth_hdr *hdr;
	uint16_t etype;

	if (len < ANX_ETH_HLEN)
		return;

	hdr = (const struct anx_eth_hdr *)frame;
	etype = anx_ntohs(hdr->ethertype);

	switch (etype) {
	case ANX_ETH_P_ARP:
		anx_arp_recv((const uint8_t *)frame + ANX_ETH_HLEN,
			     len - ANX_ETH_HLEN);
		break;
	case ANX_ETH_P_IP:
		anx_ipv4_recv((const uint8_t *)frame + ANX_ETH_HLEN,
			      len - ANX_ETH_HLEN);
		break;
	default:
		break;
	}
}

int anx_eth_send(const uint8_t dst[6], uint16_t ethertype,
		 const void *payload, uint32_t len)
{
	uint8_t frame[ANX_ETH_FRAME_MAX];
	struct anx_eth_hdr *hdr = (struct anx_eth_hdr *)frame;

	if (len > ANX_ETH_MTU)
		return ANX_EINVAL;

	anx_memcpy(hdr->dst, dst, ANX_ETH_ALEN);
	anx_virtio_net_mac(hdr->src);
	hdr->ethertype = anx_htons(ethertype);
	anx_memcpy(frame + ANX_ETH_HLEN, payload, len);

	return anx_virtio_net_send(frame, ANX_ETH_HLEN + len);
}

void anx_net_poll(void)
{
	anx_virtio_net_poll(eth_recv_cb, NULL);
}
