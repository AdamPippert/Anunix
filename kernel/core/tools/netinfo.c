/*
 * netinfo.c — Network configuration display.
 *
 * Shows the active interface, its IPv4 configuration, and where the
 * address came from. Works on any link the Ethernet layer drives:
 * virtio-net and e1000 in a VM, MT7925 Wi-Fi on bare metal.
 *
 * USAGE
 *   netinfo              Show the active interface and address
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/net.h>
#include <anx/mt7925.h>
#include <anx/kprintf.h>

static void print_ipv4(const char *label, uint32_t ip)
{
	kprintf("  %s%u.%u.%u.%u\n", label,
		(ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
		(ip >> 8) & 0xFF, ip & 0xFF);
}

void cmd_netinfo(int argc, char **argv)
{
	struct anx_net_config cfg;
	uint8_t mac[6];

	(void)argc;
	(void)argv;

	kprintf("\n=== Network Configuration ===\n\n");

	if (!anx_eth_ready()) {
		kprintf("  No network interface is up\n");
		/* Wi-Fi firmware runs before association brings the link up. */
		if (anx_mt7925_state() >= MT7925_STATE_FW_UP)
			kprintf("  Wi-Fi:      not associated; run "
				"'wifi connect <ssid> [<psk>]'\n");
		kprintf("\n");
		return;
	}

	anx_eth_mac(mac);
	anx_ipv4_get_config(&cfg);

	kprintf("  Interface:  %s\n", anx_eth_name());
	kprintf("  MAC:        %02x:%02x:%02x:%02x:%02x:%02x\n",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	if (cfg.ip == 0) {
		kprintf("  IPv4:       none\n");
		kprintf("  Status:     no address; run 'net dhcp'\n\n");
		return;
	}

	print_ipv4("IPv4:       ", cfg.ip);
	print_ipv4("Netmask:    ", cfg.netmask);
	print_ipv4("Gateway:    ", cfg.gateway);
	print_ipv4("DNS:        ", cfg.dns);
	kprintf("  Source:     %s\n", anx_net_source_name(anx_net_source()));
	kprintf("  Status:     up\n\n");
}
