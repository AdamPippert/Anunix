# anxbrowserd vs Platform Services

Status: accepted design note
Date: 2026-04-23

## Core distinction

`anxbrowserd` is an application daemon.
It is not the graphical userspace.
It runs on top of the graphical userspace.

The Anunix graphical userspace is the reusable platform layer shared by all graphical applications and workflow-driven interfaces.

## What counts as platform service

Platform services are reusable OS capabilities such as:
- process/runtime isolation
- compositor/window/surface management
- input/event routing
- clipboard/drag-drop/file-pick contracts
- shared-memory and IPC contracts
- durable application-state storage
- media, accessibility, diagnostics, and test harness support

If a capability is required by `anxbrowserd` but cannot plausibly be used by another graphical application or workflow surface, it is probably not a platform service.

## What counts as anxbrowserd responsibility

`anxbrowserd` owns:
- browser daemon process topology
- browser runtime orchestration
- browser-facing state machines
- browser product UX and security policy
- adapter code that translates between browser-core needs and Anunix contracts

## Architectural rule

The dependency arrow goes one way:

`anxbrowserd` -> Anunix graphical userspace services

Never:

Anunix graphical userspace -> browser semantics

## Global availability requirement

Every service planned in this directory must be:
1. capability-gated,
2. documented as a general OS service,
3. available to workflow-driven interfaces and non-browser applications,
4. testable independently of `anxbrowserd`.

## Design smell checklist

These are red flags and should block a plan or PR:
- a compositor or input path named or scoped only for the browser
- a storage primitive justified only by browser profile format details
- browser-origin/site concepts appearing in OS API design
- a conformance gate that only validates browser behavior and not platform behavior
- a workflow/UI application being unable to use the same service contract

## Positive pattern

Use `anxbrowserd` as the stress test.
Ship only the generalized platform primitive.
Keep the application-specific policy in Anunix-Browser.
