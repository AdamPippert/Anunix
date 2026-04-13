# Anunix Development Guide

## Project Overview

Anunix is an AI-native operating system that redefines UNIX primitives around state, transformation, memory, routing, and validation. It replaces files with State Objects, processes with Execution Cells, and adds first-class memory, routing, and network planes.

The design is specified in RFCs 0001-0007 under `docs/rfcs/` and `rfcs/`.

## Development Principles

### Minimalism and Clarity

This OS follows the UNIX tradition of small, sharp tools and clear code. Every line should earn its place. Prefer simple, obvious implementations over clever ones. If a comment is needed to explain *what* code does, the code should be rewritten. Comments explain *why*.

### Language Policy

- **Kernel code**: C (C11) and ARM64/x86_64 assembly. No exceptions.
- **Userland tools**: C preferred. Assembly where performance demands it.
- **Build tooling**: Make, shell scripts, minimal Python only for code generation if unavoidable.
- **No C++, no Rust, no Go** in the kernel or core userland. The value proposition is a system built with the same discipline as classical UNIX — readable, auditable, portable C.

### Code Style (C)

- K&R brace style, tabs for indentation (8-wide, like Linux kernel).
- Function names: `subsystem_verb_noun()` — e.g., `state_object_create()`, `cell_runtime_admit()`.
- Struct names: `struct anx_state_object`, `struct anx_cell`. Prefix `anx_` for all public symbols.
- Macros: `ANX_UPPERCASE` — e.g., `ANX_MAX_TIERS`, `ANX_OK`.
- Header guards: `#ifndef ANX_SUBSYSTEM_HEADER_H`.
- No typedefs hiding pointers. `struct anx_foo *` is always spelled out.
- Static functions for file-local scope. Minimal use of global state.
- Every public function has a one-line doc comment above it in the header.

### Code Style (Assembly)

- AT&T syntax for x86_64, standard ARM64 syntax for aarch64.
- One file per logical unit (e.g., `context_switch.S`, `syscall_entry.S`).
- `.S` extension (preprocessed assembly), not `.s`.
- Liberal comments — assembly *always* needs explanation.

### Error Handling

- Functions return `int` status codes. 0 = success, negative = error.
- Error codes defined in `include/anx/errno.h`. Subsystem-specific codes in subsystem headers.
- No `errno` global. Errors flow through return values.
- No exceptions, no longjmp for error handling. Explicit cleanup with `goto cleanup` pattern.

### Memory Management

- No hidden allocations. Every allocation has an explicit owner and a documented lifetime.
- Prefer stack allocation and arena/pool allocators over malloc/free pairs.
- All allocations go through `anx_alloc()` / `anx_free()` which track and can be audited.

## Target Platforms

### Primary Targets

| Platform | Architecture | SoC / CPU | Notes |
|----------|-------------|-----------|-------|
| Apple Silicon Macs | ARM64 (AArch64) | M1, M2 | Like Asahi Linux — requires device tree, custom boot |
| Framework Laptop 16 (2nd gen) | x86_64 | AMD/Intel | Standard UEFI boot |
| Framework Desktop | x86_64 | AMD/Intel | Standard UEFI boot |

### Architecture Split

Hardware-specific code lives under `kernel/arch/<arch>/`. Everything else is architecture-independent. The boundary is enforced: arch code implements a defined interface, core code never `#ifdef`s on architecture.

```
kernel/arch/arm64/   — Apple Silicon boot, MMU, interrupts, device tree
kernel/arch/x86_64/  — UEFI boot, page tables, APIC, ACPI
kernel/core/         — Architecture-independent kernel (state, cells, memory, routing)
```

## Build System

- GNU Make. No CMake, no autotools, no meson.
- Cross-compilation: build on M1 Mac Studio, target ARM64 and x86_64 bare metal.
- `make kernel ARCH=arm64` or `make kernel ARCH=x86_64` selects target.
- Toolchain: System clang (Xcode CLT) for compilation + locally-fetched LLVM tools for linking (`make toolchain`). No Homebrew.
- Output: raw kernel binary (`build/<arch>/anunix.bin`), ELF (`build/<arch>/anunix.elf`).

### Prerequisites

```
make toolchain             # One-time: fetches ld.lld + llvm-objcopy into tools/llvm/bin/
```

No Homebrew. System clang (Xcode Command Line Tools) handles compilation. The `make toolchain` target downloads only the LLVM binaries Apple omits (ld.lld, llvm-objcopy) from official LLVM releases into `tools/llvm/bin/`. QEMU is installed separately where needed.

### Build Targets

```
make kernel                # Build for default ARCH (detected from host)
make kernel ARCH=arm64     # Build for Apple Silicon
make kernel ARCH=x86_64    # Build for Framework devices
make clean                 # Remove build artifacts
make test                  # Run kernel unit tests (host-native)
make qemu                  # Boot in QEMU (headless, serial console)
make toolchain             # Fetch LLVM linker tools
make toolchain-check       # Verify dependencies installed
```

## Testing Strategy

**VM-first**: All validation happens in VMs before touching real hardware.

### Development Machine

- **Adams-Mac-Studio** — M1 Mac Studio, primary dev box.

### VM Testing with UTM

UTM is the VM host for both architectures:

| Target | UTM Backend | Performance | Use |
|--------|-------------|-------------|-----|
| ARM64 | Apple Virtualization.framework | Near-native | Primary development loop |
| x86_64 | QEMU emulation | Slower | Validation before Framework hardware |

### Headless Testing with QEMU

For quick iteration without UTM GUI, `make qemu` boots the kernel in QEMU with serial console output. This is the fastest feedback loop during development.

### Linux Validation with QEMU/libvirt (Jekyll)

Finished VM images are copied to **Jekyll** (Linux server) for testing under QEMU/libvirt. This validates the common deployment pattern — most users will run Anunix as a QEMU/libvirt guest on Linux before (or instead of) bare metal.

Pipeline: build on Mac Studio → test in UTM → copy to Jekyll → test under QEMU/libvirt on Linux.

### Real Hardware (later)

Real hardware testing comes after both UTM and QEMU/libvirt validation pass. Targets: Apple Silicon Macs (M1/M2), Framework Laptop 16 (2nd gen), Framework Desktop.

## Directory Structure

```
kernel/
  arch/
    arm64/            # Apple Silicon: boot, MMU, interrupts, timers
    x86_64/           # Framework: UEFI boot, paging, APIC, ACPI
  core/               # Architecture-independent kernel
    state/            # State Object Layer (RFC-0002)
    exec/             # Execution Cell Runtime (RFC-0003)
    mem/              # Memory Control Plane (RFC-0004)
    route/            # Routing Plane (RFC-0005)
    sched/            # Unified Scheduler (RFC-0005)
    net/              # Network Plane (RFC-0006)
    cap/              # Capability Objects (RFC-0007)
    posix/            # POSIX compatibility shim
  include/
    anx/              # Public kernel headers
    arch/             # Architecture-specific headers
  lib/                # Kernel support library (string, printf, etc.)
  drivers/            # Device drivers
lib/                  # Userland library (libanx)
tools/                # Build scripts, image creation
tests/                # Kernel unit tests (run on host)
docs/                 # RFCs and design documents
config/               # Build and runtime configuration
```

## Testing

- Unit tests run on the host using a test harness that stubs hardware interfaces.
- Integration tests run in QEMU.
- Every subsystem has tests. No code merges without tests.
- Test files: `tests/test_<subsystem>_<thing>.c`

## RFC to Code Mapping

| RFC | Subsystem | Directory |
|-----|-----------|-----------|
| RFC-0001 | Architecture (principles only) | — |
| RFC-0002 | State Object Model | `kernel/core/state/` |
| RFC-0003 | Execution Cell Runtime | `kernel/core/exec/` |
| RFC-0004 | Memory Control Plane | `kernel/core/mem/` |
| RFC-0005 | Routing + Scheduler | `kernel/core/route/`, `kernel/core/sched/` |
| RFC-0006 | Network Plane | `kernel/core/net/` |
| RFC-0007 | Capability Objects | `kernel/core/cap/` |

## Workflow

- One component at a time. Implement, test, integrate.
- Start with the State Object Layer (RFC-0002) — it's the foundation everything else builds on.
- Keep the Python prototype as a reference/test oracle, but the real OS is C.
- Commits are atomic and descriptive. One logical change per commit.
