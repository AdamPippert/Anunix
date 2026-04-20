/*
 * anx/mt7925.h — MediaTek MT7925 (RZ717) Wi-Fi 7 driver.
 *
 * Supports PCIe device 14C3:0717, found in the Framework Laptop 16
 * AMD Ryzen AI 300 (jekyll).  No built-in Ethernet on that machine
 * so this driver is the sole network path for bare-metal Anunix.
 *
 * Driver works in polling mode — no MSI-X (IRQ 184 is above the
 * 8259 PIC range).  anx_mt7925_poll() must be called from the
 * main loop to drain RX frames and service MCU events.
 *
 * WiFi credentials are stored as State Objects:
 *   default:/wifi/ssid  — SSID (UTF-8 string)
 *   default:/wifi/psk   — WPA2 passphrase (UTF-8 string)
 * Set via: wifi connect <ssid> [<psk>]
 */

#ifndef ANX_MT7925_H
#define ANX_MT7925_H

#include <anx/types.h>

/* Probe PCI bus, download firmware, boot MCU, init DMA rings.
 * Returns ANX_OK if the device was found and firmware started. */
int anx_mt7925_init(void);

/* True if the driver is active and MCU is running. */
bool anx_mt7925_ready(void);

/* Transmit a raw Ethernet frame.  Returns ANX_OK or ANX_EBUSY. */
int anx_mt7925_tx(const void *frame, uint16_t len);

/* Poll for received frames.  Calls anx_eth_recv() for each.
 * Safe to call repeatedly from the shell loop (non-blocking). */
void anx_mt7925_poll(void);

/* Return the station MAC address (6 bytes). */
const uint8_t *anx_mt7925_mac(void);

/* Print driver status to the kernel console. */
void anx_mt7925_info(void);

/* Connect to a WiFi network.
 * ssid: null-terminated SSID (max 32 bytes).
 * psk:  WPA2 passphrase, or NULL for open network.
 * Returns ANX_OK when association is complete. */
int anx_mt7925_connect(const char *ssid, const char *psk);

/* Disconnect from the current network. */
void anx_mt7925_disconnect(void);

/* WiFi association state */
typedef enum {
	MT7925_STATE_DOWN = 0,   /* driver not initialized */
	MT7925_STATE_FW_UP,      /* firmware running, no WiFi */
	MT7925_STATE_SCANNING,   /* scanning for networks */
	MT7925_STATE_ASSOC,      /* associated, no IP */
	MT7925_STATE_CONNECTED,  /* associated + IP via DHCP */
} anx_mt7925_state_t;

anx_mt7925_state_t anx_mt7925_state(void);

#endif /* ANX_MT7925_H */
