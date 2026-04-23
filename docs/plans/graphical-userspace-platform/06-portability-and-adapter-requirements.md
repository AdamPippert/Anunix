# Portability + Adapter Requirements (Anunix-Browser Standalone Constraint)

Status: planning requirements
Date: 2026-04-23

## Objective

Guarantee Anunix-Browser remains a standalone browser project that can run on Anunix and non-Anunix targets via adapters.

## Required architecture shape

Anunix-Browser runtime must use a platform adapter boundary, not direct Anunix-only calls throughout core browser code.

Recommended split:

- `browser-core/` (portable)
  - DOM/layout/rendering
  - JS runtime embedding
  - navigation/session/history
  - browser product features
- `platform-adapters/anunix/` (Anunix-specific)
  - process model mapping
  - window/surface integration
  - input/event mapping
  - storage + trust-store wiring
- `platform-adapters/posix|linux|macos|windows/` (non-Anunix ports)

## Adapter contract categories

1. Process/runtime lifecycle
2. Display/surface presentation
3. Input/events
4. Network transport + trust plumbing
5. Storage/profile durability
6. IPC/shared memory
7. Telemetry/crash reporting

## Portability acceptance criteria

1. Browser core compiles without including Anunix headers directly.
2. Anunix-specific includes/imports are confined to Anunix adapter module(s).
3. At least one non-Anunix adapter target remains buildable in CI.
4. Feature development in browser core does not require Anunix-specific conditionals.
5. Integration tests validate identical browser-core behavior across at least two adapter targets for selected fixture suites.

## Failure conditions (must block changes)

- Any PR that introduces Anunix-only behavior into browser core paths.
- Any PR that requires Anunix kernel internals to implement browser product logic.
- Any PR that bypasses adapter interfaces for convenience.

## Design review checklist

For each browser-adjacent design:

- Is this change an OS substrate concern or browser behavior concern?
- If browser behavior, why is this not in Anunix-Browser?
- If OS substrate, is the contract generic enough for non-browser clients?
- Does this keep browser-core portable across non-Anunix targets?

If answers are unclear, stop and re-scope before implementation.