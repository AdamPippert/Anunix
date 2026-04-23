# P2 Ticket Plan — graphical userspace hardening and ecosystem parity

Status: advisory only (planning), no implementation in this phase.
Goal: complete the graphical userspace surface needed for broad application compatibility and operational maturity.

Boundary guardrail (mandatory):
- This plan is constrained by `00-boundary-decision.md`.
- P2 scope is platform hardening contracts; application behavior and browser-engine/product behavior remain outside Anunix core.

## Ticket P2-001: Accessibility object model and assistive hooks

Objective
- Expose accessibility tree and actions as first-class objects/events.

Scope
- Accessibility tree export from surface/content nodes.
- Assistive action injection path with capability enforcement.
- Focus narration/event stream hooks.

Deterministic tests
1) Unit: accessibility tree schema conformance.
2) Integration: assistive action triggers equivalent interaction event.
3) Integration: focus-change events propagate to accessibility stream.

Definition of done
- Accessibility interactions are programmatic and testable, not ad-hoc.

---

## Ticket P2-002: Media path (audio/video) baseline

Objective
- Add platform media-path contracts needed by modern graphical applications and workflow surfaces.

Scope
- Audio output pipeline with timing guarantees.
- Video decode integration strategy (software baseline first).
- A/V sync control path.

Deterministic tests
1) Unit: audio buffer underrun recovery behavior.
2) Integration: fixed media fixture playback with bounded drift.
3) Integration: start/stop/seek state transitions deterministic.

Definition of done
- Media playback baseline is reliable enough for common graphical workloads.

---

## Ticket P2-003: Untrusted application/process isolation hardening primitives

Objective
- Introduce stronger security boundaries required for untrusted graphical and content-processing workloads.

Scope
- Isolated process classes per trust/content domain policy.
- Tight capability boundaries across application component processes.
- IPC policy matrix and deny-by-default enforcement.

Deterministic tests
1) Unit: forbidden cross-domain shared-memory map denied.
2) Integration: compromised process simulation cannot access unrelated application data path.
3) Integration: policy violation events are logged with provenance.

Definition of done
- Isolation primitives materially raise security posture for untrusted graphical workloads.

---

## Ticket P2-004: Graphical userspace conformance gate integration

Objective
- Move from ad-hoc parity checks to a standardized graphical-userspace compatibility gate.

Scope
- Curated platform fixture corpus and runner integration.
- Baseline comparison against reference application outputs (including browser-facing fixtures where relevant).
- Regression budget policy (allowed deltas, fail thresholds).

Deterministic tests
1) Runner determinism: same corpus + seed => identical report.
2) Diff determinism: changed behavior isolated to explicit test IDs.
3) Gate behavior: threshold breach fails pipeline with machine-readable reason.

Definition of done
- Compatibility regressions are automatically surfaced and blocked.

---

## Ticket P2-005: Crash diagnostics + performance observability

Objective
- Provide deep operational visibility for graphical userspace integration failures.

Scope
- Crash dump format and symbolized reporting.
- Trace timeline for frame/input/network critical paths.
- Memory/CPU/frame-time counters exposed to tooling.

Deterministic tests
1) Unit: synthetic fault produces parseable crash artifact.
2) Integration: trace captures known benchmark phases in order.
3) Integration: metric schema remains backward compatible.

Definition of done
- Failures and regressions can be diagnosed quickly without manual forensic guesswork.

---

## P2 execution order

1. P2-005 Crash diagnostics + observability
2. P2-003 Untrusted application/process isolation hardening
3. P2-001 Accessibility object model
4. P2-002 Media path baseline
5. P2-004 Graphical userspace conformance gate

Rationale
- Operational and security foundation before expensive parity scaling.
