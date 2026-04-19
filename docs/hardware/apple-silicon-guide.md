# Apple Silicon (M1/M2) Support Guide

**Audience**: Kernel developers working on ARM64 support; contributors interested in Apple Silicon bare-metal boot.  
**Status**: ARM64 kernel working in UTM. Bare-metal boot is future work.  
**Date**: 2026-04-18  

---

## Current Status

| Component | Status | Notes |
|-----------|--------|-------|
| ARM64 kernel boot | Working | Boots in UTM via Apple Virtualization.framework |
| GICv2 IRQ controller | Working | UTM presents GICv2-compatible interface |
| UART serial console | Working | PL011 at 0x09000000 in QEMU virt machine |
| virtio-net | Working | Full networking in UTM |
| virtio-blk | Working | Storage in UTM |
| Framebuffer (ASB) | Working | Apple Simple Framebuffer via device tree |
| Bare-metal boot | Not yet | Requires m1n1 + AIC driver |
| Apple AIC (interrupt controller) | Not yet | Needed for bare metal |
| Apple PMGR (power management) | Not yet | Needed for PCIe/USB on bare metal |
| Apple AGX GPU | Not yet | Substantial effort; see Section 6 |
| Apple ANE (Neural Engine) | Not yet | Harder than XDNA; see Section 7 |

The development machine (M1 Mac Studio) builds both ARM64 and x86_64 kernels natively using the system clang from Xcode Command Line Tools. ARM64 kernel development has a tight feedback loop: build on Mac Studio, boot in UTM, iterate.

---

## Apple Silicon Hardware Overview

For someone coming from x86_64 Linux or a conventional embedded background, Apple Silicon has several distinctive characteristics.

### CPU Architecture

M1 and M2 are ARM64 (AArch64) SoCs with a heterogeneous core cluster:

- **P-cores (performance)**: 3.2 GHz+, out-of-order, large L2/L3 caches. These are the Firestorm (M1) and Avalanche (M2) cores.
- **E-cores (efficiency)**: ~2 GHz, lower power. These are Icestorm (M1) and Blizzard (M2) cores.
- The CPU presents a standard AArch64 ISA. Anunix's ARM64 code runs on both core types.

### Unified Memory Architecture

M1/M2 has no separate VRAM. CPU, GPU, and ANE all share the same physical memory pool. This simplifies buffer management (no DMA copies between CPU and accelerator for data that both need), but means the GPU/ANE DMA addresses are physical addresses in the main memory range, not a separate aperture.

### Interrupt Controller: AIC

Apple Silicon uses the Apple Interrupt Controller (AIC), not ARM GIC. AIC is undocumented by Apple. The Asahi Linux project has reverse-engineered it; m1n1 source code (`src/aic.c`) is the definitive reference.

In UTM with Apple Virtualization.framework, AIC is abstracted away — the VM presents a GICv2 interface, so the current `anx_gicv2` driver works. On bare metal, GICv2 is absent and AIC must be initialized before any device interrupts work.

### Power Management: PMGR

Peripheral clocks on Apple Silicon are managed by the PMGR (Power Manager). A peripheral does not respond to MMIO access unless PMGR has enabled its clock. On bare metal, the following sequence is required before using any peripheral:

1. Identify the PMGR power domain for the peripheral.
2. Write the enable bit for that domain via PMGR MMIO.
3. Wait for the domain to acknowledge power-on.
4. Then access the peripheral's register space.

m1n1 handles PMGR setup for the peripherals it hands off. If Anunix is loaded via m1n1 (Phase 1 of the bare-metal roadmap), m1n1 will have already enabled the PMGR domains for serial and basic peripherals.

### I/O

Apple Silicon has no PS/2 controller and no legacy ISA bus. All I/O is:
- PCIe (via Apple's PCIe controller, not a standard PCIe root complex)
- USB-C / Thunderbolt
- SPI (for keyboard and touchpad on MacBooks)
- I2C, UART (internal)

This means USB HID is the only keyboard/mouse path on real Apple Silicon hardware. The full XHCI stack is required.

### Boot Protocol

Apple Silicon uses a proprietary boot protocol. The m1n1 bootloader (Asahi Linux project) is the gateway between Apple's boot ROM and custom kernels. m1n1 sets up:
- Minimal MMU configuration
- UART for serial output
- Device tree (Apple DT format, close to but not identical to Linux FDT)
- Jump to the kernel entry point with x0 = device tree pointer

This is the same hand-off convention as standard ARM64 Linux boot, so an Anunix kernel built with the ARM64 boot protocol can be loaded by m1n1 with minor adaptation.

---

## ARM64 Architecture in Anunix

### Boot Sequence

`kernel/arch/arm64/boot.S` is the ARM64 entry point. On entry:
- x0 = FDT (device tree) pointer
- CPU is in EL1 (or EL2 if coming from a hypervisor like UTM)
- MMU is off
- Caches are off

The boot stub:
1. If entering at EL2, drops to EL1 via `eret`.
2. Disables interrupts (`msr daifset, #0xf`).
3. Sets up the page tables (4KB pages, 39-bit VA, TTBR0/TTBR1).
4. Enables the MMU (`mrs/msr sctlr_el1`).
5. Sets up the stack.
6. Saves x0 (FDT pointer).
7. Calls `kernel_main(fdt_pointer)`.

### GICv2 Driver

The GICv2 driver is in `kernel/arch/arm64/gicv2.c`. Addresses for the QEMU virt machine:
- GICD (Distributor): 0x08000000
- GICC (CPU Interface): 0x08010000

These are hardcoded for the QEMU virt machine. For bare-metal Apple Silicon, these addresses are wrong — AIC lives at a different address. The correct approach is to read IRQ controller addresses from the device tree rather than hardcoding them.

INTID numbering:
- 0-15: SGI (software-generated, used for IPIs between cores)
- 16-31: PPI (per-processor, like the CPU timer on INTID 30)
- 32+: SPI (shared peripheral, virtio devices at 48+, UART at 33)

Up to 16 handlers can be registered per INTID. The registration function is `anx_irq_register(intid, handler, priv)`.

### Device Tree

Anunix reads the FDT passed at boot to discover UART, IRQ controller addresses, and memory regions. The FDT parser is minimal — it reads only what the kernel needs rather than implementing a full OF/DT subsystem.

For Apple Silicon bare metal, the Apple Device Tree format differs from the Linux FDT in node naming and some property layouts. m1n1 can translate between the two formats, or Anunix can add an Apple DT parser for the properties it needs.

---

## Path to Bare-Metal Apple Silicon Boot

This is a roadmap, not current work. Each phase builds on the previous one. Do not attempt Phase 3 before Phase 2 works.

### Phase 1: m1n1 as Bootloader

m1n1 (https://github.com/AsahiLinux/m1n1) is a minimal bootloader that runs on Apple Silicon after the boot ROM finishes. It initializes enough hardware to load a secondary payload.

Steps:
1. Install m1n1 on the target Mac via the Asahi Linux installer or manually using `idevicerestore`/`kmutil`. This requires putting the Mac in DFU mode.
2. Build Anunix for ARM64: `make kernel ARCH=arm64`.
3. Configure m1n1 to load the Anunix kernel binary instead of Linux.
4. m1n1 passes FDT pointer in x0 and jumps to the Anunix entry point.

At this point, Anunix boots to UART output. No interrupts, no display, no storage — just the kernel main loop and serial console.

### Phase 2: Apple AIC Interrupt Controller

Without interrupts, the kernel busy-polls everything. UART output works (polled), but timers, network, and any device that uses IRQs do not.

The AIC driver needs to:
1. Find the AIC MMIO base from the device tree (`aic` node, `reg` property).
2. Initialize the AIC: set event routing, enable global interrupt delivery.
3. Implement `anx_irq_register()` and `anx_irq_dispatch()` for AIC INTIDs.
4. Replace the GICv2 driver selection with AIC on Apple Silicon.

References: m1n1 `src/aic.c`, Asahi Linux kernel `drivers/irqchip/irq-apple-aic.c`.

### Phase 3: PMGR Power Management

Before PCIe, USB, or display can be used, their PMGR power domains must be enabled. m1n1 enables domains for UART and basic boot peripherals, but not PCIe or USB.

The PMGR driver needs to:
1. Parse the PMGR node from the device tree.
2. Implement `anx_pmgr_enable(domain_id)` and `anx_pmgr_disable(domain_id)`.
3. Expose domain IDs to other drivers (PCIe, USB, etc.) so they call PMGR before touching their registers.

References: m1n1 `src/pmgr.c`, Asahi Linux `drivers/soc/apple/rtkit.c` (RTKit is the PMGR co-processor on M1+).

### Phase 4: Apple PCIe Controller

Apple Silicon's PCIe controller is not a standard ECAM-compatible root complex. It has a custom initialization sequence before ECAM access works. After initialization, standard PCI config space reads/writes work normally, so existing PCI enumeration code (`anx_pci_init()`) can take over.

References: Asahi Linux `drivers/pci/controller/pcie-apple.c`.

This unlocks: USB (via an XHCI controller on the PCIe bus), NVMe, and any PCIe cards attached via Thunderbolt.

### Phase 5: Apple AGX GPU

The AGX GPU provides display output and GPU compute. This is a large effort. Do not start Phase 5 before Phases 1-4 are solid.

See Section 6 for detail on what is known and what is needed.

### Phase 6: ANE Driver

The Apple Neural Engine is available on M1/M2 and would integrate with the Anunix routing plane as an NPU engine (`ANX_CAP_TENSOR_NPU`). This is harder than XDNA because there is no public register documentation.

See Section 7 for detail.

---

## Developing for Apple Silicon Today

The current development path uses UTM for all testing.

### Building

```sh
# On Mac Studio (native ARM64 build)
make kernel ARCH=arm64

# Output: build/arm64/anunix.elf, build/arm64/anunix.bin
```

The build uses the system clang from Xcode Command Line Tools. No cross-compiler is needed because the build host and target are both ARM64.

### Testing in UTM

UTM with the Apple Virtualization.framework backend runs ARM64 VMs at near-native performance. Configuration for Anunix:

- **Machine**: virt (the QEMU virt machine profile)
- **Backend**: Apple Virtualization.framework (not QEMU emulation)
- **Memory**: 512 MiB minimum, 2 GiB recommended
- **Network**: virtio-net
- **Storage**: virtio-blk, backed by a raw disk image
- **Serial**: UART console (PL011 at 0x09000000) — connect to the UTM serial terminal

UTM passes the kernel binary directly as the boot image. No bootloader is needed for the virt machine.

### QEMU Headless (Fast Loop)

For quick iteration without the UTM GUI:

```sh
make qemu ARCH=arm64
```

This boots the kernel in QEMU with serial console output piped to the terminal. Fastest feedback loop.

### Deploying to Jekyll for Validation

After UTM testing passes, the x86_64 build is deployed to Jekyll for QEMU/libvirt validation:

```sh
~/Development/Anunix-tools/scripts/deploy-jekyll.sh
```

Jekyll runs x86_64. ARM64 testing stays on the Mac Studio via UTM.

---

## Apple AGX GPU

The Apple AGX GPU is a tile-based deferred renderer (TBDR) — architecturally different from the immediate-mode rendering pipeline of AMD or NVIDIA GPUs. This difference matters for the command stream format and buffer layout.

### What Is Known

- **Asahi Linux** has a working Mesa driver (`drm-asahi`) with an LLVM backend for shader compilation. This is the most detailed public documentation of AGX.
- **AGX Instruction Set**: Asahi has reverse-engineered the ISA. Shaders compile to AGX machine code.
- **Command stream**: Tile-based — geometry is sorted into tiles, then each tile is rendered separately, minimizing bandwidth.
- **GEM-like buffer management**: GPU buffers are allocated in unified memory, shared between CPU and GPU.

### What Anunix Would Need

1. **PMGR clock enable** for the GPU power domain (Phase 3 prerequisite).
2. **Buffer allocator**: allocate GPU-visible buffers from the shared memory pool. Simpler than discrete GPU because there is no separate VRAM aperture.
3. **Command stream construction**: format matches AGX command buffer layout as documented by Asahi.
4. **Interrupt handling**: GPU completion interrupts via AIC.
5. **Display pipeline**: The AGX includes a display engine (DCP) separate from the 3D engine. DCP has its own firmware and mailbox interface.

The Asahi Linux Mesa driver (`src/asahi/`) is the best reference. This is substantial work — a realistic estimate for a minimal display-only path is several months of dedicated effort.

This is not a near-term priority. Document it here so future contributors know where to start.

---

## ANE (Apple Neural Engine)

The Apple Neural Engine is available on M1 (11-16 core ANE) and M2. It provides matrix multiply acceleration for ML inference, similar in purpose to AMD XDNA.

### What We Know

- The ANE is accessible at a fixed MMIO address (varies by SoC).
- Asahi Linux has early reverse-engineering research. The `ane` driver in development kernels is incomplete but has the basic register layout.
- Apple uses RTKit (a co-processor) to manage the ANE, similar to how the PSP manages AMD XDNA.

### Contrast with AMD XDNA

| Aspect | AMD XDNA | Apple ANE |
|--------|----------|-----------|
| Register docs | PSP mailbox + DMA rings (linux-firmware) | No public docs; reverse-engineered |
| Firmware | npu.sbin via linux-firmware repo | Apple proprietary; must run on device |
| Toolchain | ONNX / framework → XDNA job | CoreML → ANE job (on macOS) |
| Anunix status | Skeleton driver + firmware load | Not started |

### Integration Path

When ANE support is implemented, it registers as:

```c
anx_engine_register(&(struct anx_engine_desc){
    .name    = "apple-ane",
    .type    = ANX_ENGINE_DEVICE_SERVICE,
    .caps    = ANX_CAP_TENSOR_NPU | ANX_CAP_TENSOR_INT8 | ANX_CAP_TENSOR_FP32,
    .quality = 85,
    .submit  = anx_ane_submit,
    .priv    = dev,
});
```

`quality = 85` (lower than XDNA's 90) reflects the higher implementation uncertainty. This can be adjusted once the driver is stable and benchmarked.

### References

- Asahi Linux `ane` driver: https://github.com/eiln/ane (early research)
- Apple ANE reverse engineering: Asahi Linux developer blog posts
- RTKit protocol: m1n1 `src/rtkit.c`
