# RFC-0033: ACPI Discovery, AML Resources and HID over I2C

| Field      | Value                                                        |
|------------|--------------------------------------------------------------|
| RFC        | 0033                                                         |
| Title      | ACPI Discovery, AML Resources and HID over I2C               |
| Author     | Adam Pippert                                                 |
| Status     | Draft                                                        |
| Created    | 2026-09-15                                                   |
| Depends On | RFC-0014 (Hardware Platform), RFC-0032 (USB Input)           |
| Blocks     | —                                                            |

This RFC uses the RFC 2119 requirement keywords `MUST`, `MUST NOT`, `SHOULD`,
and `MAY`, as RFC 8174 amends them.

This RFC is written after the code. It records a stack that shipped in
2026.9.15 without a prior specification, which the repository convention
requires. Where the code and this text disagree, the code is the defect.

---

## Executive Summary

A laptop touchpad is not a PCI device. Firmware describes it in AML, inside the
DSDT, as a device with hardware identifier `PNP0C50` that speaks HID over an
I2C bus. Nothing in the driver table can find it, because the driver table
probes PCI.

Four layers close that gap.

1. **ACPI discovery.** The EFI loader passes the RSDP to the kernel.
2. **An AML resource reader.** `kernel/lib/aml_res.c` walks the DSDT and
   extracts the bus address, the controller, and the interrupt line.
3. **An I2C master.** `kernel/drivers/i2c/dw_i2c.c`, with
   `kernel/drivers/i2c/amd_fch.c` supplying power and the ready line.
4. **A HID-over-I2C transport.** `kernel/drivers/input/i2c_hid.c`, feeding the
   same input path RFC-0032 feeds.

Layers 2 and 4 touch no hardware, so both run in the host test suite against
tables captured from real firmware.

---

## 1. Motivation

The Framework Laptop 16 touchpad is a PIXA3854 at I2C address `0x2C`, at
400 kHz, on the `AMDI0010` controller with `_UID` 3, MMIO `0xFEDC5000`. Its
interrupt is `AMDI0030` pin 8, level triggered, active low. Its HID descriptor
register is `0x20`.

None of those numbers is discoverable without reading AML. None is the same on
another machine. A driver that hardcodes them works on one laptop.

---

## 2. Finding the RSDP

Under BIOS the kernel scans low memory for the `RSD PTR ` signature. Under UEFI
that MUST NOT be relied on, because firmware is free to place the tables
anywhere.

The anxboot stub therefore records the RSDP in the boot information block at
physical `0x1000`, magic `ANXF`, with the RSDP at offset `0x1028`.
`kernel/drivers/acpi/acpi.c` reads it from there.

The reader MUST reject an address at or above 4 GiB, and the path is compiled
for x86_64 only.

`anx_acpi_find_table(sig, index, len)` returns a table by signature. It
resolves the DSDT through the FADT: `X_DSDT` at offset 140 first, then `DSDT`
at offset 40.

Observed under QEMU, 2026-09-15: `acpi: revision 2, oem BOCHS`.

---

## 3. Reading AML resources

`kernel/lib/aml_res.c` is a reader, not an interpreter. It executes no AML. It
walks `Device` objects and `PkgLength` encodings, matches `_HID` and `_CID`,
and extracts three resource descriptors:

| Descriptor       | Tag    | Yields                          |
|------------------|--------|---------------------------------|
| `I2cSerialBus`   | `0x8E` | slave address, speed, controller|
| `GpioInt`        | `0x8C` | pin, polarity, trigger mode     |
| `Memory32Fixed`  | `0x86` | controller MMIO base            |

The HID descriptor register comes from the `_DSM` method's data, under UUID
`3cdff6f7-4267-4555-ad05-b30a3d8938de`.

`Observed:` run against the full jekyll DSDT and its SSDTs, the reader finds
130 devices, which matches `iasl` on the same tables, and resolves exactly two
HID-over-I2C devices: `ECSL` at `0x51` on I2CB `0xfedc3000`, pin 84,
register `0x55`; and `TPAD` at `0x2c` on I2CD `0xfedc5000`, pin 8,
register `0x20`.

`tools/aml_scan_host.c` runs the same reader over real tables on a development
host. It declares its interface with `<stdint.h>` types, because the kernel
`types.h` defines its own `size_t` and that definition collides with libc.

---

## 4. Powering the controller

The controller is off at boot. `kernel/drivers/i2c/amd_fch.c` turns it on
through the AMD FCH always-on-away-controller block at `0xFED80000 + 0x1E00`.

- Devices 5 through 8 are I2C0 through I2C3.
- The D3 control register is at `0x40 + dev * 2`.
- `PWR_ON_DEV` is bit 3.
- The driver MUST read the state byte back and MUST require the value 7 before
  it proceeds.

The GPIO ready line lives at the bank base plus `pin * 4`, with `PIN_STS` at
bit 16.

For the jekyll touchpad the AOAC control for I2C3 is at `0xFED81E50` and the
GPIO bank is `AMDI0030` at `0xFED81500`.

---

## 5. The I2C master

`kernel/drivers/i2c/dw_i2c.c` is a polled DesignWare master. It takes no
interrupt, for the same reason the xHCI driver does not: the kernel has one
thread of control and a poll loop the window manager already runs.

The `AMDI0010` input clock is 150 MHz. Fast mode therefore uses `HCNT 132` and
`LCNT 239`.

---

## 6. HID over I2C

`kernel/drivers/input/i2c_hid.c` implements the transport.

- The HID descriptor is 30 bytes. The driver MUST validate it and MUST reject
  a `bcdVersion` other than `0x0100`.
- A command is `[reg_lo, reg_hi, type << 4 | id, opcode]`.
- An input report carries a 2-byte length prefix. A prefix of 0 means no data
  and MUST NOT be treated as a short read.

`anx_i2c_hid_mouse_report_id()` identifies a pointing collection by matching
the report-descriptor prefix `05 01 09 02 A1 01 85`.

`kernel/drivers/i2c/i2c_input.c` scans for `PNP0C50`, binds only devices with a
mouse collection, resets them, and polls report ID 1 into
`anx_usb_mouse_report()`, which is the same entry point RFC-0032 uses. A
touchpad and a USB mouse therefore reach the window manager by one path.

`ECSL` has no mouse collection and is skipped.

---

## 7. Wiring

`kernel_main()` calls `anx_i2c_input_init()` after `anx_drivers_probe()`,
because ACPI describes these devices and the driver table cannot probe them.
The call is non-fatal.

`anx_i2c_input_poll()` is called beside `anx_xhci_poll()` in `anx_wm_run()` and
in `kgetline()`.

---

## 8. Testing

`tests/test_aml_res.c` runs the reader against DSDT fixtures captured from real
firmware. `tests/test_i2c_hid.c` covers descriptor validation and input
framing, including the zero-length prefix. Both run in the host suite.

The Makefile excludes `kernel/drivers/i2c` from the host test sources, because
those files do MMIO. `tests/harness/mock_usb.c` stubs `anx_i2c_input_init()`
and `anx_i2c_input_poll()`.

---

## 9. What has not been tested

`Observed:` the AML reader agrees with `iasl` on real tables. The framing
layers pass in the host suite. Under QEMU the kernel reports
`i2c-hid: 0 device(s)`, which is correct, because the QEMU DSDT describes none.

`Unknown:` every path that writes to real I2C hardware. No DesignWare transfer,
no AOAC power-on, no GPIO ready line read, no I2C-HID reset and no mouse-mode
report has ever run on a machine. The emulator cannot exercise them, so the
first execution of this code will be on hardware.

---

## 10. Open work

- Run the stack on the Framework Laptop 16.
- Interrupt delivery through the GpioInt line, in place of polling.
- Devices with a touch collection rather than a mouse collection.
- `_DSM` data beyond the HID descriptor register.
