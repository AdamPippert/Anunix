# RFC-0032: USB Input — Polled xHCI and the HID Boot Protocol

| Field      | Value                                                        |
|------------|--------------------------------------------------------------|
| RFC        | 0032                                                         |
| Title      | USB Input — Polled xHCI and the HID Boot Protocol            |
| Author     | Adam Pippert                                                 |
| Status     | Draft                                                        |
| Created    | 2026-09-15                                                   |
| Depends On | RFC-0012 (Interface Plane), RFC-0014 (Hardware Platform)     |
| Blocks     | —                                                            |

This RFC uses the RFC 2119 requirement keywords `MUST`, `MUST NOT`, `SHOULD`,
and `MAY`, as RFC 8174 amends them.

This RFC is written after the code. It records a stack that shipped in
2026.9.15 without a prior specification, which the repository convention
requires. Where the code and this text disagree, the code is the defect.

---

## Executive Summary

Anunix reads a keyboard and a mouse over USB. The driver polls; it takes no
interrupt. Two layers make that work.

1. **A host controller driver**, `kernel/drivers/usb/xhci.c`. It takes the
   controller from firmware, resets it, builds the ring structures xHCI
   requires, and enumerates the root ports and any hub behind them.
2. **A protocol translator**, `kernel/drivers/input/hid_boot.c`. It converts
   HID boot-protocol reports into Anunix input events. It performs no I/O, so
   it builds and runs in the host test suite.

The split is the point. Everything that can be tested without hardware sits in
the layer that has no hardware.

---

## 1. Motivation

The Framework Laptop 16 keyboard is a USB device. It sits behind an internal
Genesys hub at `05e3:0610`, on the xHCI controller at PCI `c5:00.0`. Before
this stack, Anunix had no USB input at all. The machine answered the firmware
console and then went deaf.

PS/2 emulation does not rescue this. The machine has no PS/2 controller.

---

## 2. Scope

In scope: one xHCI controller, root ports, one level of hub, and devices that
declare the HID boot protocol.

Out of scope, and not implemented:

- interrupts, including MSI and MSI-X;
- isochronous and bulk transfers;
- the HID report-descriptor parser, so any device that does not offer the boot
  protocol is skipped;
- USB mass storage;
- more than one level of hub;
- runtime suspend and remote wakeup.

---

## 3. Taking the controller from firmware

Firmware owns the controller at hand-off. The driver MUST claim it before it
touches any operational register.

The driver walks the extended capability list for `XHCI_EXT_CAPS_LEGACY`. It
sets the OS-owned bit, bit 24, then waits for firmware to clear the BIOS-owned
bit, bit 16. It then writes `XHCI_LEGACY_DISABLE_SMI`, which is
`(0x7 << 1) + (0xff << 5) + (0x7 << 17)`, so firmware raises no system
management interrupt against a controller it no longer owns.

A driver that skips this step and resets the controller anyway leaves firmware
holding a stale view of hardware that has changed underneath it.

---

## 4. Register access

The driver MUST map its BAR through `anx_mmio_map()` and MUST fail the probe
when the map fails. It MUST NOT dereference a physical address and assume the
identity map reaches it.

The reason is recorded in RFC-0031 and in the commit that added
`kernel/arch/x86_64/mmio.c`: the anxboot stub replaces the firmware page
tables with a map covering 0 to 4 GiB, and UEFI places 64-bit BARs above that.

---

## 5. Structures

After reset the driver builds, in this order:

1. the device context base address array;
2. the scratchpad buffer array, sized from `HCSPARAMS2`;
3. the command ring;
4. the event ring and its segment table.

It then enables the root ports and begins enumeration.

---

## 6. Enumeration

For each connected port the driver runs `usb_attach()`. A device whose class
is a hub goes through `hub_setup()`, and the driver enumerates the ports behind
it. A device that offers the boot protocol goes through `hid_setup()`, which
selects boot protocol, sets the idle rate, and arms the interrupt-in endpoint.
Completions arrive through `hid_complete()`.

`anx_xhci_hid_count()` reports how many boot devices the driver bound.

Observed under QEMU with `qemu-xhci`, `usb-kbd` and `usb-mouse`, 2026-09-15:

```
xhci: controller 1b36:000d at 00:03.0
xhci: port 5 route 0 high speed: 0627:0001 class 00
xhci: keyboard on slot 1 (endpoint 0x81, 8-byte reports)
xhci: port 6 route 0 high speed: 0627:0001 class 00
xhci: mouse on slot 2 (endpoint 0x81, 4-byte reports)
xhci: 1 controller(s), 2 HID boot device(s)
```

`usb-tablet` is not a boot-protocol device. It enumerates and produces no
pointer. Test with `usb-mouse`.

---

## 7. Polling

The driver has no interrupt path. `anx_xhci_poll()` drains the event ring and
MUST be called from every loop that waits for input. Two callers exist:
`anx_wm_run()` in `kernel/core/wm/wm.c`, and `kgetline()` in
`kernel/core/shell.c`.

A third loop that waits for input and does not call `anx_xhci_poll()` is a
defect in that loop.

---

## 8. Report translation

`kernel/drivers/input/hid_boot.c` holds the whole protocol layer:

| Function                  | Input                        | Output                |
|---------------------------|------------------------------|-----------------------|
| `anx_hid_kbd_report()`    | 8-byte boot keyboard report  | key down and key up   |
| `anx_hid_key_to_input()`  | HID usage plus modifiers     | Anunix key and unicode|
| `anx_hid_mouse_report()`  | 3-byte or 4-byte mouse report| motion and buttons    |

The keyboard layer tracks the previous report to derive press and release
edges, and treats the rollover code as "no keys", not as a key.

The tables are US layout. Other layouts are not implemented.

---

## 9. Testing

`tests/test_hid_boot.c` runs in the host suite and covers report translation
with no hardware present. `tests/harness/mock_usb.c` stubs `anx_xhci_init()`,
`anx_xhci_poll()` and `anx_xhci_hid_count()`, because `wm.c` and `shell.c`
reference them and the host build cannot do DMA.

The Makefile excludes `kernel/drivers/usb` from the host test sources.

---

## 10. What has not been tested

`Observed:` the stack enumerates a keyboard and a mouse under QEMU, delivers
reports, and drives the window manager. Screenshots and the serial log are in
the 2026.9.15 release evidence.

`Unknown:` every hardware-specific path.

- The full-speed keyboard behind a high-speed hub. That is the transaction
  translator path, and QEMU does not exercise it.
- Any device on the Framework Laptop 16 itself. The stack has never run on that
  machine.

---

## 11. Open work

- A report-descriptor parser, so devices without the boot protocol work.
- Interrupt-driven completion, which removes the polling contract in section 7.
- More than one hub level.
- Keyboard layouts other than US.
