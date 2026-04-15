/*
 * tcp.c — Minimal TCP client implementation.
 *
 * Client-only TCP with blocking send/recv. Supports up to 4
 * simultaneous connections with a simple fixed-RTO retransmit.
 *
 * TODO: Full implementation in Phase 6.
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/arch.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define TCP_MAX_CONNS		4
#define TCP_RX_BUF_SIZE		8192
#define TCP_TX_BUF_SIZE		8192
#define TCP_RTO_TICKS		100	/* 1 second at 100 Hz */
#define TCP_MAX_RETRIES		5
#define TCP_EPHEMERAL_BASE	49152

enum anx_tcp_state {
	ANX_TCP_CLOSED,
	ANX_TCP_SYN_SENT,
	ANX_TCP_ESTABLISHED,
	ANX_TCP_FIN_WAIT_1,
	ANX_TCP_FIN_WAIT_2,
	ANX_TCP_TIME_WAIT,
	ANX_TCP_CLOSE_WAIT,
	ANX_TCP_LAST_ACK,
};

struct anx_tcp_conn {
	enum anx_tcp_state state;
	uint32_t local_ip, remote_ip;
	uint16_t local_port, remote_port;
	uint32_t snd_nxt;	/* next sequence to send */
	uint32_t snd_una;	/* oldest unacknowledged */
	uint32_t rcv_nxt;	/* next expected receive seq */
	uint16_t rcv_wnd;	/* advertised window */
	uint8_t *rx_buf;
	uint32_t rx_len;
	uint32_t rx_cap;
	uint64_t last_send_tick;
	uint8_t retries;
	bool in_use;
};

static struct anx_tcp_conn conns[TCP_MAX_CONNS];
static uint16_t next_ephemeral = TCP_EPHEMERAL_BASE;

void anx_tcp_init(void)
{
	anx_memset(conns, 0, sizeof(conns));
}

static struct anx_tcp_conn *tcp_find_conn(uint32_t remote_ip,
					   uint16_t remote_port,
					   uint16_t local_port)
{
	int i;

	for (i = 0; i < TCP_MAX_CONNS; i++) {
		if (conns[i].in_use &&
		    conns[i].remote_ip == remote_ip &&
		    conns[i].remote_port == remote_port &&
		    conns[i].local_port == local_port)
			return &conns[i];
	}
	return NULL;
}

static struct anx_tcp_conn *tcp_alloc_conn(void)
{
	int i;

	for (i = 0; i < TCP_MAX_CONNS; i++) {
		if (!conns[i].in_use) {
			anx_memset(&conns[i], 0, sizeof(conns[i]));
			conns[i].in_use = true;
			return &conns[i];
		}
	}
	return NULL;
}

/* TCP pseudo-header checksum */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
			      const void *tcp_pkt, uint32_t tcp_len)
{
	uint32_t sum = 0;
	const uint16_t *p;
	uint32_t len;

	/* Pseudo-header */
	sum += (src_ip >> 16) & 0xFFFF;
	sum += src_ip & 0xFFFF;
	sum += (dst_ip >> 16) & 0xFFFF;
	sum += dst_ip & 0xFFFF;
	sum += anx_htons(ANX_IP_PROTO_TCP);
	sum += anx_htons((uint16_t)tcp_len);

	/* TCP header + data */
	p = (const uint16_t *)tcp_pkt;
	len = tcp_len;
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

static int tcp_send_segment(struct anx_tcp_conn *conn, uint8_t flags,
			     const void *data, uint32_t data_len)
{
	uint8_t pkt[128 + TCP_TX_BUF_SIZE];
	struct anx_tcp_hdr *tcp = (struct anx_tcp_hdr *)pkt;
	uint32_t total = sizeof(struct anx_tcp_hdr) + data_len;

	anx_memset(tcp, 0, sizeof(struct anx_tcp_hdr));
	tcp->src_port = anx_htons(conn->local_port);
	tcp->dst_port = anx_htons(conn->remote_port);
	tcp->seq = anx_htonl(conn->snd_nxt);
	tcp->ack = anx_htonl(conn->rcv_nxt);
	tcp->data_off = (sizeof(struct anx_tcp_hdr) / 4) << 4;
	tcp->flags = flags;
	tcp->window = anx_htons(conn->rcv_wnd);
	tcp->checksum = 0;
	tcp->urgent = 0;

	if (data && data_len > 0)
		anx_memcpy(pkt + sizeof(struct anx_tcp_hdr), data, data_len);

	tcp->checksum = tcp_checksum(anx_htonl(conn->local_ip),
				     anx_htonl(conn->remote_ip),
				     pkt, total);

	conn->last_send_tick = arch_timer_ticks();
	return anx_ipv4_send(conn->remote_ip, ANX_IP_PROTO_TCP, pkt, total);
}

void anx_tcp_recv_segment(const void *data, uint32_t len, uint32_t src_ip)
{
	const struct anx_tcp_hdr *tcp = (const struct anx_tcp_hdr *)data;
	struct anx_tcp_conn *conn;
	uint16_t src_port, dst_port;
	uint32_t seq, ack_num;
	uint8_t data_off;
	const uint8_t *payload;
	uint32_t payload_len;

	if (len < sizeof(struct anx_tcp_hdr))
		return;

	src_port = anx_ntohs(tcp->src_port);
	dst_port = anx_ntohs(tcp->dst_port);
	seq = anx_ntohl(tcp->seq);
	ack_num = anx_ntohl(tcp->ack);
	data_off = (tcp->data_off >> 4) * 4;

	if (data_off > len)
		return;

	payload = (const uint8_t *)data + data_off;
	payload_len = len - data_off;

	conn = tcp_find_conn(src_ip, src_port, dst_port);
	if (!conn)
		return;

	switch (conn->state) {
	case ANX_TCP_SYN_SENT:
		if ((tcp->flags & (ANX_TCP_SYN | ANX_TCP_ACK)) ==
		    (ANX_TCP_SYN | ANX_TCP_ACK)) {
			if (ack_num == conn->snd_nxt + 1) {
				conn->snd_una = ack_num;
				conn->snd_nxt = ack_num;
				conn->rcv_nxt = seq + 1;
				conn->state = ANX_TCP_ESTABLISHED;
				/* Send ACK */
				tcp_send_segment(conn, ANX_TCP_ACK, NULL, 0);
			}
		} else if (tcp->flags & ANX_TCP_RST) {
			conn->state = ANX_TCP_CLOSED;
			conn->in_use = false;
		}
		break;

	case ANX_TCP_ESTABLISHED:
		if (tcp->flags & ANX_TCP_RST) {
			conn->state = ANX_TCP_CLOSED;
			conn->in_use = false;
			return;
		}

		/* Process ACK */
		if (tcp->flags & ANX_TCP_ACK)
			conn->snd_una = ack_num;

		/* Process incoming data */
		if (payload_len > 0 && seq == conn->rcv_nxt) {
			uint32_t space = conn->rx_cap - conn->rx_len;
			uint32_t copy = payload_len;

			if (copy > space)
				copy = space;
			if (copy > 0 && conn->rx_buf) {
				anx_memcpy(conn->rx_buf + conn->rx_len,
					   payload, copy);
				conn->rx_len += copy;
			}
			conn->rcv_nxt += payload_len;
			/* ACK received data */
			tcp_send_segment(conn, ANX_TCP_ACK, NULL, 0);
		}

		/* Handle FIN */
		if (tcp->flags & ANX_TCP_FIN) {
			conn->rcv_nxt++;
			conn->state = ANX_TCP_CLOSE_WAIT;
			tcp_send_segment(conn, ANX_TCP_ACK, NULL, 0);
		}
		break;

	case ANX_TCP_FIN_WAIT_1:
		if (tcp->flags & ANX_TCP_ACK) {
			conn->snd_una = ack_num;
			if (tcp->flags & ANX_TCP_FIN) {
				conn->rcv_nxt = seq + 1;
				conn->state = ANX_TCP_TIME_WAIT;
				tcp_send_segment(conn, ANX_TCP_ACK, NULL, 0);
			} else {
				conn->state = ANX_TCP_FIN_WAIT_2;
			}
		}
		/* Process data even during close */
		if (payload_len > 0 && seq == conn->rcv_nxt) {
			uint32_t space = conn->rx_cap - conn->rx_len;
			uint32_t copy = payload_len;

			if (copy > space)
				copy = space;
			if (copy > 0 && conn->rx_buf) {
				anx_memcpy(conn->rx_buf + conn->rx_len,
					   payload, copy);
				conn->rx_len += copy;
			}
			conn->rcv_nxt += payload_len;
		}
		break;

	case ANX_TCP_FIN_WAIT_2:
		if (tcp->flags & ANX_TCP_FIN) {
			conn->rcv_nxt = seq + 1;
			conn->state = ANX_TCP_TIME_WAIT;
			tcp_send_segment(conn, ANX_TCP_ACK, NULL, 0);
		}
		break;

	case ANX_TCP_LAST_ACK:
		if (tcp->flags & ANX_TCP_ACK) {
			conn->state = ANX_TCP_CLOSED;
			conn->in_use = false;
		}
		break;

	default:
		break;
	}
}

void anx_tcp_tick(void)
{
	/* Retransmit not yet implemented — placeholder */
}

int anx_tcp_connect(uint32_t dst_ip, uint16_t dst_port,
		     struct anx_tcp_conn **out)
{
	struct anx_tcp_conn *conn;
	uint64_t start;

	conn = tcp_alloc_conn();
	if (!conn)
		return ANX_ENOMEM;

	conn->local_ip = anx_ipv4_local_ip();
	conn->remote_ip = dst_ip;
	conn->local_port = next_ephemeral++;
	conn->remote_port = dst_port;
	conn->snd_nxt = (uint32_t)(arch_time_now() & 0xFFFFFFFF);
	conn->snd_una = conn->snd_nxt;
	conn->rcv_nxt = 0;
	conn->rcv_wnd = TCP_RX_BUF_SIZE;

	conn->rx_buf = anx_alloc(TCP_RX_BUF_SIZE);
	if (!conn->rx_buf) {
		conn->in_use = false;
		return ANX_ENOMEM;
	}
	conn->rx_len = 0;
	conn->rx_cap = TCP_RX_BUF_SIZE;

	/* Send SYN */
	conn->state = ANX_TCP_SYN_SENT;
	tcp_send_segment(conn, ANX_TCP_SYN, NULL, 0);

	/* Wait for SYN+ACK */
	start = arch_timer_ticks();
	while (conn->state == ANX_TCP_SYN_SENT &&
	       arch_timer_ticks() - start < 500) {
		anx_net_poll();
	}

	if (conn->state != ANX_TCP_ESTABLISHED) {
		if (conn->rx_buf)
			anx_free(conn->rx_buf);
		conn->in_use = false;
		return ANX_ETIMEDOUT;
	}

	*out = conn;
	return ANX_OK;
}

int anx_tcp_send(struct anx_tcp_conn *conn, const void *data, uint32_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t remaining = len;

	if (conn->state != ANX_TCP_ESTABLISHED)
		return ANX_EIO;

	while (remaining > 0) {
		uint32_t chunk = remaining;
		int ret;

		if (chunk > 1400)
			chunk = 1400;	/* stay under MTU */

		ret = tcp_send_segment(conn, ANX_TCP_ACK | ANX_TCP_PSH,
				       p, chunk);
		if (ret != ANX_OK)
			return ret;

		conn->snd_nxt += chunk;
		p += chunk;
		remaining -= chunk;

		/* Poll for ACKs between chunks */
		anx_net_poll();
	}

	return ANX_OK;
}

int anx_tcp_recv(struct anx_tcp_conn *conn, void *buf, uint32_t len,
		  uint32_t timeout_ms)
{
	uint64_t start, timeout_ticks;
	uint32_t copied;

	if (conn->state != ANX_TCP_ESTABLISHED &&
	    conn->state != ANX_TCP_CLOSE_WAIT &&
	    conn->rx_len == 0)
		return ANX_EIO;

	timeout_ticks = ((uint64_t)timeout_ms * 100) / 1000;
	start = arch_timer_ticks();

	/* Poll until we have data or timeout */
	while (conn->rx_len == 0) {
		if (conn->state == ANX_TCP_CLOSE_WAIT ||
		    conn->state == ANX_TCP_CLOSED)
			return 0;	/* EOF */
		if (arch_timer_ticks() - start >= timeout_ticks)
			return ANX_ETIMEDOUT;
		anx_net_poll();
	}

	/* Copy available data to caller */
	copied = conn->rx_len;
	if (copied > len)
		copied = len;
	anx_memcpy(buf, conn->rx_buf, copied);

	/* Shift remaining data forward */
	if (copied < conn->rx_len) {
		anx_memmove(conn->rx_buf, conn->rx_buf + copied,
			    conn->rx_len - copied);
	}
	conn->rx_len -= copied;

	return (int)copied;
}

int anx_tcp_close(struct anx_tcp_conn *conn)
{
	uint64_t start;

	if (conn->state == ANX_TCP_ESTABLISHED) {
		conn->state = ANX_TCP_FIN_WAIT_1;
		tcp_send_segment(conn, ANX_TCP_FIN | ANX_TCP_ACK, NULL, 0);
		conn->snd_nxt++;
	} else if (conn->state == ANX_TCP_CLOSE_WAIT) {
		conn->state = ANX_TCP_LAST_ACK;
		tcp_send_segment(conn, ANX_TCP_FIN | ANX_TCP_ACK, NULL, 0);
		conn->snd_nxt++;
	}

	/* Wait briefly for final ACK */
	start = arch_timer_ticks();
	while (conn->state != ANX_TCP_CLOSED &&
	       conn->state != ANX_TCP_TIME_WAIT &&
	       arch_timer_ticks() - start < 200) {
		anx_net_poll();
	}

	if (conn->rx_buf) {
		anx_free(conn->rx_buf);
		conn->rx_buf = NULL;
	}
	conn->in_use = false;
	conn->state = ANX_TCP_CLOSED;

	return ANX_OK;
}
