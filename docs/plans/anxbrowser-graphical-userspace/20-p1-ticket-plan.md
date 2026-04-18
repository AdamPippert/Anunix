# P1 Ticket Plan — parity and usability expansion

Status: advisory only (planning), no implementation in this phase.
Goal: build on P0 substrate to reach practical graphical browser operation.

## Ticket P1-001: Dirty-rect compositor + frame pacing

Objective
- Move from full-surface commits toward damage-based redraw and stable frame pacing.

Scope
- Track dirty regions per surface.
- Commit only damaged regions.
- Introduce frame pacing control for predictable latency.

Deterministic tests
1) Unit: damage region merge correctness.
2) Integration: unchanged frame causes zero-render fast path.
3) Integration: measured commit count decreases under partial updates.

Definition of done
- Repaint path is damage-driven and measurably more efficient.

---

## Ticket P1-002: Multi-window/tab surface model hardening

Objective
- Add first-class support for multiple browser surfaces with robust z-order/focus semantics.

Scope
- Parent/child surface semantics validation.
- Deterministic z-order operations (raise/lower/focus transfer).
- Surface grouping for tab stacks.

Deterministic tests
1) Unit: z-order operations produce expected ordering.
2) Integration: focus follows active top-level tab surface.
3) Integration: closing active tab transfers focus deterministically.

Definition of done
- Multi-surface browser session behavior is stable and test-covered.

---

## Ticket P1-003: Clipboard, drag/drop, file-pick interface contracts

Objective
- Provide minimum desktop interaction APIs expected by real web apps.

Scope
- Clipboard read/write API with capability checks.
- Drag/drop event flow for files/text.
- File-picker request/response contract.

Deterministic tests
1) Unit: unauthorized clipboard access denied.
2) Integration: copy/paste roundtrip for text payload.
3) Integration: drag/drop file metadata reaches target surface.

Definition of done
- Core interaction APIs required by modern web flows are present.

---

## Ticket P1-004: Text/font shaping correctness baseline

Objective
- Ensure text rendering is not limited to basic ASCII assumptions.

Scope
- Unicode codepoint handling path.
- Font fallback mechanism.
- Basic bidi/shaping integration strategy (v1 scope can leverage existing shaper integration plan in userspace).

Deterministic tests
1) Unit: UTF-8 decode validity and rejection vectors.
2) Integration: multilingual text fixtures render without crashes and with stable glyph counts.
3) Integration: fallback font path triggers deterministically when glyph missing.

Definition of done
- Text path is no longer ASCII-only fragile.

---

## Ticket P1-005: Download pipeline + object policy integration

Objective
- Browser downloads become first-class managed objects with policy controls.

Scope
- Download lifecycle state machine.
- Destination policy and capability checks.
- Partial download resume/retry policy.

Deterministic tests
1) Unit: invalid destination policy rejected.
2) Integration: interrupted download resumes and final hash matches expected.
3) Integration: object metadata/provenance recorded for completed downloads.

Definition of done
- Download behavior is deterministic, auditable, and recoverable.

---

## Ticket P1-006: Event queue QoS and telemetry

Objective
- Make event infrastructure observable and tunable under load.

Scope
- Queue depth metrics, drop counters, latency histogram.
- Priority classes (input > cosmetic updates).
- Backpressure policy with explicit thresholds.

Deterministic tests
1) Unit: priority dequeue order under mixed workload.
2) Integration: overload scenario preserves high-priority input event delivery.
3) Integration: telemetry counters match known injected workload.

Definition of done
- Event subsystem behavior under stress is measurable and predictable.

---

## P1 execution order

1. P1-006 Event queue QoS and telemetry
2. P1-001 Dirty-rect compositor + frame pacing
3. P1-002 Multi-window/tab surface model
4. P1-003 Clipboard/drag-drop/file-pick
5. P1-004 Text/font shaping baseline
6. P1-005 Download pipeline

Rationale
- Observability and scheduling first, user-facing integrations after stability work.
