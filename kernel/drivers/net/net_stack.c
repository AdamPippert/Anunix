/*
 * net_stack.c — Network stack initialization.
 *
 * Wires up ARP, IPv4, ICMP and configures the stack with
 * a static IP configuration (QEMU user-mode defaults).
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/kprintf.h>

void anx_net_stack_init(const struct anx_net_config *cfg)
{
	anx_arp_init();
	anx_arp_set_ip(cfg->ip);
	anx_ipv4_init(cfg);
	anx_udp_init();
	anx_tcp_init();
	anx_dns_init();

	kprintf("net: ip %u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u\n",
		(cfg->ip >> 24) & 0xFF, (cfg->ip >> 16) & 0xFF,
		(cfg->ip >> 8) & 0xFF, cfg->ip & 0xFF,
		(cfg->gateway >> 24) & 0xFF, (cfg->gateway >> 16) & 0xFF,
		(cfg->gateway >> 8) & 0xFF, cfg->gateway & 0xFF,
		(cfg->dns >> 24) & 0xFF, (cfg->dns >> 16) & 0xFF,
		(cfg->dns >> 8) & 0xFF, cfg->dns & 0xFF);
}
