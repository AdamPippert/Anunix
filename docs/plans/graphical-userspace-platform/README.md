# Anunix Graphical Userspace Platform Plan

Status: advisory only
Scope: planning artifacts only, no implementation work
Target: globally available graphical userspace substrate in `~/Development/Anunix`
Date: 2026-04-23

This directory is intentionally named `graphical-userspace-platform` because its true function is platform work: the deliverables defined here are reusable services for all graphical applications and workflows, not browser-private features.

## 0) Boundary decision (authoritative)

This plan enforces a strict ownership split:

- Anunix scope: graphical userspace platform services, globally available to applications and workflows.
- Anunix-Browser scope: browser product/runtime, including `anxbrowserd`.

Anunix MUST NOT absorb browser-engine responsibilities (HTML/CSS/JS parsing/execution, DOM/layout engine behavior, browser product UX logic). `anxbrowserd` remains a standalone application daemon that consumes Anunix service contracts through adapters.

See:
- `00-boundary-decision.md`
- `02-anxbrowserd-vs-platform-services.md`
- `05-integration-contracts.md`
- `06-portability-and-adapter-requirements.md`

## 1) What exists today (verified in tree)

1. Interface Plane primitives exist in kernel headers and core code:
   - `kernel/include/anx/interface_plane.h`
   - `kernel/core/iface/iface.c`
   - `kernel/core/iface/event_queue.c`
2. Surface/Event object types are already in state object enum:
   - `kernel/include/anx/state_object.h` (`ANX_OBJ_SURFACE`, `ANX_OBJ_EVENT`)
3. Basic renderers exist:
   - GPU framebuffer renderer: `kernel/core/iface/renderer_gpu.c`
   - Headless renderer: `kernel/core/iface/renderer_headless.c`
4. Basic input bridge exists:
   - `kernel/include/anx/input.h`
   - `kernel/core/input/input.c`
5. Simple compositor pass exists (not a full compositor cell runtime):
   - `anx_iface_compositor_repaint()` in `kernel/core/iface/iface.c`
6. Interface CLI tools exist:
   - `kernel/core/tools/iface_tools.c` (`surfctl`, `evctl`, `compctl`, `envctl`)
7. Framebuffer GUI shell exists:
   - `kernel/drivers/fb/gui.c`

## 2) Critical gap: what is still missing for Anunix graphical userspace

The current codebase has interface scaffolding, but it is not yet a complete graphical userspace substrate with stable, reusable contracts.

`anxbrowserd` is one demanding consumer, but the actual gap is broader: these services must be usable by workflow surfaces, editors, dashboards, object browsers, shells, and future graphical applications.

### P0 missing capabilities (must-have)

1. Userspace process/runtime isolation suitable for graphical application daemons and helpers
   - Current limitation is explicit: kernel is effectively single-address-space for builtins and POSIX mode is a thin shim.
   - Evidence: `docs/tool-plan.md` line 9, `kernel/include/anx/posix.h`, `kernel/core/posix/posix.c` (`exec` stub).

2. Syscall/ABI + executable loading path for external userspace graphical applications

3. Compositor as real Execution Cell runtime (not just repaint helper)

4. Input routing hardening (hit testing, focus guarantees, timestamps, event ordering guarantees)
   - Current events have timestamp placeholder (`0` in input path).

5. OS trust/networking primitives available to userspace graphical applications
   - TLS + certificate chain/host validation + trust store lifecycle.

6. Durable application state/session/cache storage primitives

7. Shared-memory / low-copy IPC for application-runtime ↔ compositor/service boundaries

8. Deterministic conformance/perf harness integrated with Anunix test loop
   - For repeatable validation of graphical userspace behavior and higher-level consumers such as `anxbrowserd`.

### P1 missing capabilities (should-have for practical graphical use)

1. GPU/compositor acceleration path with dirty-rect efficiency.
2. Multi-window/surface primitives and focus policy hardening.
3. Clipboard, file-pick, drag/drop integration contracts.
4. Font/text shaping support needed by graphical applications and workflow UIs.
5. Transfer/import/export policy hooks exposed as OS contracts.
6. Event queue QoS/backpressure/drop telemetry.

### P2 missing capabilities (full parity/hardening)

1. Accessibility tree and assistive-tech hooks.
2. Media pipeline capabilities (audio/video path contracts).
3. Untrusted application/process isolation hardening primitives.
4. Graphical userspace conformance gate with higher-level consumer suites layered on top.
5. Crash diagnostics and frame-time/memory observability at graphical-workload granularity.

## 3) Planning artifacts in this directory

- `00-boundary-decision.md` — architecture boundary and ownership rules (authoritative)
- `02-anxbrowserd-vs-platform-services.md` — explicit split between the `anxbrowserd` application daemon and reusable platform services
- `05-integration-contracts.md` — contract-level interfaces for globally available graphical userspace services
- `06-portability-and-adapter-requirements.md` — adapter boundary requirements keeping Anunix-Browser standalone
- `10-p0-ticket-plan.md` — detailed P0 tickets + deterministic tests
- `20-p1-ticket-plan.md` — P1 ticket/test plan
- `30-p2-ticket-plan.md` — P2 ticket/test plan
- `tickets.yaml` — machine-readable ticket index
- `acceptance.yaml` — machine-readable acceptance criteria

All files are structured for handoff to execution agents while preserving the application-vs-platform boundary above.