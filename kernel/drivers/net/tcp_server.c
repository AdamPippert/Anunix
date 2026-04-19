/*
 * tcp_server.c — TCP server (listen/accept) extension.
 *
 * Adds passive-open support to the TCP stack. A port registered
 * with anx_tcp_listen() will accept incoming SYN packets, complete
 * the three-way handshake, and invoke a user-provided callback
 * with the new connection.
 *
 * Single-connection, synchronous — adequate for the HTTP API server.
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/arch.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Mirrors the connection struct in tcp.c — keep in sync */
enum anx_tcp_state {
	ANX_TCP_CLOSED,
	ANX_TCP_SYN_SENT,
	ANX_TCP_ESTABLISHED,
	ANX_TCP_FIN_WAIT_1,
	ANX_TCP_FIN_WAIT_2,
	ANX_TCP_TIME_WAIT,
	ANX_TCP_CLOSE_WAIT,
	ANX_TCP_LAST_ACK,
	ANX_TCP_LISTEN,
	ANX_TCP_SYN_RCVD,
};

struct anx_tcp_conn {
	enum anx_tcp_state state;
	uint32_t local_ip, remote_ip;
	uint16_t local_port, remote_port;
	uint32_t snd_nxt;
	uint32_t snd_una;
	uint32_t rcv_nxt;
	uint16_t rcv_wnd;
	uint8_t *rx_buf;
	uint32_t rx_len;
	uint32_t rx_cap;
	uint64_t last_send_tick;
	uint8_t retries;
	bool in_use;
};

#define TCP_SRV_MAX_LISTENERS	4
#define TCP_SRV_MAX_CONNS	4
#define TCP_SRV_RX_BUF_SIZE	8192

typedef void (*anx_tcp_accept_fn)(struct anx_tcp_conn *conn, void *arg);

struct tcp_listener {
	uint16_t port;
	anx_tcp_accept_fn callback;
	void *arg;
	bool active;
};

static struct tcp_listener listeners[TCP_SRV_MAX_LISTENERS];
static struct anx_tcp_conn srv_conns[TCP_SRV_MAX_CONNS];

/* TCP pseudo-header checksum — same as in tcp.c */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
			      const void *tcp_pkt, uint32_t tcp_len)
{
	uint32_t sum = 0;
	const uint16_t *p;
	uint32_t len;

	sum += (src_ip >> 16) & 0xFFFF;
	sum += src_ip & 0xFFFF;
	sum += (dst_ip >> 16) & 0xFFFF;
	sum += dst_ip & 0xFFFF;
	sum += anx_htons(ANX_IP_PROTO_TCP);
	sum += anx_htons((uint16_t)tcp_len);

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

static int tcp_srv_send_segment(struct anx_tcp_conn *conn, uint8_t flags,
				 const void *data, uint32_t data_len)
{
	uint8_t *pkt;
	struct anx_tcp_hdr *tcp;
	uint32_t total = sizeof(struct anx_tcp_hdr) + data_len;
	int ret;

	pkt = anx_alloc(total);
	if (!pkt)
		return ANX_ENOMEM;

	tcp = (struct anx_tcp_hdr *)pkt;
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
	ret = anx_ipv4_send(conn->remote_ip, ANX_IP_PROTO_TCP, pkt, total);
	anx_free(pkt);
	return ret;
}

static struct tcp_listener *find_listener(uint16_t port)
{
	int i;

	for (i = 0; i < TCP_SRV_MAX_LISTENERS; i++) {
		if (listeners[i].active && listeners[i].port == port)
			return &listeners[i];
	}
	return NULL;
}

static struct anx_tcp_conn *srv_find_conn(uint32_t remote_ip,
					   uint16_t remote_port,
					   uint16_t local_port)
{
	int i;

	for (i = 0; i < TCP_SRV_MAX_CONNS; i++) {
		if (srv_conns[i].in_use &&
		    srv_conns[i].remote_ip == remote_ip &&
		    srv_conns[i].remote_port == remote_port &&
		    srv_conns[i].local_port == local_port)
			return &srv_conns[i];
	}
	return NULL;
}

static struct anx_tcp_conn *srv_alloc_conn(void)
{
	int i;

	for (i = 0; i < TCP_SRV_MAX_CONNS; i++) {
		if (!srv_conns[i].in_use) {
			anx_memset(&srv_conns[i], 0, sizeof(srv_conns[i]));
			srv_conns[i].in_use = true;
			return &srv_conns[i];
		}
	}
	return NULL;
}

int anx_tcp_listen(uint16_t port, anx_tcp_accept_fn cb, void *arg)
{
	int i;

	if (find_listener(port))
		return ANX_EEXIST;

	for (i = 0; i < TCP_SRV_MAX_LISTENERS; i++) {
		if (!listeners[i].active) {
			listeners[i].port = port;
			listeners[i].callback = cb;
			listeners[i].arg = arg;
			listeners[i].active = true;
			kprintf("tcp: listening on port %u\n",
				(uint32_t)port);
			return ANX_OK;
		}
	}
	return ANX_EFULL;
}

void anx_tcp_unlisten(uint16_t port)
{
	int i;

	for (i = 0; i < TCP_SRV_MAX_LISTENERS; i++) {
		if (listeners[i].active && listeners[i].port == port) {
			listeners[i].active = false;
			break;
		}
	}
}

/*
 * Called from the IPv4 receive path for incoming TCP segments
 * destined to a listening port. Returns true if handled.
 */
bool anx_tcp_srv_input(const void *data, uint32_t len, uint32_t src_ip)
{
	const struct anx_tcp_hdr *tcp = (const struct anx_tcp_hdr *)data;
	struct tcp_listener *lsn;
	struct anx_tcp_conn *conn;
	uint16_t src_port, dst_port;
	uint32_t seq, ack_num;
	uint8_t data_off;
	const uint8_t *payload;
	uint32_t payload_len;

	if (len < sizeof(struct anx_tcp_hdr))
		return false;

	src_port = anx_ntohs(tcp->src_port);
	dst_port = anx_ntohs(tcp->dst_port);
	seq = anx_ntohl(tcp->seq);
	ack_num = anx_ntohl(tcp->ack);
	data_off = (tcp->data_off >> 4) * 4;

	if (data_off > len)
		return false;

	payload = (const uint8_t *)data + data_off;
	payload_len = len - data_off;

	/* Check if this is for an existing server connection */
	conn = srv_find_conn(src_ip, src_port, dst_port);
	if (conn) {
		switch (conn->state) {
		case ANX_TCP_SYN_RCVD:
			/* Waiting for ACK to complete handshake */
			if (tcp->flags & ANX_TCP_ACK) {
				conn->snd_una = ack_num;
				conn->state = ANX_TCP_ESTABLISHED;
				/* Invoke the accept callback */
				lsn = find_listener(dst_port);
				if (lsn && lsn->callback)
					lsn->callback(conn, lsn->arg);
			}
			return true;

		case ANX_TCP_ESTABLISHED:
			if (tcp->flags & ANX_TCP_RST) {
				conn->state = ANX_TCP_CLOSED;
				if (conn->rx_buf) {
					anx_free(conn->rx_buf);
					conn->rx_buf = NULL;
				}
				conn->in_use = false;
				return true;
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
				tcp_srv_send_segment(conn, ANX_TCP_ACK,
						     NULL, 0);
			}

			/* Handle FIN */
			if (tcp->flags & ANX_TCP_FIN) {
				conn->rcv_nxt++;
				conn->state = ANX_TCP_CLOSE_WAIT;
				tcp_srv_send_segment(conn, ANX_TCP_ACK,
						     NULL, 0);
			}
			return true;

		case ANX_TCP_FIN_WAIT_1:
			if (tcp->flags & ANX_TCP_ACK) {
				conn->snd_una = ack_num;
				if (tcp->flags & ANX_TCP_FIN) {
					conn->rcv_nxt = seq + 1;
					conn->state = ANX_TCP_TIME_WAIT;
					tcp_srv_send_segment(conn, ANX_TCP_ACK,
							     NULL, 0);
				} else {
					conn->state = ANX_TCP_FIN_WAIT_2;
				}
			}
			return true;

		case ANX_TCP_FIN_WAIT_2:
			if (tcp->flags & ANX_TCP_FIN) {
				conn->rcv_nxt = seq + 1;
				conn->state = ANX_TCP_TIME_WAIT;
				tcp_srv_send_segment(conn, ANX_TCP_ACK,
						     NULL, 0);
			}
			return true;

		case ANX_TCP_LAST_ACK:
			if (tcp->flags & ANX_TCP_ACK) {
				conn->state = ANX_TCP_CLOSED;
				if (conn->rx_buf) {
					anx_free(conn->rx_buf);
					conn->rx_buf = NULL;
				}
				conn->in_use = false;
			}
			return true;

		default:
			return true;
		}
	}

	/* No existing connection — check for SYN on a listening port */
	lsn = find_listener(dst_port);
	if (!lsn)
		return false;

	if (!(tcp->flags & ANX_TCP_SYN) || (tcp->flags & ANX_TCP_ACK))
		return true;	/* not a new connection attempt */

	/* Allocate a new server connection */
	conn = srv_alloc_conn();
	if (!conn) {
		/* No slots — send RST */
		return true;
	}

	conn->local_ip = anx_ipv4_local_ip();
	conn->remote_ip = src_ip;
	conn->local_port = dst_port;
	conn->remote_port = src_port;
	conn->snd_nxt = (uint32_t)(arch_time_now() & 0xFFFFFFFF);
	conn->snd_una = conn->snd_nxt;
	conn->rcv_nxt = seq + 1;
	conn->rcv_wnd = TCP_SRV_RX_BUF_SIZE;

	conn->rx_buf = anx_alloc(TCP_SRV_RX_BUF_SIZE);
	if (!conn->rx_buf) {
		conn->in_use = false;
		return true;
	}
	conn->rx_len = 0;
	conn->rx_cap = TCP_SRV_RX_BUF_SIZE;

	/* Send SYN+ACK */
	conn->state = ANX_TCP_SYN_RCVD;
	tcp_srv_send_segment(conn, ANX_TCP_SYN | ANX_TCP_ACK, NULL, 0);
	conn->snd_nxt++;

	return true;
}

int anx_tcp_srv_send(struct anx_tcp_conn *conn, const void *data,
		      uint32_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t remaining = len;

	if (conn->state != ANX_TCP_ESTABLISHED)
		return ANX_EIO;

	while (remaining > 0) {
		uint32_t chunk = remaining;
		int ret;

		if (chunk > 1400)
			chunk = 1400;

		ret = tcp_srv_send_segment(conn,
					   ANX_TCP_ACK | ANX_TCP_PSH,
					   p, chunk);
		if (ret != ANX_OK)
			return ret;

		conn->snd_nxt += chunk;
		p += chunk;
		remaining -= chunk;

		anx_net_poll();
	}

	return ANX_OK;
}

int anx_tcp_srv_recv(struct anx_tcp_conn *conn, void *buf, uint32_t len,
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

	while (conn->rx_len == 0) {
		if (conn->state == ANX_TCP_CLOSE_WAIT ||
		    conn->state == ANX_TCP_CLOSED)
			return 0;
		if (arch_timer_ticks() - start >= timeout_ticks)
			return ANX_ETIMEDOUT;
		anx_net_poll();
	}

	copied = conn->rx_len;
	if (copied > len)
		copied = len;
	anx_memcpy(buf, conn->rx_buf, copied);

	if (copied < conn->rx_len) {
		anx_memmove(conn->rx_buf, conn->rx_buf + copied,
			    conn->rx_len - copied);
	}
	conn->rx_len -= copied;

	return (int)copied;
}

int anx_tcp_srv_close(struct anx_tcp_conn *conn)
{
	uint64_t start;

	/* Wait up to 1s for all sent data to be ACKed before sending FIN.
	 * Ensures the client has received all TCP segments, so FIN only
	 * appears after data in the byte stream. */
	start = arch_timer_ticks();
	while (conn->snd_una != conn->snd_nxt &&
	       conn->state == ANX_TCP_ESTABLISHED &&
	       arch_timer_ticks() - start < 100) {
		anx_net_poll();
	}

	if (conn->state == ANX_TCP_ESTABLISHED) {
		conn->state = ANX_TCP_FIN_WAIT_1;
		tcp_srv_send_segment(conn, ANX_TCP_FIN | ANX_TCP_ACK, NULL, 0);
		conn->snd_nxt++;
	} else if (conn->state == ANX_TCP_CLOSE_WAIT) {
		conn->state = ANX_TCP_LAST_ACK;
		tcp_srv_send_segment(conn, ANX_TCP_FIN | ANX_TCP_ACK, NULL, 0);
		conn->snd_nxt++;
	}

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

	anx_memset(conn, 0, sizeof(*conn));

	anx_net_poll();
	anx_net_poll();

	return ANX_OK;
}
