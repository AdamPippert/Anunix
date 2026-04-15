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
| Model servers | **Model Hosting** | Kernel control plane for model lifecycle, leasing, and routing |

The kernel is real, it boots, and all subsystems initialize — including the model hosting control plane.

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
- **Model hosting control plane** — engine lifecycle, resource leasing, model server cells, staged routing, budget profiles
- Hardware capability probing (CPU, RAM, accelerators)
- Framebuffer console driver (VGA for x86_64, ramfb for ARM64)
- Interactive kernel monitor shell
- 11 passing unit tests
- QEMU boot for both architectures (built from source, no Homebrew)
- ANSI color boot splash

### Model Hosting Architecture

The kernel acts as a **control plane** for AI model hosting — it does not run inference itself. Key components:

- **Engine Registry** — models register with capability tags, quantization format, parameter count, context window, and throughput benchmarks
- **Engine Lifecycle** — 9-state machine (REGISTERED → LOADING → READY → AVAILABLE → DEGRADED → DRAINING → UNLOADING → OFFLINE / MAINTENANCE)
- **Resource Leasing** — memory tier and accelerator (GPU/NPU) reservations per engine, with availability tracking and exhaustion protection
- **Model Server Cells** — privileged `ANX_CELL_MODEL_SERVER` cells that host running engines, with health monitoring, backpressure, and automatic restart
- **Staged Routing** — deterministic feasibility + scoring in the kernel, with an escalation flag for a local semantic routing service (future) and slow-path RLM planner (future)
- **Budget Profiles** — named profiles (interactive_private, background_enrichment, critical_decision) with scoring weights and hard caps on latency and cost
- **Route Feedback** — ring buffer recording outcome signals (latency, cost, tokens, validation pass/fail) for future route improvement

### What's next

- Exception vectors (GIC for ARM64, IDT for x86_64) — needed for safe framebuffer detection and interrupt-driven I/O
- Engine dispatch wiring — connect route planner results to model server inference requests
- POSIX compatibility shim
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
make qemu              # Boot in QEMU, serial console (Ctrl-A X to quit)
make qemu-fb           # Boot with framebuffer display window
make qemu ARCH=arm64   # Boot ARM64 kernel
make test              # Run unit tests (11 tests)
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
    route/          Routing Plane + Model Hosting (RFC-0005)
      engine.c      Engine registry + lifecycle
      planner.c     Route planner with escalation
      model_server.c Model server cell management
      lease.c       Resource leasing (memory + accelerators)
      hwprobe.c     Hardware capability probing
      feedback.c    Route outcome recording
      budget.c      Budget profiles
    sched/          Unified Scheduler           (RFC-0005)
    net/            Network Plane               (RFC-0006)
    cap/            Capability Objects          (RFC-0007)
    shell.c         Interactive kernel monitor
    splash.c        Boot splash screen
    main.c          Kernel entry point
  drivers/
    fb/             Framebuffer + console drivers
  include/anx/      Public kernel headers
  lib/              Kernel support (kprintf, alloc, font, string, etc.)
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
