/*
 * dns.c — Simple DNS A-record resolver.
 *
 * Sends a standard DNS query to the configured DNS server via UDP
 * and parses the first A record from the response. Blocking with
 * timeout.
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define DNS_PORT		53
#define DNS_LOCAL_PORT		10053
#define DNS_TIMEOUT_MS		3000
#define DNS_MAX_RESPONSE	512

/* DNS header (12 bytes) */
struct dns_header {
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;
} __attribute__((packed));

#define DNS_FLAG_QR		0x8000	/* response flag */
#define DNS_FLAG_RD		0x0100	/* recursion desired */
#define DNS_TYPE_A		1
#define DNS_CLASS_IN		1

/* State for receiving DNS response */
static volatile bool dns_got_reply;
static uint32_t dns_result_ip;
static uint16_t dns_txn_id;

static void dns_recv_cb(const void *data, uint32_t len,
			uint32_t src_ip, uint16_t src_port, void *arg)
{
	const struct dns_header *hdr;
	const uint8_t *p, *end;
	uint16_t ancount;
	int i;

	(void)src_ip;
	(void)src_port;
	(void)arg;

	if (len < sizeof(struct dns_header))
		return;

	hdr = (const struct dns_header *)data;

	/* Check it's a response matching our transaction */
	if (anx_ntohs(hdr->id) != dns_txn_id)
		return;
	if (!(anx_ntohs(hdr->flags) & DNS_FLAG_QR))
		return;

	ancount = anx_ntohs(hdr->ancount);
	if (ancount == 0)
		return;

	/* Skip past the header and question section */
	p = (const uint8_t *)data + sizeof(struct dns_header);
	end = (const uint8_t *)data + len;

	/* Skip question: walk labels until null, then skip QTYPE + QCLASS */
	while (p < end && *p != 0) {
		if ((*p & 0xC0) == 0xC0) {
			p += 2;	/* compression pointer */
			goto past_question;
		}
		p += 1 + *p;	/* length byte + label */
	}
	if (p < end)
		p++;	/* skip null terminator */
past_question:
	p += 4;		/* skip QTYPE (2) + QCLASS (2) */

	/* Parse answer records looking for type A */
	for (i = 0; i < (int)ancount && p + 12 <= end; i++) {
		uint16_t atype, aclass, rdlength;

		/* Skip name (could be pointer or labels) */
		if ((*p & 0xC0) == 0xC0) {
			p += 2;
		} else {
			while (p < end && *p != 0)
				p += 1 + *p;
			if (p < end)
				p++;
		}

		if (p + 10 > end)
			break;

		atype = ((uint16_t)p[0] << 8) | p[1];
		aclass = ((uint16_t)p[2] << 8) | p[3];
		/* skip TTL (4 bytes) */
		rdlength = ((uint16_t)p[8] << 8) | p[9];
		p += 10;

		if (atype == DNS_TYPE_A && aclass == DNS_CLASS_IN &&
		    rdlength == 4 && p + 4 <= end) {
			dns_result_ip = ((uint32_t)p[0] << 24) |
					((uint32_t)p[1] << 16) |
					((uint32_t)p[2] << 8) |
					 (uint32_t)p[3];
			dns_got_reply = true;
			return;
		}

		p += rdlength;
	}
}

/* Encode a hostname into DNS wire format (labels) */
static int dns_encode_name(const char *name, uint8_t *buf, uint32_t bufsize)
{
	uint8_t *label_len;
	uint32_t pos = 0;
	uint32_t count = 0;

	if (pos >= bufsize)
		return ANX_EINVAL;

	label_len = &buf[pos++];
	*label_len = 0;

	while (*name) {
		if (*name == '.') {
			if (count == 0)
				return ANX_EINVAL;
			label_len = &buf[pos++];
			*label_len = 0;
			count = 0;
		} else {
			if (pos >= bufsize - 1)
				return ANX_EINVAL;
			buf[pos++] = (uint8_t)*name;
			(*label_len)++;
			count++;
		}
		name++;
	}

	/* Null terminator */
	if (pos >= bufsize)
		return ANX_EINVAL;
	buf[pos++] = 0;

	return (int)pos;
}

void anx_dns_init(void)
{
	dns_got_reply = false;
	dns_result_ip = 0;
	dns_txn_id = 0;
}

int anx_dns_resolve(const char *hostname, uint32_t *ip_out)
{
	uint8_t pkt[DNS_MAX_RESPONSE];
	struct dns_header *hdr = (struct dns_header *)pkt;
	uint8_t *p;
	int name_len;
	uint32_t query_len;
	uint32_t dns_server;
	uint64_t start, timeout_ticks;

	dns_server = anx_ipv4_dns();
	if (dns_server == 0)
		return ANX_EINVAL;

	/* Build DNS query */
	anx_memset(pkt, 0, sizeof(pkt));

	dns_txn_id = (uint16_t)(arch_time_now() & 0xFFFF);
	hdr->id = anx_htons(dns_txn_id);
	hdr->flags = anx_htons(DNS_FLAG_RD);
	hdr->qdcount = anx_htons(1);

	p = pkt + sizeof(struct dns_header);
	name_len = dns_encode_name(hostname, p, DNS_MAX_RESPONSE -
				   sizeof(struct dns_header) - 4);
	if (name_len < 0)
		return ANX_EINVAL;

	p += name_len;

	/* QTYPE = A (1), QCLASS = IN (1) */
	p[0] = 0; p[1] = DNS_TYPE_A;
	p[2] = 0; p[3] = DNS_CLASS_IN;
	p += 4;

	query_len = (uint32_t)(p - pkt);

	/* Bind to receive response */
	dns_got_reply = false;
	dns_result_ip = 0;
	anx_udp_bind(DNS_LOCAL_PORT, dns_recv_cb, NULL);

	/* Send query */
	anx_udp_send(dns_server, DNS_LOCAL_PORT, DNS_PORT, pkt, query_len);

	/* Poll for response */
	timeout_ticks = (DNS_TIMEOUT_MS * 100) / 1000;
	start = arch_timer_ticks();

	while (!dns_got_reply &&
	       arch_timer_ticks() - start < timeout_ticks) {
		anx_net_poll();
	}

	anx_udp_unbind(DNS_LOCAL_PORT);

	if (dns_got_reply) {
		*ip_out = dns_result_ip;
		return ANX_OK;
	}

	return ANX_ETIMEDOUT;
}
