/*
 * icmp.c — ICMP echo request and reply.
 *
 * Responds to incoming echo requests (ping) and provides an
 * outbound ping function for connectivity testing.
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/string.h>
#include <anx/arch.h>
#include <anx/kprintf.h>

/* Reuse the IP checksum for ICMP */
static uint16_t icmp_checksum(const void *data, uint32_t len)
{
	return anx_ip_checksum(data, len);
}

void anx_icmp_recv(const void *data, uint32_t len, uint32_t src_ip)
{
	const struct anx_icmp_hdr *icmp = (const struct anx_icmp_hdr *)data;

	if (len < sizeof(struct anx_icmp_hdr))
		return;

	if (icmp->type == ANX_ICMP_ECHO_REQUEST && icmp->code == 0) {
		/* Build echo reply — copy entire payload, swap type */
		uint8_t reply[ANX_ETH_MTU];
		uint32_t reply_len = len;
		struct anx_icmp_hdr *rhdr;

		if (reply_len > sizeof(reply))
			reply_len = sizeof(reply);

		anx_memcpy(reply, data, reply_len);
		rhdr = (struct anx_icmp_hdr *)reply;
		rhdr->type = ANX_ICMP_ECHO_REPLY;
		rhdr->checksum = 0;
		rhdr->checksum = icmp_checksum(reply, reply_len);

		anx_ipv4_send(src_ip, ANX_IP_PROTO_ICMP, reply, reply_len);
	}
}

int anx_icmp_ping(uint32_t dst_ip, uint16_t seq)
{
	struct anx_icmp_hdr icmp;
	uint64_t start;
	int ret;

	anx_memset(&icmp, 0, sizeof(icmp));
	icmp.type = ANX_ICMP_ECHO_REQUEST;
	icmp.code = 0;
	icmp.id = anx_htons(0x4E58);	/* "NX" */
	icmp.seq = anx_htons(seq);
	icmp.checksum = 0;
	icmp.checksum = icmp_checksum(&icmp, sizeof(icmp));

	ret = anx_ipv4_send(dst_ip, ANX_IP_PROTO_ICMP, &icmp, sizeof(icmp));
	if (ret != ANX_OK)
		return ret;

	/* Wait for reply (up to 2 seconds) */
	start = arch_timer_ticks();
	while (arch_timer_ticks() - start < 200) {
		anx_net_poll();
		/* Reply would be handled by anx_icmp_recv but we don't
		 * have a reply-tracking mechanism yet — just poll to
		 * give the stack a chance to process. For now, consider
		 * the ping "sent" and return success. */
	}

	return ANX_OK;
}
