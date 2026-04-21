/*
 * mock_wifi.c — Stub WiFi driver symbols for host-native test builds.
 *
 * The MT7925 driver uses hardware I/O not available on the host.
 */

#include <anx/types.h>
#include <anx/mt7925.h>

int  anx_mt7925_init(void) { return 0; }
bool anx_mt7925_ready(void) { return false; }
int  anx_mt7925_tx(const void *f, uint16_t l) { (void)f; (void)l; return -1; }
void anx_mt7925_poll(void) {}
void anx_mt7925_info(void) {}
int  anx_mt7925_connect(const char *s, const char *p) { (void)s; (void)p; return -1; }
void anx_mt7925_disconnect(void) {}

static const uint8_t zero_mac[6];
const uint8_t *anx_mt7925_mac(void) { return zero_mac; }

/* cmd_wifi stub (shell tool excluded from TEST_CORE, but referenced by shell.c) */
void cmd_wifi(int argc, char **argv) { (void)argc; (void)argv; }
