# Anunix Browser Graphical Mode Prerequisites (Advisory Plan)

Status: advisory only
Scope: planning artifacts only, no implementation work
Target: userspace prerequisites in `~/Development/Anunix`
Date: 2026-04-18

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

## 2) Critical gap: what is still missing for a real graphical browser in userspace

The current codebase has interface scaffolding, but it is not yet a complete userspace-capable browser host platform.

### P0 missing capabilities (must-have)

1. Userspace process/runtime isolation suitable for browser components
   - Current limitation is explicit: kernel is effectively single-address-space for builtins and POSIX mode is a thin shim.
   - Evidence: `docs/tool-plan.md` line 9, `kernel/include/anx/posix.h`, `kernel/core/posix/posix.c` (`exec` stub).

2. Syscall/ABI + executable loading path for userspace browser processes
   - Needed to run browser runtime outside kernel builtins.

3. Compositor as real Execution Cell runtime (not just repaint helper)
   - Current implementation is synchronous helper function, minimal focus policy.

4. Input routing hardening (hit testing, focus guarantees, timestamps, event ordering guarantees)
   - Current events have timestamp placeholder (`0` in input path).

5. Browser-grade secure networking baseline in-kernel/userspace boundary
   - TLS + certificate chain/host validation + trust store lifecycle.

6. Profile/cache storage semantics required by browser session data
   - Durable profile writes, locking behavior, corruption recovery policy.

7. Shared-memory / low-copy IPC path for renderer/compositor/browser component boundaries
   - Needed before multi-process browser architecture is practical.

8. Deterministic conformance/perf harness integrated with Anunix build/test loop
   - Needed to compare behavior vs Chromium/Firefox baselines continuously.

### P1 missing capabilities (should-have for parity/perf)

1. GPU/compositor acceleration path with dirty-rect efficiency.
2. Multi-window/tab surface management semantics.
3. Clipboard, file-pick, drag/drop integration.
4. Font/text shaping and internationalization correctness path.
5. Download pipeline and policy checks.
6. Better event queue QoS/backpressure/drop telemetry.

### P2 missing capabilities (full parity/hardening)

1. Accessibility tree and assistive-tech hooks.
2. Audio/video media path for modern sites.
3. Site/process isolation hardening profile.
4. WPT-class conformance gate and longitudinal regression reports.
5. Crash diagnostics and frame-time/memory observability at browser workload granularity.

## 3) Planning artifacts in this directory

- `10-p0-ticket-plan.md` — detailed P0 tickets + deterministic tests
- `20-p1-ticket-plan.md` — P1 ticket/test plan
- `30-p2-ticket-plan.md` — P2 ticket/test plan

All files are structured for handoff to a cheaper execution agent.
