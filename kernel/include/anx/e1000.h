/*
 * anx/e1000.h — Intel E1000/E1000e Gigabit Ethernet driver.
 *
 * Covers:
 *   8086:100E  82540EM (QEMU default "e1000" emulation)
 *   8086:100F  82545EM (VMware default)
 *   8086:10D3  82574L  (ICH10 PCH, common on desktop boards)
 *   8086:10EA  82577LM (Ibex Peak / first-gen Calpella)
 *   8086:1533  I210-AT (Atom / embedded / network appliances)
 *   8086:1539  I211-AT (common on consumer motherboards)
 *
 * Used as the fallback NIC when no virtio-net device is present.
 * On real hardware this covers most Intel platform NICs.
 *
 * The driver uses MMIO (BAR0) exclusively.  Legacy PIO mode
 * (BAR1 + BAR2) is not supported.
 */

#ifndef ANX_E1000_H
#define ANX_E1000_H

#include <anx/types.h>

/* Initialize — probe PCI, set up rings, register with net stack.
 * Returns ANX_OK if a supported NIC was found and started. */
int anx_e1000_init(void);

/* True if the E1000 driver is active. */
bool anx_e1000_ready(void);

/* Transmit a raw Ethernet frame (including header).
 * Returns ANX_OK or ANX_EBUSY if TX ring is full. */
int anx_e1000_tx(const void *frame, uint16_t len);

/* Poll for received frames.  Calls net_receive() for each frame found.
 * Safe to call from the main loop (non-blocking). */
void anx_e1000_poll(void);

/* Return MAC address (6 bytes). */
const uint8_t *anx_e1000_mac(void);

/* Print NIC status to kernel console. */
void anx_e1000_info(void);

#endif /* ANX_E1000_H */
