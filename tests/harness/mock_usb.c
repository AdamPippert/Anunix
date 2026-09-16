/*
 * mock_usb.c — Stub input driver symbols for host-native test builds.
 *
 * The xHCI driver programs hardware by DMA and MMIO, which the host build
 * cannot do. wm.c and shell.c call its poll hook, so the link needs these
 * symbols. The protocol layer it uses, hid_boot.c, builds and runs on the
 * host.
 */

#include <anx/types.h>
#include <anx/xhci.h>

int anx_xhci_init(void) { return ANX_ENOENT; }
void anx_xhci_poll(void) {}
uint32_t anx_xhci_hid_count(void) { return 0; }
