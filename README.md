<p align="center">
  <img src="assets/logo-full.jpg" alt="Anunix" width="600">
</p>

<p align="center">
  <strong>The AI-Native Operating System</strong><br>
  Redefining UNIX primitives around state, transformation, memory, routing, and validation.<br>
  Written in C and assembly. No C++, no Rust, no Go.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-2026.4.15-blue" alt="Version">
  <img src="https://img.shields.io/badge/arch-x86__64%20%7C%20ARM64-green" alt="Architecture">
  <img src="https://img.shields.io/badge/license-MIT-lightgrey" alt="License">
  <img src="https://img.shields.io/badge/tests-12%20passing-brightgreen" alt="Tests">
</p>

---

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

## Release: 2026.4.15

### Highlights

- **Boots on real UEFI hardware** — tested on AMD Ryzen 9 HX 370 (96GB RAM) via USB ISO
- **Full VM networking stack** — virtio-net driver, Ethernet, ARP, IPv4, ICMP, UDP, TCP, DNS, HTTP/1.1
- **Credential store** (RFC-0008) — kernel-enforced secrets management with opaque payloads
- **Authenticated API calls** — `api` command reads credentials and injects auth headers
- **Boot-time credential provisioning** — pass secrets via GRUB command line, no manual entry
- **Command history** — up/down arrow navigation with secret value scrubbing
- **PCI bus enumeration** — discovers all devices on bus 0, enables DMA bus mastering
- **CalVer versioning** — switched from semver to date-based versioning (YYYY.M.D)
- **8 RFCs** — complete architecture from state objects through credential management

### Boot Output

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

anx> dns example.com
resolving example.com...
example.com -> 172.66.147.243

anx> http-get example.com 80 /
GET http://example.com:80/
HTTP 200, 540 bytes
```

## Networking Stack

The kernel includes a complete networking stack for virtual and physical machines:

| Layer | Component | Status |
|-------|-----------|--------|
| Device | Virtio-net PCI driver (legacy PIO) | Working |
| Link | Ethernet frame dispatch | Working |
| Network | ARP, IPv4 (gateway routing, checksums) | Working |
| Network | ICMP echo request/reply (ping) | Working |
| Transport | UDP with port dispatch, DNS resolver | Working |
| Transport | TCP client (4 connections, blocking I/O) | Working |
| Application | HTTP/1.1 GET/POST with auth header injection | Working |
| Security | Credential store with boot-time provisioning | Working |
| Security | TLS | Deferred (host-side proxy) |

## Credential Management (RFC-0008)

Secrets are first-class kernel objects with enforced invariants — not plaintext strings:

```
anx> secret set anthropic-api-key sk-ant-api03-...
credential: anthropic-api-key stored (51 bytes)

anx> secret show anthropic-api-key
  name:     anthropic-api-key
  type:     api_key
  size:     51 bytes
  accesses: 0
  payload:  [REDACTED]

anx> api anthropic-api-key 10.0.2.2 8080 /v1/models
API 10.0.2.2:8080/v1/models (credential: anthropic-api-key)
HTTP 200, ...
```

**Kernel-enforced invariants:**
- Payloads never appear in traces, provenance logs, kprintf, or network messages
- Secure zeroing on revoke/rotate (constant-time, compiler-safe)
- Command history scrubs secret values (`secret set name` stored, value stripped)
- Boot-time provisioning: `qemu -append "cred:name=value"` — no manual entry
- Remote fetch: `secret fetch <name> <host> <port> [path]`

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
make qemu              # Boot in QEMU, serial console
make qemu-fb           # Boot with framebuffer display window
make test              # Run unit tests (12 tests)
make iso               # Build bootable x86_64 ISO (BIOS + UEFI)
```

### Boot with networking and credentials

```sh
# Start TLS proxy on host (for HTTPS API access)
socat TCP-LISTEN:8080,fork,reuseaddr OPENSSL:api.anthropic.com:443,verify=1 &

# Boot with virtio-net and pre-provisioned API key
qemu-system-x86_64 -m 512M -nographic -serial mon:stdio -no-reboot \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -kernel build/x86_64/anunix-qemu.elf \
  -append "cred:anthropic-api-key=sk-ant-api03-YOUR-KEY"
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
assets/             Brand assets (logo)
docs/rfcs/          Design specifications
config/             GRUB boot configuration
```

## Design Documents

| RFC | Title | Status |
|-----|-------|--------|
| [RFC-0001](docs/rfcs/RFC-0001-architecture-thesis.md) | Architecture Thesis | Draft |
| [RFC-0002](rfcs/RFC-0002-state-object-model.md) | State Object Model | Draft |
| [RFC-0003](docs/rfcs/RFC-0003-execution-cell-runtime.md) | Execution Cell Runtime | Draft |
| [RFC-0004](docs/rfcs/RFC-0004-memory-control-plane.md) | Memory Control Plane | Draft |
| [RFC-0005](docs/rfcs/RFC-0005-routing-and-scheduler.md) | Routing Plane and Unified Scheduler | Draft |
| [RFC-0006](docs/rfcs/RFC-0006-network-plane.md) | Network Plane and Federated Execution | Draft |
| [RFC-0007](docs/rfcs/RFC-0007-capability-objects.md) | Capability Objects | Draft |
| [RFC-0008](docs/rfcs/RFC-0008-credential-objects.md) | Credential Objects and Secrets Management | Draft |

## Roadmap

### Next: Installable Agent OS (2026.5)

- **Text-based installer** with kickstart-style provisioning (JSON State Object)
- **Virtio-blk driver** + journaled on-disk object store
- **ACPI parsing** + extended PCI hardware discovery
- **DHCP client** for network-at-install-time
- **Multi-key authentication** — console login + scoped credential store access
- **Claude API client** — `ask` command in the kernel shell
- **Minimum viable agent** — perceive/plan/act/observe loop
- **Graphical installer** (after text installer is validated)
- Real hardware validation: Framework Laptop 16 → M1 Mac Studio

## License

MIT
