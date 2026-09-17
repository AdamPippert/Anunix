/*
 * eth.c — Ethernet frame dispatch and construction.
 *
 * Receives raw frames from the NIC (virtio-net preferred, e1000 fallback),
 * parses the Ethernet header, and dispatches to ARP or IPv4 handlers.
 * Provides frame construction for outbound packets.
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/virtio_net.h>
#include <anx/e1000.h>
#include <anx/mt7925.h>
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

/*
 * The active NIC, in the order anx_eth_send() tries them. ARP and DHCP
 * used to ask virtio-net for the address directly, which left every other
 * NIC announcing the wrong MAC and made DHCP refuse to run at all.
 */
bool anx_eth_ready(void)
{
	return anx_virtio_net_ready() || anx_e1000_ready() ||
	       anx_mt7925_ready();
}

const char *anx_eth_name(void)
{
	if (anx_virtio_net_ready())
		return "virtio-net0";
	if (anx_e1000_ready())
		return "e1000";
	if (anx_mt7925_ready())
		return "wlan0 (mt7925)";
	return "none";
}

int anx_eth_mac(uint8_t out[6])
{
	if (anx_virtio_net_ready()) {
		anx_virtio_net_mac(out);
		return ANX_OK;
	}
	if (anx_e1000_ready()) {
		anx_memcpy(out, anx_e1000_mac(), ANX_ETH_ALEN);
		return ANX_OK;
	}
	if (anx_mt7925_ready()) {
		anx_memcpy(out, anx_mt7925_mac(), ANX_ETH_ALEN);
		return ANX_OK;
	}
	anx_memset(out, 0, ANX_ETH_ALEN);
	return ANX_EIO;
}

int anx_eth_send(const uint8_t dst[6], uint16_t ethertype,
		 const void *payload, uint32_t len)
{
	uint8_t frame[ANX_ETH_FRAME_MAX];
	struct anx_eth_hdr *hdr = (struct anx_eth_hdr *)frame;

	if (len > ANX_ETH_MTU)
		return ANX_EINVAL;

	anx_memcpy(hdr->dst, dst, ANX_ETH_ALEN);
	hdr->ethertype = anx_htons(ethertype);
	anx_memcpy(frame + ANX_ETH_HLEN, payload, len);

	if (anx_virtio_net_ready()) {
		anx_virtio_net_mac(hdr->src);
		return anx_virtio_net_send(frame, ANX_ETH_HLEN + len);
	}

	if (anx_e1000_ready()) {
		anx_memcpy(hdr->src, anx_e1000_mac(), ANX_ETH_ALEN);
		return anx_e1000_tx(frame, (uint16_t)(ANX_ETH_HLEN + len));
	}

	if (anx_mt7925_ready()) {
		anx_memcpy(hdr->src, anx_mt7925_mac(), ANX_ETH_ALEN);
		return anx_mt7925_tx(frame, (uint16_t)(ANX_ETH_HLEN + len));
	}

	return ANX_EIO;
}

void anx_net_poll(void)
{
	if (anx_virtio_net_ready())
		anx_virtio_net_poll(eth_recv_cb, NULL);
	else if (anx_e1000_ready())
		anx_e1000_poll();
	else
		anx_mt7925_poll();
}
