/*
 * anx/net.h — Network protocol structures and stack API.
 *
 * Defines packet headers for Ethernet, ARP, IPv4, ICMP, UDP, and TCP,
 * plus byte-order helpers and the network stack configuration.
 */

#ifndef ANX_NET_H
#define ANX_NET_H

#include <anx/types.h>

/* --- Byte-order conversion (x86_64 is little-endian) --- */

static inline uint16_t anx_htons(uint16_t v)
{
	return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t anx_ntohs(uint16_t v)
{
	return anx_htons(v);
}

static inline uint32_t anx_htonl(uint32_t v)
{
	return ((v >> 24) & 0x000000FF)
	     | ((v >>  8) & 0x0000FF00)
	     | ((v <<  8) & 0x00FF0000)
	     | ((v << 24) & 0xFF000000);
}

static inline uint32_t anx_ntohl(uint32_t v)
{
	return anx_htonl(v);
}

/* Build a 32-bit IPv4 address from four octets */
#define ANX_IP4(a, b, c, d) \
	(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
	 ((uint32_t)(c) << 8)  |  (uint32_t)(d))

/* --- Ethernet --- */

#define ANX_ETH_ALEN		6
#define ANX_ETH_HLEN		14
#define ANX_ETH_MTU		1500
#define ANX_ETH_FRAME_MAX	(ANX_ETH_HLEN + ANX_ETH_MTU)

#define ANX_ETH_P_IP		0x0800
#define ANX_ETH_P_ARP		0x0806

struct anx_eth_hdr {
	uint8_t dst[ANX_ETH_ALEN];
	uint8_t src[ANX_ETH_ALEN];
	uint16_t ethertype;		/* big-endian */
} __attribute__((packed));

/* --- ARP --- */

#define ANX_ARP_HW_ETHER	1
#define ANX_ARP_OP_REQUEST	1
#define ANX_ARP_OP_REPLY	2

struct anx_arp_pkt {
	uint16_t hw_type;		/* ANX_ARP_HW_ETHER */
	uint16_t proto_type;		/* ANX_ETH_P_IP */
	uint8_t hw_len;			/* 6 */
	uint8_t proto_len;		/* 4 */
	uint16_t opcode;		/* request / reply */
	uint8_t sender_mac[ANX_ETH_ALEN];
	uint32_t sender_ip;		/* big-endian */
	uint8_t target_mac[ANX_ETH_ALEN];
	uint32_t target_ip;		/* big-endian */
} __attribute__((packed));

/* --- IPv4 --- */

#define ANX_IP_PROTO_ICMP	1
#define ANX_IP_PROTO_TCP	6
#define ANX_IP_PROTO_UDP	17

struct anx_ipv4_hdr {
	uint8_t ver_ihl;		/* version (4) | IHL (5) */
	uint8_t tos;
	uint16_t total_len;		/* big-endian */
	uint16_t id;			/* big-endian */
	uint16_t frag_off;		/* big-endian */
	uint8_t ttl;
	uint8_t protocol;
	uint16_t checksum;		/* big-endian */
	uint32_t src_ip;		/* big-endian */
	uint32_t dst_ip;		/* big-endian */
} __attribute__((packed));

/* --- ICMP --- */

#define ANX_ICMP_ECHO_REPLY	0
#define ANX_ICMP_ECHO_REQUEST	8

struct anx_icmp_hdr {
	uint8_t type;
	uint8_t code;
	uint16_t checksum;		/* big-endian */
	uint16_t id;			/* big-endian */
	uint16_t seq;			/* big-endian */
} __attribute__((packed));

/* --- UDP --- */

struct anx_udp_hdr {
	uint16_t src_port;		/* big-endian */
	uint16_t dst_port;		/* big-endian */
	uint16_t length;		/* big-endian */
	uint16_t checksum;		/* big-endian */
} __attribute__((packed));

/* --- TCP --- */

struct anx_tcp_hdr {
	uint16_t src_port;		/* big-endian */
	uint16_t dst_port;		/* big-endian */
	uint32_t seq;			/* big-endian */
	uint32_t ack;			/* big-endian */
	uint8_t data_off;		/* upper 4 bits = offset in 32-bit words */
	uint8_t flags;
	uint16_t window;		/* big-endian */
	uint16_t checksum;		/* big-endian */
	uint16_t urgent;		/* big-endian */
} __attribute__((packed));

#define ANX_TCP_FIN	0x01
#define ANX_TCP_SYN	0x02
#define ANX_TCP_RST	0x04
#define ANX_TCP_PSH	0x08
#define ANX_TCP_ACK	0x10

/* --- Network configuration (hardcoded for QEMU user-mode) --- */

struct anx_net_config {
	uint32_t ip;			/* host byte order */
	uint32_t netmask;
	uint32_t gateway;
	uint32_t dns;
};

/* --- Stack-wide API --- */

/* Initialize the full network stack with the given config */
void anx_net_stack_init(const struct anx_net_config *cfg);

/* Ethernet layer */
void anx_eth_recv(const void *frame, uint32_t len);
int anx_eth_send(const uint8_t dst[6], uint16_t ethertype,
		 const void *payload, uint32_t len);

/* ARP layer */
void anx_arp_init(void);
void anx_arp_recv(const void *data, uint32_t len);
int anx_arp_resolve(uint32_t ip, uint8_t mac_out[6]);

/* IPv4 layer */
void anx_ipv4_init(const struct anx_net_config *cfg);
void anx_ipv4_recv(const void *data, uint32_t len);
int anx_ipv4_send(uint32_t dst_ip, uint8_t proto,
		   const void *payload, uint32_t len);
uint16_t anx_ip_checksum(const void *data, uint32_t len);

/* ICMP layer */
void anx_icmp_recv(const void *data, uint32_t len, uint32_t src_ip);
int anx_icmp_ping(uint32_t dst_ip, uint16_t seq);

/* Set the local IP for ARP replies (called by stack init) */
void anx_arp_set_ip(uint32_t ip);

/* Get the local IP address (host byte order) */
uint32_t anx_ipv4_local_ip(void);

/* Get the configured DNS server (host byte order) */
uint32_t anx_ipv4_dns(void);

/* UDP layer */
typedef void (*anx_udp_recv_fn)(const void *data, uint32_t len,
				uint32_t src_ip, uint16_t src_port,
				void *arg);
void anx_udp_init(void);
void anx_udp_recv(const void *data, uint32_t len, uint32_t src_ip);
int anx_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
		  const void *data, uint32_t len);
int anx_udp_bind(uint16_t port, anx_udp_recv_fn handler, void *arg);
void anx_udp_unbind(uint16_t port);

/* DNS resolver */
void anx_dns_init(void);
int anx_dns_resolve(const char *hostname, uint32_t *ip_out);

/* TCP layer */
struct anx_tcp_conn;
void anx_tcp_init(void);
void anx_tcp_recv_segment(const void *data, uint32_t len, uint32_t src_ip);
void anx_tcp_tick(void);
int anx_tcp_connect(uint32_t dst_ip, uint16_t dst_port,
		     struct anx_tcp_conn **out);
int anx_tcp_send(struct anx_tcp_conn *conn, const void *data, uint32_t len);
int anx_tcp_recv(struct anx_tcp_conn *conn, void *buf, uint32_t len,
		  uint32_t timeout_ms);
int anx_tcp_close(struct anx_tcp_conn *conn);

/* TCP server (passive open) */
typedef void (*anx_tcp_accept_fn)(struct anx_tcp_conn *conn, void *arg);
int anx_tcp_listen(uint16_t port, anx_tcp_accept_fn cb, void *arg);
void anx_tcp_unlisten(uint16_t port);
bool anx_tcp_srv_input(const void *data, uint32_t len, uint32_t src_ip);
int anx_tcp_srv_send(struct anx_tcp_conn *conn, const void *data,
		      uint32_t len);
int anx_tcp_srv_recv(struct anx_tcp_conn *conn, void *buf, uint32_t len,
		      uint32_t timeout_ms);
int anx_tcp_srv_close(struct anx_tcp_conn *conn);

/* DHCP client */
int anx_dhcp_discover(struct anx_net_config *cfg);

/* NTP time sync */
int anx_ntp_sync(uint32_t server_ip);

/* Poll the network device and process any received packets */
void anx_net_poll(void);

#endif /* ANX_NET_H */
