/*
 * wifi.c — WiFi management shell tool.
 *
 * Usage:
 *   wifi status               show driver and association state
 *   wifi connect <ssid>       connect to open network
 *   wifi connect <ssid> <psk> connect to WPA2 network
 *   wifi disconnect           disconnect from current network
 *   wifi mac                  print station MAC address
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/mt7925.h>
#include <anx/kprintf.h>
#include <anx/string.h>

void cmd_wifi(int argc, char **argv)
{
	if (argc < 2) {
		kprintf("usage: wifi <status|connect|disconnect|mac>\n");
		return;
	}

	if (anx_strcmp(argv[1], "status") == 0) {
		anx_mt7925_info();
		return;
	}

	if (anx_strcmp(argv[1], "mac") == 0) {
		const uint8_t *mac = anx_mt7925_mac();
		kprintf("%02x:%02x:%02x:%02x:%02x:%02x\n",
			mac[0], mac[1], mac[2],
			mac[3], mac[4], mac[5]);
		return;
	}

	if (anx_strcmp(argv[1], "disconnect") == 0) {
		anx_mt7925_disconnect();
		return;
	}

	if (anx_strcmp(argv[1], "connect") == 0) {
		if (argc < 3) {
			kprintf("usage: wifi connect <ssid> [<psk>]\n");
			return;
		}
		const char *ssid = argv[2];
		const char *psk  = (argc >= 4) ? argv[3] : NULL;

		kprintf("wifi: connecting to \"%s\"\n", ssid);
		int ret = anx_mt7925_connect(ssid, psk);
		if (ret != ANX_OK)
			kprintf("wifi: connect failed (%d)\n", ret);
		else
			kprintf("wifi: associated\n");
		return;
	}

	kprintf("wifi: unknown command '%s'\n", argv[1]);
}
