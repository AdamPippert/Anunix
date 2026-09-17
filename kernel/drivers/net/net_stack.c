/*
 * net_stack.c — Network stack initialization.
 *
 * Wires up ARP, IPv4, UDP, TCP and DNS, and changes the addresses of a
 * running stack when a lease arrives after boot (a Wi-Fi association
 * made from the shell, for instance).
 */

#include <anx/types.h>
#include <anx/net.h>
#include <anx/kprintf.h>
#include <anx/mt7925.h>

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

void anx_net_configure(const struct anx_net_config *cfg)
{
	anx_arp_set_ip(cfg->ip);
	anx_ipv4_init(cfg);
}

int anx_net_dhcp(void)
{
	struct anx_net_config cfg;
	int ret;

	if (!anx_eth_ready()) {
		kprintf("net: no interface is up\n");
		return ANX_EIO;
	}
	kprintf("dhcp: discovering on %s...\n", anx_eth_name());
	ret = anx_dhcp_discover(&cfg);
	if (ret != ANX_OK) {
		kprintf("dhcp: no lease (%d)\n", ret);
		if (anx_mt7925_ready())
			anx_mt7925_info();
		return ret;
	}
	anx_net_configure(&cfg);
	return ANX_OK;
}
