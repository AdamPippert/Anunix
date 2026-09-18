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

#define PING_ID		0x4E58		/* "NX" */

/* The echo reply anx_icmp_ping() is waiting for, if any. */
static uint16_t ping_wait_seq;
static bool ping_waiting;
static bool ping_answered;
static uint32_t ping_from;

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
	} else if (icmp->type == ANX_ICMP_ECHO_REPLY && ping_waiting &&
		   anx_ntohs(icmp->id) == PING_ID &&
		   anx_ntohs(icmp->seq) == ping_wait_seq) {
		ping_answered = true;
		ping_from = src_ip;
	}
}

int anx_icmp_ping(uint32_t dst_ip, uint16_t seq, uint32_t *rtt_ms)
{
	struct anx_icmp_hdr icmp;
	uint64_t start, now;
	int ret;

	anx_memset(&icmp, 0, sizeof(icmp));
	icmp.type = ANX_ICMP_ECHO_REQUEST;
	icmp.code = 0;
	icmp.id = anx_htons(PING_ID);
	icmp.seq = anx_htons(seq);
	icmp.checksum = 0;
	icmp.checksum = icmp_checksum(&icmp, sizeof(icmp));

	ping_wait_seq = seq;
	ping_answered = false;
	ping_waiting = true;
	start = arch_timer_ticks();
	ret = anx_ipv4_send(dst_ip, ANX_IP_PROTO_ICMP, &icmp, sizeof(icmp));
	if (ret != ANX_OK) {
		ping_waiting = false;
		return ret;
	}

	/* Wait for the matching reply (up to 2 seconds at 100 Hz) */
	do {
		anx_net_poll();
		now = arch_timer_ticks();
	} while (!ping_answered && now - start < 200);
	ping_waiting = false;

	if (!ping_answered || ping_from != dst_ip)
		return ANX_ETIMEDOUT;
	if (rtt_ms)
		*rtt_ms = (uint32_t)((now - start) * 10);
	return ANX_OK;
}
