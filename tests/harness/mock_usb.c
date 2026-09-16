/*
 * mock_usb.c — Stub input driver symbols for host-native test builds.
 *
 * The xHCI and I2C input drivers program hardware by DMA and MMIO, which
 * the host build cannot do. wm.c and shell.c call their poll hooks, so the
 * link needs these symbols. The protocol layers they use (hid_boot.c,
 * i2c_hid.c, aml_res.c) build and run on the host.
 */

#include <anx/types.h>
#include <anx/xhci.h>
#include <anx/i2c_input.h>

int anx_xhci_init(void) { return ANX_ENOENT; }
void anx_xhci_poll(void) {}
uint32_t anx_xhci_hid_count(void) { return 0; }

int anx_i2c_input_init(void) { return ANX_ENOENT; }
void anx_i2c_input_poll(void) {}
