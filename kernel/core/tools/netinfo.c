/*
 * netinfo.c — Network configuration display.
 *
 * Shows IP configuration from the network stack.
 *
 * USAGE
 *   netinfo              Show all network interfaces
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/net.h>
#include <anx/virtio_net.h>
#include <anx/kprintf.h>

void cmd_netinfo(int argc, char **argv)
{
	uint8_t mac[6];
	uint32_t ip;

	(void)argc;
	(void)argv;

	kprintf("\n=== Network Configuration ===\n\n");

	if (!anx_virtio_net_ready()) {
		kprintf("  No network interface detected\n\n");
		return;
	}

	anx_virtio_net_mac(mac);
	ip = anx_ipv4_local_ip();

	kprintf("  Interface:  virtio-net0\n");
	kprintf("  MAC:        %x:%x:%x:%x:%x:%x\n",
		(uint32_t)mac[0], (uint32_t)mac[1],
		(uint32_t)mac[2], (uint32_t)mac[3],
		(uint32_t)mac[4], (uint32_t)mac[5]);
	kprintf("  IPv4:       %u.%u.%u.%u\n",
		(ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
		(ip >> 8) & 0xFF, ip & 0xFF);
	kprintf("  DNS:        %u.%u.%u.%u\n",
		(anx_ipv4_dns() >> 24) & 0xFF,
		(anx_ipv4_dns() >> 16) & 0xFF,
		(anx_ipv4_dns() >> 8) & 0xFF,
		anx_ipv4_dns() & 0xFF);
	kprintf("  Status:     up\n\n");
}
