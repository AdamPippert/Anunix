# Anunix

An AI-native operating system that redefines UNIX primitives around state, transformation, memory, routing, and validation.

Written in C and assembly. No C++, no Rust, no Go.

## What It Does

Anunix replaces classical UNIX abstractions with primitives designed for AI-native workloads:

| UNIX | Anunix | Why |
|------|--------|-----|
| Files | **State Objects** | Content-addressed, versioned, with provenance |
| Processes | **Execution Cells** | Lifecycle-managed, composable, with resource budgets |
| `malloc`/`mmap` | **Memory Planes** | Tiered memory with semantic decay and promotion |
| Pipes | **Routing Plane** | Type-aware routing with pluggable transformation engines |
| Sockets | **Network Plane** | Federated execution across machines |
| `chmod`/ACLs | **Capabilities** | Object-level, unforgeable, delegatable |

The kernel is real, it boots, and all six subsystems initialize.

## Current Status

**The kernel boots on both ARM64 and x86_64 under QEMU.**

```
                       ___
                      /   \
                     /  o  \
                    /       \
                   /  _____  \
                  /  /     \  \
                 /  /       \  \
                /__/         \__\

                A N U N I X
          The AI-Native Operating System

  Anunix 0.1.0 booting

arch init complete
state object layer initialized
execution cell runtime initialized
memory control plane initialized
routing plane and scheduler initialized
network plane initialized
capability store initialized
kernel init complete -- all subsystems online

Anunix kernel monitor ready. Type 'help' for commands.

anx>
```

### What works

- Dual-architecture kernel (ARM64 + x86_64) built from the same source
- All subsystem foundations (RFC-0002 through RFC-0007)
- Interactive kernel monitor shell
- 7 passing unit tests
- QEMU boot for both architectures (built from source, no Homebrew)
- ANSI color boot splash

### What's next

- POSIX compatibility shim
- Framebuffer display driver (VGA/ramfb)
- GIC + timer interrupts (ARM64)
- IDT + APIC (x86_64)
- Real hardware testing (Apple Silicon, Framework Laptop)

## Target Platforms

| Platform | Architecture | Status |
|----------|-------------|--------|
| QEMU virt (ARM64) | AArch64 | Boots, all subsystems |
| QEMU (x86_64) | x86_64 | Boots, all subsystems |
| Apple Silicon Macs | AArch64 | Planned |
| Framework Laptop 16 | x86_64 | Planned |
| Framework Desktop | x86_64 | Planned |

## Building

### Prerequisites

- macOS with Xcode Command Line Tools (`xcode-select --install`)
- No Homebrew required

### One-time setup

```sh
make toolchain         # Fetch ld.lld + llvm-objcopy
make qemu-deps         # Build QEMU from source (~5 min)
```

### Build and run

```sh
make kernel            # Build for host architecture
make kernel ARCH=arm64 # Build for ARM64
make kernel ARCH=x86_64 # Build for x86_64
make qemu              # Boot in QEMU (Ctrl-A X to quit)
make qemu ARCH=arm64   # Boot ARM64 kernel
make test              # Run unit tests
```

## Project Structure

```
kernel/
  arch/
    arm64/          ARM64: PL011 UART, boot, page tables
    x86_64/         x86_64: COM1 serial, multiboot, long mode switch
  core/
    state/          State Object Layer          (RFC-0002)
    exec/           Execution Cell Runtime      (RFC-0003)
    mem/            Memory Control Plane        (RFC-0004)
    route/          Routing Plane               (RFC-0005)
    sched/          Unified Scheduler           (RFC-0005)
    net/            Network Plane               (RFC-0006)
    cap/            Capability Objects          (RFC-0007)
    shell.c         Interactive kernel monitor
    splash.c        Boot splash screen
    main.c          Kernel entry point
  include/anx/      Public kernel headers
  lib/              Kernel support (kprintf, alloc, string, etc.)
tests/              Host-native unit tests
tools/              Build scripts (LLVM fetch, QEMU build)
assets/             Brand logos
docs/rfcs/          Design specifications
```

## Design Documents

- [RFC-0001: Architecture Thesis](docs/rfcs/RFC-0001-architecture-thesis.md)
- [RFC-0002: State Object Model](rfcs/RFC-0002-state-object-model.md)
- [RFC-0003: Execution Cell Runtime](docs/rfcs/RFC-0003-execution-cell-runtime.md)
- [RFC-0004: Memory Control Plane](docs/rfcs/RFC-0004-memory-control-plane.md)
- [RFC-0005: Routing Plane and Unified Scheduler](docs/rfcs/RFC-0005-routing-and-scheduler.md)
- [RFC-0006: Network Plane and Federated Execution](docs/rfcs/RFC-0006-network-plane.md)
- [RFC-0007: Capability Objects](docs/rfcs/RFC-0007-capability-objects.md)

## Testing with UTM

To test on a Mac with [UTM](https://mac.getutm.app):

1. Build the kernel: `make kernel ARCH=arm64`
2. In UTM: **New VM** → **Emulate** → **Other**
3. Set Architecture to **ARM64 (aarch64)**, System to **virt**, Memory to **512 MB**
4. Skip storage (no disk needed)
5. In VM Settings → QEMU, add these arguments:
   ```
   -serial mon:stdio
   -kernel /path/to/Anunix/build/arm64/anunix.elf
   ```
6. Boot the VM — the splash and shell appear in the serial console

For x86_64, use Architecture **x86_64** and kernel path `build/x86_64/anunix-qemu.elf`.

## License

MIT
