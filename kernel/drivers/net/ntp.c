/*
 * ntp.c — Simple NTP client for time synchronization.
 *
 * Sends an NTP query to a time server (default: time.nist.gov)
 * and adjusts the RTC offset to match network time.
 *
 * Uses SNTP (Simple NTP) — single request/response, no drift
 * correction. Sufficient for boot-time synchronization.
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/io.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define NTP_PORT	123
#define NTP_LOCAL_PORT	10123
#define NTP_TIMEOUT_MS	3000

/* NTP packet (48 bytes minimum) */
struct ntp_packet {
	uint8_t li_vn_mode;	/* LI=0, VN=4, Mode=3 (client) */
	uint8_t stratum;
	uint8_t poll;
	uint8_t precision;
	uint32_t root_delay;
	uint32_t root_dispersion;
	uint32_t ref_id;
	uint32_t ref_ts_sec;
	uint32_t ref_ts_frac;
	uint32_t orig_ts_sec;
	uint32_t orig_ts_frac;
	uint32_t rx_ts_sec;
	uint32_t rx_ts_frac;
	uint32_t tx_ts_sec;	/* transmit timestamp — this is what we want */
	uint32_t tx_ts_frac;
} __attribute__((packed));

/* NTP epoch: 1900-01-01, UNIX epoch: 1970-01-01 */
#define NTP_UNIX_OFFSET		2208988800ULL

static volatile bool ntp_got_reply;
static uint32_t ntp_timestamp;

static void ntp_recv_cb(const void *data, uint32_t len,
			 uint32_t src_ip, uint16_t src_port, void *arg)
{
	const struct ntp_packet *pkt = (const struct ntp_packet *)data;

	(void)src_ip;
	(void)src_port;
	(void)arg;

	if (len < sizeof(struct ntp_packet))
		return;

	/* tx_ts_sec is in NTP epoch (seconds since 1900) */
	ntp_timestamp = anx_ntohl(pkt->tx_ts_sec);
	ntp_got_reply = true;
}

int anx_ntp_sync(uint32_t server_ip)
{
	struct ntp_packet pkt;
	uint64_t start, timeout_ticks;

	if (server_ip == 0)
		return ANX_EINVAL;

	anx_memset(&pkt, 0, sizeof(pkt));
	pkt.li_vn_mode = 0x23;	/* LI=0, VN=4, Mode=3 (client) */

	ntp_got_reply = false;
	ntp_timestamp = 0;

	anx_udp_bind(NTP_LOCAL_PORT, ntp_recv_cb, NULL);
	anx_udp_send(server_ip, NTP_LOCAL_PORT, NTP_PORT,
		     &pkt, sizeof(pkt));

	timeout_ticks = (NTP_TIMEOUT_MS * 100) / 1000;
	start = arch_timer_ticks();

	while (!ntp_got_reply &&
	       arch_timer_ticks() - start < timeout_ticks)
		anx_net_poll();

	anx_udp_unbind(NTP_LOCAL_PORT);

	if (!ntp_got_reply)
		return ANX_ETIMEDOUT;

	{
		uint32_t unix_ts = ntp_timestamp - (uint32_t)NTP_UNIX_OFFSET;
		uint32_t secs = unix_ts % 60;
		uint32_t mins = (unix_ts / 60) % 60;
		uint32_t hrs  = (unix_ts / 3600) % 24;

		kprintf("ntp: %u:%u:%u UTC (unix %u)\n",
			hrs, mins, secs, unix_ts);

		/*
		 * Set the CMOS RTC to match NTP time.
		 * This is a simplified write — real RTC update should
		 * wait for the update-in-progress flag.
		 */
		{
			uint8_t status_b = 0x06; /* 24hr, binary mode */

			/* Disable NMI and set RTC */
			anx_outb(0x0B | 0x80, 0x70);
			anx_outb(status_b, 0x71);

			anx_outb(0x00 | 0x80, 0x70);
			anx_outb((uint8_t)secs, 0x71);

			anx_outb(0x02 | 0x80, 0x70);
			anx_outb((uint8_t)mins, 0x71);

			anx_outb(0x04 | 0x80, 0x70);
			anx_outb((uint8_t)hrs, 0x71);

			/* Re-enable NMI */
			anx_outb(0x0B, 0x70);
			anx_outb(status_b, 0x71);
		}
	}

	return ANX_OK;
}
