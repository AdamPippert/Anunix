# P1 Ticket Plan — graphical userspace parity and usability expansion

Status: advisory only (planning), no implementation in this phase.
Goal: build on P0 substrate to provide robust graphical userspace contracts for all applications and workflows.

Boundary guardrail (mandatory):
- This plan is constrained by `00-boundary-decision.md`.
- P1 scope is reusable graphical userspace runtime substrate, not application-specific or browser-engine/product logic.

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

## Ticket P1-002: Multi-window/surface model hardening

Objective
- Add first-class support for multiple externally-managed application surfaces with robust z-order/focus semantics.

Scope
- Parent/child surface semantics validation.
- Deterministic z-order operations (raise/lower/focus transfer).
- Surface grouping for related windows/transient sets.

Deterministic tests
1) Unit: z-order operations produce expected ordering.
2) Integration: focus follows active top-level surface.
3) Integration: closing active surface transfers focus deterministically.

Definition of done
- Multi-surface application behavior is stable and test-covered.

---

## Ticket P1-003: Clipboard, drag/drop, file-pick interface contracts

Objective
- Provide minimum desktop interaction APIs expected by graphical applications and workflow UIs.

Scope
- Clipboard read/write API with capability checks.
- Drag/drop event flow for files/text.
- File-picker request/response contract.

Deterministic tests
1) Unit: unauthorized clipboard access denied.
2) Integration: copy/paste roundtrip for text payload.
3) Integration: drag/drop file metadata reaches target surface.

Definition of done
- Core interaction APIs required by modern graphical workflows/applications are present.

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

## Ticket P1-005: Transfer/import/export policy hooks + object integration

Objective
- Expose OS-level transfer/import/export policy hooks so applications can enforce artifact movement policy without embedding platform logic in Anunix.

Scope
- Destination policy and capability checks for artifact writes.
- Resume/retry storage contract primitives.
- Metadata/provenance hooks for externally-managed transfer/import/export workflows.

Deterministic tests
1) Unit: invalid destination policy rejected.
2) Integration: interrupted transfer resumes and final hash matches expected.
3) Integration: object metadata/provenance recorded for completed transfer/import/export artifacts.

Definition of done
- Transfer artifact behavior is deterministic, auditable, and recoverable via OS contracts.

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
3. P1-002 Multi-window/surface model
4. P1-003 Clipboard/drag-drop/file-pick
5. P1-004 Text/font shaping baseline
6. P1-005 Transfer/import/export policy hooks

Rationale
- Observability and scheduling first, then reusable application-facing integrations after stability work.
