# Boundary Decision: anxbrowserd vs Anunix Graphical Userspace

Status: accepted
Date: 2026-04-23
Decision owner: platform architecture

## Decision statement

Anunix owns the graphical userspace substrate as a general operating capability.
`anxbrowserd` is an application daemon owned by the standalone Anunix-Browser project.

The platform work in this directory exists to build globally available graphical userspace services for all applications and workflows. `anxbrowserd` is a demanding consumer and forcing function, not a privileged subsystem.

## Why this decision exists

1. Keep Anunix focused on OS primitives, policy, and reusable system services.
2. Prevent browser-engine and browser-product logic from leaking into the OS.
3. Make every dependency justified by `anxbrowserd` reusable by non-browser graphical applications and workflow surfaces.
4. Preserve portability: Anunix-Browser must remain runnable on non-Anunix targets.
5. Preserve composability: Workflow UIs, object viewers, editors, dashboards, and future apps must all consume the same platform contracts.

## Explicit ownership matrix

Anunix owns:
- Process/isolation primitives
- Executable loading and syscall ABI contracts
- Compositor/window/surface primitives and event guarantees
- Input routing, clipboard, drag/drop, file-pick, and dialog contracts
- IPC/shared-memory primitives and capability checks
- TLS/trust store and durable storage primitives
- Media, accessibility, observability, and conformance harness primitives
- System-wide discovery, policy, and capability gating for graphical services

Anunix-Browser owns:
- `anxbrowserd` application behavior and lifecycle
- HTML parser
- CSS parser/layout/style engine
- JavaScript engine/runtime embedding
- DOM, rendering tree, painting policy
- Navigation/session/history logic
- Browser UX/product behaviors (tabs, omnibox behavior, settings, extension model)
- Browser networking policy above OS transport/security primitives
- Web platform semantics and conformance behavior
- Site/origin policy specific to browser security model

## Hard non-goals for Anunix (do not implement)

- No in-kernel or core-userland browser engine.
- No HTML/CSS/JS execution semantics in Anunix core.
- No browser business logic disguised as a platform helper.
- No `anxbrowserd`-private OS path that is unavailable to other graphical applications/workflows.
- No browser-specific concepts (tab model, origin model, history model, DOM model) baked into core graphical userspace APIs.

## Required design tests for every new ticket

Each ticket touching this area must answer:

1. Is this an OS primitive/contract or `anxbrowserd`/browser behavior?
2. Can `anxbrowserd` consume this via a stable interface without embedding browser logic into Anunix?
3. Can at least one non-browser graphical application or workflow justify this primitive independently?
4. Is the resulting service globally available to graphical applications/workflows rather than private to one consumer?
5. Does this keep Anunix-Browser portable to non-Anunix environments?

If any answer fails, the ticket must move to Anunix-Browser or be redesigned.

## Enforcement

- All ticket plans in this directory are constrained by this file.
- `acceptance.yaml` includes boundary and global-availability guardrail checks.
- PR reviews for graphical-userspace changes must include explicit notes on reusable platform scope.

## Change control

This decision can be revised only by a dedicated design update document that:
- states the proposed boundary change,
- shows measurable benefit,
- lists blast radius and migration impact,
- includes rollback strategy.
