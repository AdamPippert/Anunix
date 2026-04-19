# RFC-0014: Hardware Platform Support

**Status**: Draft  
**Date**: 2026-04-18  
**Author**: Anunix Kernel Team  

---

## 1. Overview

Anunix targets real hardware from day one. VMs and emulators are used for development and testing, but they are not the destination — they are the proving ground. The primary hardware targets are:

| Platform | Architecture | CPU / SoC | Notes |
|----------|-------------|-----------|-------|
| Framework Desktop (2nd gen) | x86_64 | AMD Ryzen AI HX 370 | Primary x86_64 target; includes XDNA NPU |
| Framework Laptop 16 (2nd gen) | x86_64 | AMD/Intel | Secondary x86_64 target |
| Apple Silicon Macs | ARM64 (AArch64) | M1, M2 | Primary ARM64 target; near-native in UTM |

The Framework Desktop is the priority x86_64 machine because it includes the AMD XDNA NPU (Ryzen AI HX 370), which is Anunix's primary AI acceleration target. Apple Silicon is the primary ARM64 target because the development machine is an M1 Mac Studio.

QEMU/UTM are used throughout development as the fast feedback loop. Jekyll (a Linux server) hosts QEMU/libvirt for final validation before real hardware. See the build system documentation for the full pipeline.

---

## 2. Boot-Time Hardware Pre-Discovery

These devices must be reachable before PCI enumeration. `anx_pci_init()` has not run yet. Anything that requires PCI scanning, DMA setup, or firmware loading is not available at this stage.

### 2.1 Graphics (Framebuffer)

Both architectures use a linear framebuffer passed at boot. No driver is needed — the bootloader or firmware sets up a framebuffer and passes its physical address, dimensions, and pixel format to the kernel via the boot protocol.

- **x86_64**: multiboot2 framebuffer tag from GRUB. The kernel reads `multiboot2_tag_framebuffer` and maps the framebuffer linearly.
- **ARM64**: Apple Simple Framebuffer (ASB), passed via device tree. Physical address and geometry come from the FDT `simple-framebuffer` node.

This is intentionally minimal. GPU-accelerated display comes later, after PCI enumeration and driver loading.

### 2.2 Console (UART/Serial)

UART is the first output path and remains available throughout boot. It requires no interrupts, no DMA, and no PCI.

- **x86_64**: COM1 at I/O port 0x3F8, 115200 baud, 8N1.
- **ARM64 (QEMU virt)**: PL011 UART at MMIO 0x09000000.
- **ARM64 (Apple Silicon bare metal)**: Apple UART — address varies by SoC; set up by m1n1 before Anunix loads.

All early kernel messages go to UART before any other subsystem initializes. The framebuffer console is layered on top after the framebuffer is mapped.

### 2.3 Keyboard (PS/2)

PS/2 keyboard is implemented and working. It requires no PCI, no DMA, and no firmware.

- Controller: 8042 (or compatible), ports 0x60 (data) and 0x64 (status/command).
- IRQ: IRQ 1 (keyboard), IRQ 12 (mouse aux channel).
- Scancode translation: scan code set 2 → ASCII, with modifier tracking.

PS/2 is available on x86_64 only. ARM64 (Apple Silicon) has no PS/2 controller; USB HID keyboards require the full USB stack.

### 2.4 Mouse (PS/2)

PS/2 mouse is planned. The 8042 aux channel is the same controller as the keyboard.

- Data arrives on IRQ 12 via the aux channel (0x60 data port, aux mode).
- 3-byte packet format: buttons + delta X + delta Y.
- Status: planned, not yet implemented.

### 2.5 Network (Pre-PCI)

There is no pre-PCI network path. Network requires PCI enumeration. See Section 3.1 for network drivers.

---

## 3. PCI-Enumerated Devices

These devices are discovered after `anx_pci_init()` scans the PCI bus. The kernel probes by vendor ID and device ID, instantiates drivers, and registers them with the appropriate subsystem.

### 3.1 Network

The network driver priority is:

1. **virtio-net** — probed first. Covers QEMU and most VM environments. Simple, fast, no firmware needed.
2. **Intel E1000/E1000e** — fallback for real hardware. 10 device IDs covered, handles the full E1000 family. This covers the vast majority of x86_64 bare-metal deployments.

Together these two drivers cover >95% of environments Anunix is likely to run on during development and early deployment.

### 3.2 AI Accelerators

The AMD XDNA NPU is the primary AI acceleration target.

- **AMD XDNA NPU**: PCI 1022:1502, 1022:1506, 1022:1638, 1022:17f0. Driver: `xdna.c`. Registers as `ANX_ENGINE_DEVICE_SERVICE` with `ANX_CAP_TENSOR_NPU | ANX_CAP_TENSOR_INT8 | ANX_CAP_TENSOR_FP32`. See [xdna-driver-guide.md](../hardware/xdna-driver-guide.md) for full details.
- **AMD Radeon GPU**: planned. PCIe device; will use open-source AMDGPU register documentation as a starting point.
- **NVIDIA RTX GPU**: planned. Requires MMIO register map from reverse-engineering efforts.

### 3.3 Storage

- **virtio-blk**: implemented and working in QEMU. Shares IRQ 11 with virtio-net in the standard QEMU configuration.
- **NVMe**: planned. PCIe standard; register interface is well-documented and straightforward to implement.

---

## 4. Architecture-Specific Details

### 4.1 x86_64

**Boot protocol**: multiboot2 from GRUB. The kernel entry point is a 32-bit protected-mode entry that sets up long mode, then jumps to the 64-bit kernel proper. The multiboot2 info structure carries the framebuffer tag, memory map, and command line.

**Interrupts**: PIC 8259A. IRQs 0-15, remapped to IDT vectors 32-47 to avoid conflicts with CPU exception vectors. Shared IRQ lines are supported with a table of 4 handlers per IRQ line. The APIC path is planned but not yet implemented.

**MMIO**: identity-mapped in the kernel page tables. MMIO regions are mapped at their physical addresses. PCI BAR regions are mapped as needed when drivers probe devices.

**Standard QEMU IRQ assignments**:
- IRQ 1: PS/2 keyboard
- IRQ 4: COM1 UART
- IRQ 11: virtio-blk, virtio-net (shared)
- IRQ 12: PS/2 mouse (aux)

### 4.2 ARM64

**Boot protocol**: Device tree (FDT) passed in x0 at kernel entry. The ARM64 boot stub (`kernel/arch/arm64/boot.S`) sets up the MMU (4KB pages, 39-bit VA), establishes a stack, and calls `kernel_main()` with the FDT pointer.

**Interrupts**: GICv2. The GICD (distributor) is at 0x08000000 and GICC (CPU interface) at 0x08010000 in the QEMU virt machine. INTID 32 and above are SPIs (shared peripheral interrupts). Up to 16 registered handlers per INTID.

**Apple Silicon bare metal**: The M1/M2 uses the Apple Interrupt Controller (AIC), not GICv2. UTM (Apple Virtualization.framework) presents a GICv2-compatible interface, so the current GICv2 driver works in UTM. Bare-metal boot requires AIC support; see [apple-silicon-guide.md](../hardware/apple-silicon-guide.md).

**PMGR (Apple Power Management)**: Apple Silicon peripherals require clock enables via the PMGR before they can be accessed. This is not needed in UTM (the VM handles clock state), but is required for bare-metal PCIe, USB, and display.

---

## 5. Hardware Support Matrix

| Device Class | Driver File | Status | Notes |
|-------------|-------------|--------|-------|
| PS/2 keyboard | `drivers/ps2.c` | Implemented, working | IRQ 1, ports 0x60/0x64 |
| PS/2 mouse | `drivers/ps2.c` | Planned | IRQ 12 aux channel |
| USB HID keyboard | `drivers/usb_mouse.c` | Polling only, IRQ not wired | Requires XHCI stack |
| USB HID mouse | `drivers/usb_mouse.c` | Polling only, IRQ not wired | Requires XHCI stack |
| virtio-net | `drivers/virtio_net.c` | Implemented, working | QEMU primary network |
| Intel E1000/E1000e | `drivers/e1000.c` | Implemented | 10 device IDs; real hardware fallback |
| AMD XDNA NPU | `drivers/xdna.c` | Skeleton + firmware load | PSP mailbox; needs npu.sbin |
| AMD Radeon GPU | — | Planned | Open-source register docs |
| NVIDIA RTX GPU | — | Planned | Reverse-engineered MMIO |
| NVMe storage | — | Planned | PCIe standard |
| virtio-blk | `drivers/virtio_blk.c` | Implemented | QEMU storage |
| PL011 UART | `kernel/arch/arm64/` | Implemented | ARM64 console |
| Apple Simple Framebuffer | `kernel/arch/arm64/` | Implemented | ARM64 early display |
| multiboot2 framebuffer | `kernel/arch/x86_64/` | Implemented | x86_64 early display |

---

## 6. IRQ Architecture

### 6.1 x86_64 — PIC 8259A

The 8259A PIC provides 15 usable IRQ lines (IRQs 0-15, with IRQ 2 used for cascade). Each IRQ line supports a shared handler table with 4 slots. If a device shares an IRQ, its handler checks the device's own status register to confirm the interrupt originated from it before handling.

```
IRQ  0  — PIT timer
IRQ  1  — PS/2 keyboard
IRQ  2  — PIC cascade (unusable)
IRQ  3  — COM2 (if present)
IRQ  4  — COM1 (UART debug console)
IRQ  5  — (free)
IRQ  6  — floppy (historical; unused)
IRQ  7  — LPT1 (historical; unused)
IRQ  8  — RTC
IRQ  9  — (free / ACPI SCI)
IRQ 10  — (free)
IRQ 11  — virtio-blk, virtio-net (shared in QEMU)
IRQ 12  — PS/2 mouse (aux channel)
IRQ 13  — FPU (historical)
IRQ 14  — ATA primary (if present)
IRQ 15  — ATA secondary (if present)
```

The shared IRQ 11 arrangement (virtio-blk + virtio-net) is the default QEMU layout. On real hardware, PCI devices get IRQs assigned by the BIOS/ACPI — the shared table handles whatever assignment comes in.

### 6.2 ARM64 — GICv2

GICv2 supports up to 1020 INTIDs. INTIDs 0-15 are SGIs (software-generated), 16-31 are PPIs (per-processor), and 32+ are SPIs (shared peripheral interrupts). Anunix registers up to 16 handlers per INTID.

In the QEMU virt machine, UART is on SPI 1 (INTID 33), virtio devices start at INTID 48.

For Apple Silicon bare metal, the GICv2 driver must be replaced with an AIC driver before any interrupts work. See [apple-silicon-guide.md](../hardware/apple-silicon-guide.md), Phase 2.

---

## 7. Future Hardware

These are documented as intent, not current work. Contributors picking up any of these items should read the relevant upstream sources.

**AMD Radeon GPU**: The open-source AMDGPU driver in the Linux kernel provides the register map and command stream format. Anunix would need a GEM-like buffer manager, a command ring, and interrupt handling for GPU completion events. The AMDGPU DCN display engine is a separate block for display output.

**NVIDIA RTX GPU**: Nouveau (open-source) and the Envytools project have reverse-engineered register documentation for older NVIDIA hardware. NVIDIA has released some open-source kernel module code since 2022; this is the best starting point for modern RTX support. MMIO register layout varies by GPU generation.

**NVMe**: The NVMe specification is publicly available from nvmexpress.org. The interface is well-understood: admin queue + I/O queues, 64-byte submission and completion queue entries, MSI-X for interrupts. This is the most straightforward item on this list.

**USB XHCI**: The XHCI specification from Intel covers the host controller. A full USB stack is needed to enumerate devices, handle class drivers (HID for keyboards and mice, mass storage, etc.), and manage power states. This unlocks USB keyboards/mice on all platforms, including Apple Silicon where PS/2 is unavailable.

**Apple AIC**: Required for Apple Silicon bare metal. The AIC (Apple Interrupt Controller) is documented in part by the Asahi Linux project. m1n1 source code is the best reference. See [apple-silicon-guide.md](../hardware/apple-silicon-guide.md).

**Apple ANE (Neural Engine)**: Available on M1/M2. Would register as `ANX_ENGINE_DEVICE_SERVICE` with `ANX_CAP_TENSOR_NPU`. No public register documentation; Asahi Linux has early reverse-engineering research. Significantly harder than AMD XDNA. See [apple-silicon-guide.md](../hardware/apple-silicon-guide.md).
