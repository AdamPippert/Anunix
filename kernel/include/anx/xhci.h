/*
 * anx/xhci.h — Polled xHCI host controller driver.
 *
 * Binds every xHCI controller on the PCI bus, enumerates root ports and
 * USB 2.0 hubs, and attaches HID boot-protocol keyboards and mice. The
 * driver uses no interrupts: anx_xhci_poll() drains the event rings and
 * delivers input reports, and the kernel main loop calls it.
 */

#ifndef ANX_XHCI_H
#define ANX_XHCI_H

#include <anx/types.h>

/* Probe and start every xHCI controller. Safe to call more than once. */
int anx_xhci_init(void);

/* Drain pending events on every controller and deliver input reports. */
void anx_xhci_poll(void);

/* Number of HID boot devices attached across all controllers. */
uint32_t anx_xhci_hid_count(void);

#endif /* ANX_XHCI_H */
