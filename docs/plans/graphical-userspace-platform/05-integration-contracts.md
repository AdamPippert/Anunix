# Anunix Graphical Userspace Service Contracts

Status: draft planning contract
Date: 2026-04-23

## Contract objective

Define stable, globally available graphical userspace service contracts in Anunix.
`anxbrowserd` is one consumer of these contracts. Workflow surfaces, editors, dashboards, object browsers, and future graphical applications must consume the same substrate.

## Contract rules

1. Contracts are interface-level, not implementation-level.
2. Contracts must be versioned.
3. Contracts must have deterministic tests.
4. Contracts must not encode browser business logic.
5. Contracts must be globally consumable by graphical applications/workflows, not private to `anxbrowserd`.

## Contract set

### C0. Graphical service availability contract

Provided by Anunix:
- System-wide registration/discovery of graphical services
- Capability-gated access to compositor/input/media/storage services
- Stable service lifecycle visible to all graphical applications/workflows

Consumed by:
- `anxbrowserd`
- Workflow-driven UIs
- Native graphical applications and tooling

### C1. Process + execution contract

Provided by Anunix:
- Userspace process isolation
- Executable loading (ELF64 baseline)
- Syscall ABI v0

Consumed by:
- `anxbrowserd`
- Any graphical application daemon or helper process

Versioning:
- `ANX_ABI_MAJOR.MINOR` (major breaks compatibility)

### C2. Display/compositor/window contract

Provided by Anunix:
- Surface lifecycle, z-order, focus, repaint scheduling
- Window/transient-parent relationships and activation rules
- Compositor runtime guarantees

Consumed by:
- `anxbrowserd`
- Workflow editor/object viewer/shell windows
- Any graphical application that presents surfaces

Versioning:
- Interface Plane contract version (`ANX_IFACE_API_V*`)

### C3. Input/event contract

Provided by Anunix:
- Keyboard/pointer event schema
- Monotonic timestamps
- Ordering and backpressure behavior
- Clipboard, drag/drop, file-pick, and related interaction contracts

Consumed by:
- `anxbrowserd`
- Workflow and desktop applications

### C4. Shared-memory/IPC contract

Provided by Anunix:
- Shared buffer mapping rights
- Notification channel
- Capability-enforced access

Consumed by:
- `anxbrowserd`
- Any graphical application exchanging frames/data with compositor or peers

### C5. Security/transport contract

Provided by Anunix:
- TLS validation primitives
- Trust-store lifecycle APIs

Consumed by:
- `anxbrowserd`
- Any userspace application requiring OS trust plumbing

### C6. Durable application-state contract

Provided by Anunix:
- Atomic write, lock semantics, crash recovery primitives
- Persistent object/storage rules for session/cache/state data

Consumed by:
- `anxbrowserd`
- Workflow editors, graphical tools, and any stateful application

### C7. Media/accessibility/diagnostics contract

Provided by Anunix:
- Media path primitives
- Accessibility hooks
- Trace hooks, crash artifacts, perf counters, queue telemetry

Consumed by:
- `anxbrowserd`
- All graphical applications/workflows needing media, assistive, or diagnostics support

### C8. Conformance/reporting contract

Provided by Anunix:
- Deterministic harness execution and artifact schema
- Platform fixture suites for graphical userspace behavior
- Optional higher-level consumer suites (including browser-facing regression suites)

Consumed by:
- CI
- `anxbrowserd`
- Other graphical applications/workflows

## Out-of-contract domains

Owned by Anunix-Browser:
- HTML/CSS/JS semantics
- DOM/layout/rendering behavior
- Browser UX policy and product decisions
- Web feature behavior above OS contract layer
- Browser-specific site/origin policy

Owned by individual applications/workflows:
- App-specific view models and business logic
- App-private document/session semantics above generic storage contracts
- Consumer-specific navigation/state policy

## Compatibility target

Anunix graphical userspace services should support:
- `anxbrowserd` via an Anunix adapter,
- first-party workflow applications,
- non-browser graphical apps,
while keeping consumer logic outside the OS.

That portability and generality requirement is a design constraint, not a best effort.
