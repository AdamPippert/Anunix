# P0 Ticket Plan — Anunix userspace prerequisites for graphical browser

Status: advisory only (planning), no implementation in this phase.
Goal: produce executable ticket/test specs for a lower-cost implementation agent.

## P0 principles

- Build only enabling substrate; do not build browser features yet.
- Keep primitives deterministic and testable in kernel/unit + QEMU integration.
- C/ASM-first for kernel and core userspace runtime.

## Ticket P0-001: Userspace process isolation baseline

Objective
- Introduce minimal process isolation model suitable for browser components (UI process, renderer process, network process) to run outside kernel builtins.

Current evidence
- `docs/tool-plan.md`: kernel builtins in single address space.
- `kernel/include/anx/posix.h`: Mode A shim only.
- `kernel/core/posix/posix.c`: `anx_posix_exec()` returns stub (`ANX_ENOSYS`).

Scope
- Define process/address-space abstraction and ownership model.
- Implement minimal scheduler integration for isolated userspace tasks.
- Keep syscall surface minimal (open/read/write/close/poll/time/mmap-shared placeholder).

Out of scope
- Full POSIX compatibility.

Deterministic tests
1) Unit: create isolated process context and verify independent virtual address descriptors.
2) Unit: process crash does not corrupt sibling process memory map metadata.
3) Integration (QEMU): spawn two userspace test binaries; verify independent lifecycle and clean teardown.

Definition of done
- `exec` no longer stub for minimal static userspace test binary.
- Isolation invariants enforced in tests.

---

## Ticket P0-002: Executable loader + syscall ABI v0

Objective
- Load and run a minimal userspace binary with stable syscall ABI.

Scope
- Loader for one binary format target (ELF64 static is acceptable baseline).
- Syscall table v0 and userspace entry trampoline.
- Return-path and error-code mapping.

Deterministic tests
1) Unit: invalid binary rejection matrix (bad magic, invalid entry, bad segments).
2) Integration: hello-world userspace test exits with status code and stdout capture.
3) ABI test: syscall numbers map deterministically and reject unsupported calls with explicit code.

Definition of done
- Userspace test program can execute under QEMU and return deterministic output.

---

## Ticket P0-003: Compositor as Execution Cell (minimum viable)

Objective
- Replace/augment direct repaint helper with compositor cell execution loop tied to Interface Plane registry.

Current evidence
- `anx_iface_compositor_repaint()` exists as helper in `kernel/core/iface/iface.c`.

Scope
- Compositor cell lifecycle (start/stop/recover).
- Domain assignment (`visual-desktop` minimum).
- Focus arbitration policy v0 and repaint scheduling cadence.

Deterministic tests
1) Unit: one compositor cell per domain invariant.
2) Unit: compositor restart preserves surface registry integrity.
3) Integration: map N surfaces, trigger repaint cycles, verify commit order by z-order.

Definition of done
- Repaint/focus path is mediated by compositor cell runtime, not only direct shell-triggered helper.

---

## Ticket P0-004: Input routing hardening for interactive apps

Objective
- Make keyboard/mouse event routing browser-safe and deterministic.

Current evidence
- Input path exists (`kernel/core/input/input.c`) but timestamps and policy depth are minimal.

Scope
- Monotonic event timestamping.
- Deterministic ordering guarantees under burst load.
- Focus/hit-target policy for pointer events (v0).
- Event queue overrun accounting and backpressure policy.

Deterministic tests
1) Unit: key up/down ordering under synthetic burst.
2) Unit: focused surface receives events; unfocused does not.
3) Integration: pointer move/button events route correctly after focus changes.
4) Integration: queue overflow increments drop counter deterministically.

Definition of done
- Input invariants are codified in automated tests.

---

## Ticket P0-005: Browser-grade TLS trust baseline

Objective
- Provide secure outbound browser/network process path without proxy dependency.

Current evidence
- README roadmap points to in-kernel TLS as planned; current system relies on proxy flow for some model traffic.

Scope
- TLS 1.3 client path with hostname verification.
- Certificate chain validation.
- Trust-store object lifecycle (load/update/rotate).

Deterministic tests
1) Unit: cert chain accept/reject vectors.
2) Unit: hostname mismatch rejection.
3) Integration: HTTPS GET to known endpoint succeeds; invalid cert endpoint fails with explicit reason.

Definition of done
- TLS path and trust validation no longer rely on external TLS proxy workaround.

---

## Ticket P0-006: Browser profile/cache storage primitives

Objective
- Ensure userspace browser can persist profile/session/cache safely.

Scope
- Atomic write contract for profile blobs.
- Locking semantics for concurrent open handles.
- Crash recovery behavior for partially written state.

Deterministic tests
1) Unit: atomic rename/write commit leaves no torn profile.
2) Unit: concurrent writer lock contention deterministic failure/retry path.
3) Integration: forced reboot mid-write recovers last committed profile state.

Definition of done
- Profile durability and integrity guarantees documented and validated.

---

## Ticket P0-007: Shared-memory IPC v0 (userspace <-> compositor)

Objective
- Introduce low-copy IPC primitives needed for browser process decomposition.

Scope
- Shared buffer object type and mapping control.
- Event/notification channel for producer-consumer updates.
- Capability checks on mapping rights.

Deterministic tests
1) Unit: producer writes, consumer reads exact bytes + sequence id.
2) Unit: unauthorized map attempt denied.
3) Integration: userspace test process publishes frame buffer; compositor cell consumes and presents.

Definition of done
- A minimal but secure shared-memory transport exists for graphics data flow.

---

## Ticket P0-008: Conformance/perf harness integration (weekly)

Objective
- Integrate deterministic baseline harness into Anunix workflow for browser prerequisites.

Scope
- Add harness entrypoint in test tooling.
- Capture metrics JSON artifact every run.
- Compare current run with previous baseline and emit drift report.

Deterministic tests
1) Unit: report schema validation.
2) Integration: two consecutive runs produce stable diff semantics.
3) Integration: CI-style pass/fail gate on threshold breaches.

Definition of done
- Weekly deterministic conformance report generation is part of test workflow.

---

## P0 execution order (strict)

1. P0-001 Userspace isolation baseline
2. P0-002 Loader + syscall ABI
3. P0-007 Shared-memory IPC v0
4. P0-003 Compositor cell runtime
5. P0-004 Input routing hardening
6. P0-006 Profile/cache storage primitives
7. P0-005 TLS trust baseline
8. P0-008 Conformance/perf harness integration

Rationale
- Process/runtime substrate first; compositor/input/storage/network layered after that.
