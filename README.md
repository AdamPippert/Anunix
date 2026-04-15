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
| `.env` files | **Credential Objects** | Kernel-enforced secrets with opaque payloads and scoped access |

## Current Status

**Version 2026.4.15** (CalVer). The kernel boots on ARM64 and x86_64, with full VM networking and HTTP client capability.

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

  Anunix 2026.4.15 booting

pci: 6 devices found
virtio-net: 52:54:0:12:34:56 on irq 11
net: ip 10.0.2.15 gw 10.0.2.2 dns 10.0.2.3
credential store initialized
kernel init complete -- all subsystems online

Anunix kernel monitor ready. Type 'help' for commands.

anx> ping 10.0.2.2
ping 10.0.2.2...
ping sent

anx> dns example.com
resolving example.com...
example.com -> 172.66.147.243

anx> http-get example.com 80 /
GET http://example.com:80/
HTTP 200, 540 bytes
```

### What works

- Dual-architecture kernel (ARM64 + x86_64) built from the same source
- **Boots on real UEFI hardware** (AMD Ryzen 9 HX 370) and QEMU/OVMF
- All subsystem foundations (RFC-0002 through RFC-0008)
- **VM networking** — virtio-net driver, full IP stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP), DNS resolver, HTTP/1.1 client
- **Credential store** (RFC-0008) — kernel-enforced secrets with opaque payloads, never exposed in traces or logs
- **PCI bus enumeration** — discovers devices on bus 0, enables bus mastering for DMA
- Model hosting control plane — engine lifecycle, resource leasing, model server cells, staged routing, budget profiles
- Framebuffer console driver (VGA for x86_64, ramfb for ARM64)
- Bootable ISO (BIOS + UEFI) for USB installation
- Interactive kernel monitor shell with networking commands
- 12 passing unit tests
- ANSI color boot splash

### Networking Stack

The kernel includes a complete networking stack for QEMU virtual machines:

| Layer | Component | Status |
|-------|-----------|--------|
| Device | Virtio-net PCI driver (legacy PIO) | Working |
| Link | Ethernet frame dispatch | Working |
| Network | ARP, IPv4 (gateway routing, checksums) | Working |
| Network | ICMP echo request/reply (ping) | Working |
| Transport | UDP with port dispatch, DNS resolver | Working |
| Transport | TCP client (4 connections, blocking I/O) | Working |
| Application | HTTP/1.1 GET/POST (Connection: close) | Working |
| Security | Credential store (RFC-0008 Phase 1) | Working |
| Security | TLS | Deferred (host-side proxy for now) |

For HTTPS APIs (like Claude), a TLS-terminating proxy runs on the QEMU host:
```sh
socat TCP-LISTEN:8080,fork,reuseaddr OPENSSL:api.anthropic.com:443,verify=1
```

### Credential Management (RFC-0008)

Secrets are first-class kernel objects, not plaintext strings:

```
anx> secret set anthropic-api-key sk-ant-api03-...
credential: anthropic-api-key stored (51 bytes)

anx> secret list
  anthropic-api-key  api_key  51 bytes  0 accesses

anx> secret show anthropic-api-key
  name:     anthropic-api-key
  type:     api_key
  size:     51 bytes
  accesses: 0
  payload:  [REDACTED]
```

Kernel-enforced invariants:
- Payloads never appear in traces, provenance logs, kprintf, or network messages
- Secure zeroing on revoke/rotate (constant-time, compiler-safe)
- Named addressing decouples rotation from policy
- Phase 2 adds cell-scoped access bindings (least privilege)

### What's next

- Credential injection into HTTP requests via routing plane
- In-kernel TLS 1.3 client (BearSSL port)
- Real hardware networking (driver for AMD/Intel NICs)
- Engine dispatch wiring — connect route planner to live model APIs
- Persistent storage (virtio-blk driver)

## Target Platforms

| Platform | Architecture | Status |
|----------|-------------|--------|
| QEMU virt (ARM64) | AArch64 | Boots, all subsystems |
| QEMU (x86_64) | x86_64 | Boots, networking, HTTP client |
| QEMU + OVMF (UEFI) | x86_64 | Boots, networking, HTTP client |
| AMD Ryzen 9 HX 370 | x86_64 | Boots (USB ISO, framebuffer) |
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
make iso-deps          # Fetch GRUB + xorriso for ISO builds
```

### Build and run

```sh
make kernel            # Build for host architecture
make kernel ARCH=x86_64 # Build for x86_64
make qemu              # Boot in QEMU, serial console (Ctrl-A X to quit)
make qemu-fb           # Boot with framebuffer display window
make test              # Run unit tests (12 tests)
make iso               # Build bootable x86_64 ISO (BIOS + UEFI)
```

### Boot with networking

```sh
qemu-system-x86_64 -m 512M -nographic -serial mon:stdio -no-reboot \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -kernel build/x86_64/anunix-qemu.elf
```

## Project Structure

```
kernel/
  arch/
    arm64/          ARM64: PL011 UART, boot, page tables
    x86_64/         x86_64: COM1 serial, multiboot, IDT, PIC, PIT
  core/
    state/          State Object Layer          (RFC-0002)
    exec/           Execution Cell Runtime      (RFC-0003)
    mem/            Memory Control Plane        (RFC-0004)
    route/          Routing Plane + Model Hosting (RFC-0005)
    sched/          Unified Scheduler           (RFC-0005)
    net/            Network Plane               (RFC-0006)
    cap/            Capability Objects          (RFC-0007)
    credential.c    Credential Store            (RFC-0008)
    shell.c         Interactive kernel monitor
    main.c          Kernel entry point
  drivers/
    fb/             Framebuffer + console drivers
    pci/            PCI bus enumeration
    virtio/         Virtio transport + virtio-net driver
    net/            IP stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP, DNS, HTTP)
  include/anx/      Public kernel headers
  lib/              Kernel support (kprintf, alloc, font, string, etc.)
tests/              Host-native unit tests
tools/              Build scripts (LLVM fetch, QEMU build, ISO assembly)
docs/rfcs/          Design specifications
config/             GRUB boot configuration
```

## Design Documents

- [RFC-0001: Architecture Thesis](docs/rfcs/RFC-0001-architecture-thesis.md)
- [RFC-0002: State Object Model](rfcs/RFC-0002-state-object-model.md)
- [RFC-0003: Execution Cell Runtime](docs/rfcs/RFC-0003-execution-cell-runtime.md)
- [RFC-0004: Memory Control Plane](docs/rfcs/RFC-0004-memory-control-plane.md)
- [RFC-0005: Routing Plane and Unified Scheduler](docs/rfcs/RFC-0005-routing-and-scheduler.md)
- [RFC-0006: Network Plane and Federated Execution](docs/rfcs/RFC-0006-network-plane.md)
- [RFC-0007: Capability Objects](docs/rfcs/RFC-0007-capability-objects.md)
- [RFC-0008: Credential Objects and Secrets Management](docs/rfcs/RFC-0008-credential-objects.md)

## License

MIT
