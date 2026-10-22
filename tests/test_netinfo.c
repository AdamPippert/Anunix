/* netinfo output across links and address sources. */
#include <anx/types.h>
#include <anx/net.h>
#include <anx/tools.h>
#include <anx/string.h>
#include <anx/kprintf.h>

#define CHECK(c) do { if (!(c)) { kprintf("netinfo failed at %u\n", __LINE__); return -1; } } while (0)

void mock_eth_set_link(const char *name, const uint8_t mac[6]);

static char out[1024];

static const char *run_netinfo(void)
{
	char *argv[] = { "netinfo", NULL };
	uint32_t n;

	anx_kprintf_capture_start(out, sizeof(out));
	cmd_netinfo(1, argv);
	n = anx_kprintf_capture_stop();
	out[n < sizeof(out) ? n : sizeof(out) - 1] = '\0';
	return out;
}

int test_netinfo(void)
{
	static const uint8_t vm_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
	static const uint8_t wifi_mac[6] = { 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
	struct anx_net_config none = { 0, 0, 0, 0 };
	struct anx_net_config qemu = {
		ANX_IP4(10, 0, 2, 15), ANX_IP4(255, 255, 255, 0),
		ANX_IP4(10, 0, 2, 2), ANX_IP4(10, 0, 2, 3),
	};
	struct anx_net_config lan = {
		ANX_IP4(192, 168, 0, 123), ANX_IP4(255, 255, 255, 0),
		ANX_IP4(192, 168, 0, 1), ANX_IP4(192, 168, 0, 1),
	};
	const char *s;

	/* No link at all. */
	mock_eth_set_link(NULL, NULL);
	anx_net_configure(&none, ANX_NET_SRC_NONE);
	s = run_netinfo();
	CHECK(anx_strstr(s, "No network interface is up"));
	CHECK(!anx_strstr(s, "IPv4:"));

	/* VM: the boot fallback must be distinguishable from a lease. */
	mock_eth_set_link("virtio-net0", vm_mac);
	anx_net_configure(&qemu, ANX_NET_SRC_FALLBACK);
	s = run_netinfo();
	CHECK(anx_strstr(s, "Interface:  virtio-net0"));
	CHECK(anx_strstr(s, "MAC:        52:54:00:12:34:56"));
	CHECK(anx_strstr(s, "IPv4:       10.0.2.15"));
	CHECK(anx_strstr(s, "Gateway:    10.0.2.2"));
	CHECK(anx_strstr(s, "Source:     QEMU user-net fallback"));
	CHECK(anx_strstr(s, "Status:     up"));

	anx_net_configure(&qemu, ANX_NET_SRC_DHCP);
	s = run_netinfo();
	CHECK(anx_strstr(s, "Source:     DHCP lease"));

	/* Bare metal: Wi-Fi associated, no lease yet. */
	mock_eth_set_link("wlan0 (mt7925)", wifi_mac);
	anx_net_configure(&none, ANX_NET_SRC_DHCP);
	CHECK(anx_net_source() == ANX_NET_SRC_NONE);
	s = run_netinfo();
	CHECK(anx_strstr(s, "Interface:  wlan0 (mt7925)"));
	CHECK(anx_strstr(s, "MAC:        0a:0b:0c:0d:0e:0f"));
	CHECK(anx_strstr(s, "IPv4:       none"));
	CHECK(anx_strstr(s, "run 'net dhcp'"));
	CHECK(!anx_strstr(s, "Status:     up"));

	/* Bare metal: Wi-Fi with a LAN lease. */
	anx_net_configure(&lan, ANX_NET_SRC_DHCP);
	s = run_netinfo();
	CHECK(anx_strstr(s, "IPv4:       192.168.0.123"));
	CHECK(anx_strstr(s, "Netmask:    255.255.255.0"));
	CHECK(anx_strstr(s, "Gateway:    192.168.0.1"));
	CHECK(anx_strstr(s, "Source:     DHCP lease"));

	anx_net_configure(&lan, ANX_NET_SRC_STATIC);
	s = run_netinfo();
	CHECK(anx_strstr(s, "Source:     static (config)"));

	mock_eth_set_link(NULL, NULL);
	anx_net_configure(&none, ANX_NET_SRC_NONE);
	return 0;
}
