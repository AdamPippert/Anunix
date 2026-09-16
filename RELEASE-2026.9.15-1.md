# Anunix 2026.9.15-1 Release Notes

Milestone: **Anunix has input hardware of its own, and it boots on a real
UEFI machine.**

Two defects kept Anunix off the target laptop. The UEFI loader built a page
table that did not cover the address firmware loaded it at, so the kernel
faulted on the instruction after the `CR3` switch. And the machine's keyboard
is a USB device, which Anunix could not read, so the kernel went deaf the
moment it left the firmware console.

This release fixes the first and implements the second. It also merges the AI
research line, days 001 through 082, into `main`.

## Requirement keywords

These notes use ordinary `must` and `can`. They state no normative
requirements. RFC-0032 and RFC-0033 carry the specifications.

## Highlights

- **The UEFI loader maps the whole image before it leaves boot services.**
  The EFI stub links at `0x140000000` and carries no `.reloc` section, so
  firmware loads it near 5 GiB. The old map covered 0 to 4 GiB.
- **A polled xHCI driver, a HID boot-protocol keyboard and mouse**, and a
  HID-over-I2C touchpad stack with ACPI and AML discovery behind it.
- **The power button asks before it halts the machine.** Clicking the menu bar
  power icon used to halt immediately, with no confirmation.
- **NVMe owns its DMA buffer**, and times out against the TSC rather than a
  loop count.
- **Research days 001 to 082 merged into `main`**, with four new tests that
  cover the seams between days 077 and 082.
- **RFC-0032 and RFC-0033 written**, closing the specification debt these two
  driver stacks carried.
- **64 host-native suites pass, 0 fail.** The 2026.9.4 release ran 60.

## The loader defect

`kernel/boot/efi/efi_stub.c` allocated four 1 GiB pages, filled `PML4[0]`
alone, and loaded `CR3`. That map covers exactly 4 GiB. The stub itself was
running above it.

The fault reproduces in OVMF q35 with `-m 4G` as `#PF CR2=0x140001737`. The
address is inside the stub's own text.

`build_identity_map()` now runs before `ExitBootServices`. It allocates page
tables below 4 GiB with `AllocatePages`, maps 1 GiB pages up to the higher of
the memory map top and the framebuffer end, and caps at 8 TiB through
`PT_PDPT_MAX 16`. `efi_types.h` gained `EFI_ALLOCATE_TYPE` and a typed
`AllocatePages`.

The regression test is `tools/test-uefi-highmem.sh`, reached by
`make test-uefi-highmem`. It boots the ISO twice, at 1 GiB and at 4 GiB of
guest RAM. Only the 4 GiB guest places the image above 4 GiB, as real hardware
does. The script fails if firmware loaded `ANUNIX.EFI` below 4 GiB, so it
cannot pass without testing the thing it claims to test.

Observed 2026-09-15 on superrouter:

```
  BOOT  low (1G): reached 'kernel init complete' in 14s
  BOOT  high (4G): reached 'kernel init complete' in 14s
  PASS  high: ANUNIX.EFI loaded at 0x00140000000, above 4 GiB
  UEFI high-memory boot test passed
```

## MMIO mapping

The same 4 GiB map broke device access. UEFI places 64-bit BARs above 4 GiB —
OVMF gives the QEMU NVMe `0xc000000000` — so the first register read faulted
with `CR2` equal to the BAR.

`anx_mmio_map(phys, size)` adds 1 GiB pages covering a range and returns a
pointer. Mappings are uncached, `PCD|PWT`, because MMIO registers have side
effects: a cached mapping can serve a read from a stale line, or let a write
pass a doorbell. An entry that is already present is left alone, because a
1 GiB page low in memory also covers RAM.

`nvme.c`, `ahci.c` and the new `xhci.c` map their BARs and fail the probe when
the map fails. arm64 gets an identity-returning stub, because it has no page
tables of its own to add to yet.

Not converted, and still using raw BAR addresses: `e1000`, `hda`, `xdna`,
`mt7925` and `virtio`. Each faults the same way on a UEFI machine whose
firmware places its BAR high.

This work was cherry-picked from `rfc-0031-partition-layer`, a branch that is
pushed to Forgejo and merged into neither `main` nor the research line. Only
the MMIO part is in this release. The partition layer, the installer changes
and RFC-0031 stay on that branch.

## USB input

`kernel/drivers/usb/xhci.c` is a polled xHCI driver with no interrupt path. It
claims the controller from firmware through the legacy support extended
capability, resets it, builds the DCBAA, the scratchpad, the command ring and
the event ring, enables the root ports, and enumerates through `usb_attach`,
`hub_setup`, `hid_setup` and `hid_complete`.

`kernel/drivers/input/hid_boot.c` translates boot-protocol reports. It performs
no I/O, so it builds and runs in the host test suite, which is what
`tests/test_hid_boot.c` exercises.

`usb_mouse.c` lost its own PCI scan; the xHCI driver now owns enumeration.
`wm.c` and `shell.c` call `anx_xhci_poll()` in their idle loops.

RFC-0032 specifies this stack.

## ACPI, AML and HID over I2C

A laptop touchpad is described in AML, not on PCI. Four layers reach it.

`kernel/drivers/acpi/acpi.c` takes the RSDP from the EFI loader boot
information at `0x1000`, magic `ANXF`, RSDP at `0x1028`. Scanning low memory
does not work under UEFI, where firmware places the tables where it likes.
`anx_acpi_find_table()` resolves the DSDT through the FADT.

`kernel/lib/aml_res.c` walks AML `Device` and `PkgLength` structures, matches
`_HID` and `_CID`, and extracts `I2cSerialBus`, `GpioInt` and `Memory32Fixed`
descriptors. It executes no AML.

`kernel/drivers/i2c/dw_i2c.c` is a polled DesignWare master.
`kernel/drivers/i2c/amd_fch.c` powers the controller through AMD FCH AOAC and
reads the GPIO ready line. `kernel/drivers/input/i2c_hid.c` implements the
HID-over-I2C transport, and `kernel/drivers/i2c/i2c_input.c` binds `PNP0C50`
devices that carry a mouse collection.

Observed on the host, against tables captured from the Framework Laptop 16: the
reader finds 130 devices across the DSDT and its SSDTs, which matches `iasl`,
and resolves exactly two HID-over-I2C devices. One, `ECSL`, is correctly
skipped as non-pointing.

RFC-0033 specifies this stack.

## The power dialog and hotkeys

Clicking the power icon in the menu bar called `arch_halt()` directly. There
was no confirmation, and no way to reboot from the desktop at all.

`kernel/core/wm/wm_power.c` adds a modal dialog with Restart, Halt and Cancel,
with Cancel selected on open. Escape and N cancel, Y and R restart, H halts,
Enter commits the selection, arrows and Tab move it, a click outside cancels,
and every other key is swallowed. `pwr_commit()` flushes the boot log before it
acts.

The dialog is modal in fact. `wm.c` checks it first on key down and at the top
of pointer handling, and guards sit at priority 0 in `anx_wm_app_key_route()`
and at the top of `anx_wm_hotkey_dispatch()`.

Underneath it: `arch_reboot()` on x86_64, arm64 and heteris, and
`anx_acpi_reset()`, which restarts through the FADT reset register and returns
`ANX_ENOTSUP` when firmware offers none.

New hotkeys in `kernel/core/wm/wm_hotkey.c`:

| Keys                          | Action                |
|-------------------------------|-----------------------|
| `Meta+Escape`, `Ctrl+Alt+Del` | power dialog          |
| `Meta+W`                      | close window          |
| `Meta+Shift+W`                | workflow designer     |
| `Meta+T`                      | snap or tile toggle   |
| `Meta`+arrows, `Meta+HJKL`    | focus by direction    |
| `Meta+Shift`+arrows           | move window           |
| `Meta+Ctrl`+arrows            | resize window         |

`anx_wm_window_focus_dir()` scores candidates as `along + across * 4`, so a
window slightly off to the side does not beat one directly ahead, and skips
minimized windows. The switcher gained H, L, J and K as vim aliases.

## NVMe

`nvme_poll()` used a fixed budget of 20 million iterations, which expires on
Zen 5 before a DRAM-less SN770M finishes a read. A late DMA then wrote into a
caller heap buffer that `gpt.c` or `blk_probe.c` had already freed, and the
stale command ID failed the next command. `PRP2` was left 0 for page-straddling
buffers.

The controller now writes only into a driver-owned bounce page, the timeout is
TSC-based at `NVME_POLL_SECONDS 10` against a 6 GHz ceiling, and `nvme_poll()`
skips completions it is no longer waiting on.

Honest status: the corruption was never reproduced. An injection run against
the old driver failed at `identify` instead. The defect is inferred from the
symptom — an `Exception 6` at a truncated RIP, mid-print of the `disk:` line.
The fix is sound on its own terms. It is not a demonstrated repair of a
demonstrated fault.

## Crash reporting

The exception handler printed a truncated RIP, low 32 bits only, which sent the
first NVMe investigation to the wrong address. It now prints the full RIP, CS,
RFLAGS, RSP, CR2 and CR3, every general-purpose register, the 16 bytes at RIP,
and up to 16 stack entries inside the kernel text range, then flushes the boot
log.

## Research days 001 to 082

`origin/codex/ai-os-integration` resolves to the same commit as
`origin/codex/ai-os-day-082`. Every day tip from 076 to 082 is an ancestor of
it, and `git cherry` reports no unmatched commit on any day branch, so merging
the integration branch subsumes every individual day.

One conflict, in `Makefile`. `.PHONY` was resolved as the union of both sides,
keeping the RAID targets from `main` and the research targets.

`origin/codex/ai-os-release-preflight` looks like a ready-made reconciliation
and is not one. Its second parent is day 076, so days 077 through 082 are
absent from it. It was not used.

## New tests for the seams between days

Days 077 to 082 each landed on their own branch, and each shipped a probe in
`kernel/core/tools/research_dayNNN.c` that exercises one day alone. Nothing
exercised two together, and the host suite covered none of them.

`tests/test_research_integration.c` covers four seams:

| Seam | Days | Property |
|------|------|----------|
| `struct anx_cell` fields | 081, 082 | a group lifecycle leaves the routing catalog selection byte-identical |
| physical pages | 078, 082 | an idle reclaim leaves group scratch bytes and page count untouched |
| ownership and authority | 077, 082 | a group handoff moves the execution holder, not workload ownership |
| dispatch gates | 081, 082 | group revocation does not move the planner's verdict, and a rejected catalog index does not change admission |

Checked for non-vacuity rather than assumed. Corrupting the group scratch
between the write and the read fails the second case at `-9107`. Clearing
`routing.catalog_index` during the group lifecycle fails the first at `-9005`.
Both return to passing when the injected fault is removed.

## Build fix

`tools/fetch-grub.sh` pinned Debian's grub2 at `2.06-13+deb12u1`. Debian
replaced that build with `2.06-13+deb12u2` and removed the old files, so both
downloads returned a 404 page, `curl` saved the page under the `.deb` name, and
the script failed at `ar: file format not recognized`. The pin now names the
current point release. It will break the same way at the next one.


## Why this release is tagged `2026.9.15-1`

The tag `2026.9.15` was pushed first and its workflow failed on a green
test run. The failure was in the release gate, not in the tests.

The gate read the whole captured test output and took the first regex
match for `[0-9]+ failed`:

```sh
FAIL=$(grep -oE "[0-9]+ failed" test-output.txt | head -1 || echo "0 failed")
```

The suite prints this line on an expected negative path, well before its
summary:

```
kickstart: workflow load=anx:workflow/no-such/v1 failed (-3)
```

`v1 failed` matches. The gate therefore reported `1 failed` for a run that
had printed `64 passed, 0 failed`, and blocked the release.

`Observed:` the same defect blocked 2026.9.4, whose log shows
`=== Results: 60 passed, 0 failed ===` followed by `Tests: 60 passed, 1 failed`
and `Release blocked: tests failed`.

The gate now reads the harness summary line and nothing else. It blocks when
any test fails, when the suite reports zero passing tests, when `make test`
exits non-zero, and when no summary line is present at all, because a missing
summary means the suite did not finish.

No test was changed, skipped or relaxed to produce this release.

## Verification

Every check below ran on superrouter on 2026-09-15, against the merged tree.

| Check | Command | Result |
|-------|---------|--------|
| x86_64 kernel | `make kernel ARCH=x86_64` | builds |
| arm64 kernel | `make kernel ARCH=arm64` | builds |
| host C suite | `make test` | 64 passed, 0 failed |
| Python suites | `make test` | 9 passed, 12 passed |
| UEFI high memory | `make test-uefi-highmem` | passes at 1 GiB and 4 GiB |
| ISO | `make iso ARCH=x86_64` | builds, 28 MiB |

The host C suite ran 60 before this release's driver and integration tests were
added, and 64 after. No suite was removed.

QEMU boot, OVMF with `qemu-xhci`, `usb-kbd` and `usb-mouse`, 4 GiB:

- reaches `kernel init complete -- all subsystems online` and starts the
  desktop session;
- `xhci: 1 controller(s), 2 HID boot device(s)`, keyboard on slot 1 with 8-byte
  reports and mouse on slot 2 with 4-byte reports;
- `i2c-hid: 0 device(s)`, which is correct, because the QEMU DSDT describes
  none;
- `acpi: revision 2, oem BOCHS`, so ACPI came through the loader boot
  information;
- no exception and no panic in the serial log;
- eight relative mouse moves of `-60, -60` moved the pointer by exactly
  `-480, -480`;
- `Ctrl+Alt+Delete` opened the power dialog, and Escape closed it, leaving the
  dialog region pixel-identical to the clean desktop.

## Known issues

- **`Meta+Enter` did not open a terminal under QEMU.** `Ctrl+Alt+Delete`
  reached the window manager through the same USB keyboard, so the input path
  works. `Unknown:` whether QEMU's `sendkey` fails to hold the Meta modifier
  across a USB HID combination, or whether the window manager's Meta mapping is
  wrong. Untested on real hardware.
- **`Meta+T` garbles window contents.** Surfaces do not re-render at a new
  size, and `renderer_gpu.c` skips drawing when the surface buffer is smaller
  than `width * height * 4`. Resize-aware surfaces are a prerequisite for real
  tiling.
- **Closing a window leaves artifacts on screen.**
- **`hda.c` picks the wrong audio device**, taking the first class 04/03 device
  rather than the Ryzen HDA.
- **Titlebar traffic lights are drawn on the left and hit-tested on the right.**
  The visible dots are inert, and the minimize dot has no hit test at all.
- **Several finished subsystems ship with no callers**, including
  `anx_wm_switcher_open()`, `anx_wm_app_menu_open()`, the accessibility tree,
  and the font fallback registry.

## Untested on real hardware

This release has not been booted on the Framework Laptop 16. Everything below
is written, passes in QEMU or in the host suite, and has never run on the
machine it was written for.

- The full-speed keyboard behind a high-speed hub, which is the transaction
  translator path. QEMU cannot exercise it.
- Every I2C write: DesignWare transfers, AOAC power-on, the GPIO ready line,
  the I2C-HID reset, and mouse-mode reports.
- The framebuffer above 4 GiB, at `0x6210000000`.
- The MT7925 ownership handshake. Firmware is still not baked into the image,
  so association cannot work yet.
- The NVMe fix against the defect it was written for.

## Upgrading

Nothing in this release changes an on-disk format. The object store, the
partition table and the array superblocks are unchanged.

The ISO and the ELF are attached to this release. The EFI stub in this ISO maps
memory above 4 GiB, so it replaces any earlier stub that hangs at
`ExitBootServices` on a UEFI machine.
