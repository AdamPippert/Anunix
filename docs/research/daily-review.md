# Daily AI-first OS research review

The review covers 73 briefings from June 26 through September 6, 2026.
The user confirmed one briefing per day in this sequence.

Source: [AI-first OS research and Project Solara updates](https://chatgpt.com/c/6a3eabf4-cb88-83ea-942e-a601dd12b392).
Each entry retains the source message identifier.
Research learning below summarizes the conversation; performance claims from papers are not independently reproduced here.

The initial comparison uses Anunix commit `543ada0`.
Each daily branch updates its comparison against the preceding merged result.
Anunix is a native C kernel; Linux-specific mechanisms need native equivalents or an explicit applicability decision.

Acceptance statements describe planned tests, not completed validation.
A day remains queued until the preceding feature passes its Jekyll virtual machine test and merges.
A successful host test does not establish successful guest execution.

## Day 001 — 2026-06-26

**AI-First OS Concepts: Current Research Map**

Learning: Keep model proposals behind deterministic enforcement of identity, authority, resources, and effects.

Anunix comparison: Cells, capabilities, and memory tiers exist. The external-call runtime bypasses its declared side-effect restriction.

Evidence: [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c).

Planned acceptance: Deny an unauthorized external handler before invocation; preserve authorized execution and an audit trace.

Status: Merged at `b946a76` after Jekyll validation. Tested commit: `23f1e4f`. Source message: `38205a54-ff49-4d7e-90a3-354c4d155e1c`.

Detailed comparison and results: [Day 001](day-001.md).

## Day 002 — 2026-06-27

**AI-First OS Research Brief**

Learning: Convert repeated successful workflows into reusable, validated artifacts with explicit promotion and rollback.

Anunix comparison: The baseline imports bounded binary bundles but does not validate strings, ports, or graph structure. Day 2 adds validation before library registration.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/workflow/wf_bundle.c](../../kernel/core/workflow/wf_bundle.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Reject malformed workflow artifacts before registration; exercise a valid packaged workflow in the guest.

Status: Merged at `10f5213` after Jekyll validation. Tested commit: `48673fa`. Source message: `e2f56565-6637-4d84-acee-920ffaf5e9d8`.

Detailed comparison and results: [Day 002](day-002.md).

## Day 003 — 2026-06-28

**AI-First OS Exploration: Control Planes, Kernels, and Scheduling**

Learning: Govern the scarce resources of an agent task, including tools, tokens, memory, and permissions.

Anunix comparison: The baseline reserves engine memory but ignores model-request deadlines and output limits. Day 3 enforces request admission and forwards cell limits.

Evidence: [kernel/core/route/budget.c](../../kernel/core/route/budget.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Exercise admission and release under bounded resources; reject work that exceeds its declared budget.

Status: Merged at `f4b0172` after Jekyll validation. Tested commit: `ae7c64a`. Source message: `0a562c41-3184-421b-83a1-5f44f5af80c9`.

Detailed comparison and results: [Day 003](day-003.md).

## Day 004 — 2026-06-29

**AI-First OS Research Update: Agentic Control Planes + AI-Tuned Scheduling**

Learning: Profile workloads before generating policies, then compare candidates against a fixed incumbent.

Anunix comparison: The baseline promotion gate can overflow score margins and thresholds. It does not resolve the declared incumbent. Day 4 widens arithmetic and checks incumbent installation.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/regime/regime.c](../../kernel/core/regime/regime.c), [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c).

Planned acceptance: Reject a candidate indistinguishable from the incumbent and preserve the incumbent after rejection.

Status: Merged at `2fd7c18` after Jekyll validation. Tested commit: `0f0faef`. Source message: `1275902e-2f68-4578-9a29-56937f0cdd6e`.

Detailed comparison and results: [Day 004](day-004.md).

## Day 005 — 2026-06-30

**AI-First OS Exploration: Agent-Native Scheduling and Tunable Kernels**

Learning: Package observation, benchmarking, policy promotion, and rollback as auditable workflows.

Anunix comparison: The baseline has workflow execution and a capability promotion gate but no connection between them. Day 5 adds a permission-gated workflow step with sealed evidence and retained decisions.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c).

Planned acceptance: Execute an optimization workflow with explicit evidence and retain the prior policy after a failed gate.

Status: Merged at `35d0245` after Jekyll validation. Tested commit: `42c7ca2`. Source message: `cfc2060b-2b88-4876-a57d-ff393f2e5f9a`.

Detailed comparison and results: [Day 005](day-005.md).

## Day 006 — 2026-07-01

**AI-First OS Update: Agent libOS + Tool-Call-Aware Resource Control**

Learning: Give tool calls their own resource scope within an agent lifecycle.

Anunix comparison: The baseline ignores recursion permission, configured child limits, cognitive inheritance, and slot cleanup. Day 6 enforces scoped derivation and ancestry admission, then returns slots on child destruction.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c).

Planned acceptance: Check that a child tool call cannot exceed its parent scope and releases resources on failure.

Status: Merged at `2409aff` after Jekyll validation. Tested commit: `f181a9a`. Source message: `eb3a9f70-77e4-4830-a53d-3ad1d8b603a2`.

Detailed comparison and results: [Day 006](day-006.md).

## Day 007 — 2026-07-02

**AI-First OS Exploration: Agent libOS + Tool-Call Scheduling**

Learning: Schedule and account for the tool invocation, where short resource spikes occur.

Anunix comparison: The baseline drops the materialized trace identifier and has no per-tool timing or byte accounting. Day 7 records those values and tests child slot ownership and release.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/route/budget.c](../../kernel/core/route/budget.c).

Planned acceptance: Record per-tool timing, byte counts and outcomes; verify child slot acquisition and release through failure and success.

Status: Merged at `7985529` after Jekyll validation. Tested commit: `2fcac65`. Source message: `7689695c-608c-474b-b3d2-bc21f885aae4`.

Detailed comparison and results: [Day 007](day-007.md).

## Day 008 — 2026-07-03

**AI-First OS Exploration: Agent Runtime Meets Kernel Resource Control**

Learning: Separate agent identity, task branches, tool calls, and transport primitives.

Anunix comparison: The baseline omits parent and plan identifiers and leaves trace payloads writable. Day 8 preserves those identifiers and seals materialized traces.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c).

Planned acceptance: Exercise a child call through the runtime while preserving lineage and enforcing delegated authority.

Status: Merged at `ca47725` after Jekyll validation. Tested commit: `2dd72f9`. Source message: `7ae8ff25-29ce-435f-8a3c-f20a4e699dcc`.

Detailed comparison and results: [Day 008](day-008.md).

## Day 009 — 2026-07-04

**AI-First OS Update: Policy Enforcement Becomes the Control Plane**

Learning: Enforce policy beneath model-visible tools, where an action actually reaches the system.

Anunix comparison: The baseline treats object creator identity as caller authority. Day 9 supplies the active cell identity and enforces separate read and write handle permissions.

Evidence: [kernel/core/state/access.c](../../kernel/core/state/access.c), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c).

Planned acceptance: Attempt a prohibited operation through the ordinary dispatch path and establish that no effect occurs.

Status: Merged at `21f7d71` after Jekyll validation. Tested commit: `cab5076`. Source message: `71a4dede-4ed0-409a-b882-c241fa4c4867`.

Detailed comparison and results: [Day 009](day-009.md).

## Day 010 — 2026-07-05

**AI-First OS Update: Enforcement, Not Autonomy, Is Becoming the Core**

Learning: Admission, cancellation, and reaping belong to the runtime, alongside resource scheduling.

Anunix comparison: The baseline leaves descendants and duplicate queue entries after cancellation. Day 10 cancels queue ownership, preserves terminal runs, and reaps eligible child tasks.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/sched/scheduler.c](../../kernel/core/sched/scheduler.c), [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c).

Planned acceptance: Cancel queued work and establish that no runnable entry or resource owner survives cancellation.

Status: Merged at `a8fe0cc` after Jekyll validation. Tested commit: `fda8c2d`. Source message: `ec9c71c0-8871-450a-944e-a28f73b3e2dd`.

Detailed comparison and results: [Day 010](day-010.md).

## Day 011 — 2026-07-06

**AI-First OS Update: The Stack Is Converging Around Enforcement**

Learning: Treat memory as a governed resource alongside agent execution and tool admission.

Anunix comparison: The baseline admits memory entries without a cell byte ceiling. Day 11 checks sealed payload admission against an inherited limit and retains its accounting owner until forgetting.

Evidence: [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c), [kernel/include/anx/memory.h](../../kernel/include/anx/memory.h), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Reject excessive admission while preserving an existing cell's usable memory.

Status: Merged at `c20f683` after Jekyll validation. Tested commit: `4ef9177`. Source message: `e231e283-bef2-43f6-8daa-b98afc7755e8`.

Detailed comparison and results: [Day 011](day-011.md).

## Day 012 — 2026-07-07

**AI-First OS Update: The Control Plane Is Becoming Concrete**

Learning: Make lifecycle and resource enforcement observable through one typed execution boundary.

Anunix comparison: The baseline permits external calls without saved traces and records generic denials. Day 12 reserves audit objects, records typed gates, and reports audit failure separately from completed execution.

Evidence: [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/route/budget.c](../../kernel/core/route/budget.c).

Planned acceptance: Produce a denial trace that identifies the failed gate and records no successful external effect.

Status: Merged at `242fe58` after Jekyll validation. Tested commit: `1e65c12`. Source message: `afc05449-e490-4a36-8b73-13994999e7b0`.

Detailed comparison and results: [Day 012](day-012.md).

## Day 013 — 2026-07-08

**AI-First OS Update: Kernel Tunability + Policy-as-Code**

Learning: Represent tunable policy as a typed artifact with bounded revision authority.

Anunix comparison: The baseline accepts negative scoring divisors and unchecked weights or snapshots. Day 13 validates policy and snapshot bounds before simulation, preserving the previous result on rejection.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c).

Planned acceptance: Reject malformed simulation inputs without changing the previous result. A valid alternate candidate changes the simulated winner while replaying the incumbent reproduces its original result. Live policy activation remains outside this step.

Status: Merged at `57873d0` after Jekyll validation. Tested commit: `a199e97`. Source message: `5048a664-da01-470d-a8f3-841629df80a5`.

Detailed comparison and results: [Day 013](day-013.md).

## Day 014 — 2026-07-09

**AI-First OS Update: Observability Is Becoming the Missing Plane**

Learning: Connect agent intent to the system effects and resource changes it caused.

Anunix comparison: The baseline retains caller identifiers and outcomes but uses a generic creation event. Day 14 preserves the intent name before dispatch or cancellation and verifies causal records after cell destruction.

Evidence: [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c).

Planned acceptance: Trace an allowed effect and a denied effect back to separate requesting cells.

Status: Merged at `09a3718` after Jekyll validation. Tested commit: `c383dca`. Source message: `d2eef052-ef2c-4ac2-9fd2-5230b7b5aa2e`.

Detailed comparison and results: [Day 014](day-014.md).

## Day 015 — 2026-07-10

**AI-First OS Update: Scheduling Moves Above the Request Boundary**

Learning: Place sessions using continuity and locality, while different control loops run at appropriate timescales.

Anunix comparison: The baseline scores each route independently. Day 15 adds an optional session planner that retains eligible affinity, rechecks a declared backend pool, and preserves prior state on rejection.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Preserve an eligible session's engine affinity while rejecting an unavailable engine.

Status: Merged at `658e0f5` after Jekyll validation. Tested commit: `84d5cce`. Source message: `5437e7e4-38ad-4556-98c7-08606daa9991`.

Detailed comparison and results: [Day 015](day-015.md).

## Day 016 — 2026-07-11

**AI-First OS Update: Tool Calls and Inference Gangs Become Kernel Objects**

Learning: Mediate tool calls as governed operations and change inference resources only at safe boundaries.

Anunix comparison: The baseline checks authority only at effect preparation. Day 16 rechecks owner identity, lifecycle, permission, and data policy before dispatch while preserving settlement after dispatch begins.

Evidence: [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Reject revoked authority and changed data policy before dispatch. Preserve the prepared phase on rejection and permit outcome settlement after dispatch begins.

Status: Merged at `becb198` after Jekyll validation. Tested commit: `0987f34`. Source message: `e5d6a0c6-bee9-48f7-bff4-00780959b7fb`.

Detailed comparison and results: [Day 016](day-016.md).

## Day 017 — 2026-07-12

**AI-First OS Update: Context Becomes a Protected, Schedulable Resource**

Learning: Protect model-visible context by trust, version, scope, and residency.

Anunix comparison: The baseline bypasses metadata access policies in ICM. Day 17 enforces metadata permissions for views, catalogs, tags, and publication while keeping annotations separate from access authority.

Evidence: [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c), [kernel/core/state/access.c](../../kernel/core/state/access.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c).

Planned acceptance: Deny unauthorized ICM projection and mutation. A public authority annotation must not grant access to protected metadata or payload, and authorized sealed-object annotations must remain usable.

Status: Merged at `01c91d6` after Jekyll validation. Tested commit: `a356f48`. Source message: `3d6016a3-1b0f-4d3e-9bc6-bca4af973f93`.

Detailed comparison and results: [Day 017](day-017.md).

## Day 018 — 2026-07-13

**AI-First OS Update: Add a Slow Kernel-Configuration Loop**

Learning: Separate build configuration, runtime policy, and scheduler dispatch into distinct optimization loops.

Anunix comparison: The baseline records image hashes without a compiled configuration profile. Day 18 binds architecture and research mode to an artifact digest and checks the running guest.

Evidence: [kernel/core/sched/scheduler.c](../../kernel/core/sched/scheduler.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Bind the compiled architecture and research mode to the tested image; reject incompatible requirements and changed artifacts.

Status: Merged at `074b3ec` after Jekyll validation. Tested commit: `8311951`. Source message: `0f7ae5cd-ece1-4b5f-917c-cdcd0ae3339e`.

Detailed comparison and results: [Day 018](day-018.md).

## Day 019 — 2026-07-14

**AI-First OS Update: Separate the Immutable Authority Kernel from the Self-Evolving Capability Plane**

Learning: Allow capabilities to evolve while keeping their authority ceiling under independent control.

Anunix comparison: The baseline checks lifecycle and promotion scores without an independent authority ceiling. Day 19 checks private grants, installed lineage scope, and the promoting cell before registration.

Evidence: [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/core/credential.c](../../kernel/core/credential.c), [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c).

Planned acceptance: Reject ungranted or expanded execution authority; accept a scored replacement within the incumbent scope and the caller permissions.

Status: Validated on Jekyll. Tested commit: `387ddc4`. Source message: `c20ed7d5-7da3-4ba6-ba26-549efe59e0c5`.

Detailed comparison and results: [Day 019](day-019.md).

## Day 020 — 2026-07-15

**AI-First OS Update: Semantic Memory Metadata Is Becoming a Scheduling ABI**

Learning: Export semantic retention hints while the memory subsystem owns physical placement.

Anunix comparison: Memory tiers and decay exist. Semantic prompt regions are not automatically physical KV-cache regions.

Evidence: [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/include/anx/memory.h](../../kernel/include/anx/memory.h).

Planned acceptance: Exercise bounded retention hints and preserve protected state during eviction.

Status: Queued. Source message: `d2928218-8709-4a8b-8251-f76e37b0577c`.

## Day 021 — 2026-07-16

**AI-First OS Update: Semantic Control Loops Are Moving from Theory to Working Systems**

Learning: Use typed actions in slower control loops; retain deterministic resource ownership.

Anunix comparison: The Twin and regime detector support observation and gating. A general typed actuation loop remains separate.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/regime/regime.c](../../kernel/core/regime/regime.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Reject an out-of-range action and restore the prior policy after a failed trial.

Status: Queued. Source message: `4d0e3645-bcdc-41c0-bb55-a3e4cd28e04e`.

## Day 022 — 2026-07-17

**AI-First OS Update: Identity Becomes a Signed Lifecycle Boundary**

Learning: Keep persistent agent identity separate from model, process, and host identity.

Anunix comparison: Credentials and cell identities exist. Persona Objects remain a design document rather than a complete lifecycle service.

Evidence: [kernel/core/credential.c](../../kernel/core/credential.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Require an authorized identity transition before widening the persistent mandate.

Status: Queued. Source message: `e3b5929c-297d-4584-a409-2ba221be3fa4`.

## Day 023 — 2026-07-18

**AI-First OS Update: The GUI Becomes a Semantic I/O Subsystem**

Learning: Expose graphical controls as typed objects with state and verifiable actions.

Anunix comparison: The interface plane and accessibility structures exist. Raw coordinates do not establish semantic action validity.

Evidence: [kernel/include/anx/interface_plane.h](../../kernel/include/anx/interface_plane.h), [kernel/core/state/access.c](../../kernel/core/state/access.c).

Planned acceptance: Reject an action on a stale or unauthorized control and validate an allowed action's resulting state.

Status: Queued. Source message: `18e4cfa8-3933-472b-900e-800cd425e87a`.

## Day 024 — 2026-07-19

**AI-First OS Update: “Pause” Must Be an Effect Fence**

Learning: A pause must fence effects across the relevant execution scope.

Anunix comparison: Cells and workflows can wait or cancel. The effect protocol has its own independent phase transitions.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c).

Planned acceptance: Pause one branch and establish that sibling effects cannot cross the same scope's fence.

Status: Queued. Source message: `38396083-009d-4719-87bb-27720ab59c9d`.

## Day 025 — 2026-07-20

**AI-First OS Update: Tool Catalogs Become a Virtual I/O Namespace**

Learning: Expose a bounded tool namespace through capability handles instead of loading every schema into context.

Anunix comparison: The capability registry and object catalog exist. Discovery and invocation need a common authority boundary.

Evidence: [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c), [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c).

Planned acceptance: Reject invocation through a revoked handle and bound the visible catalog to authorized entries.

Status: Queued. Source message: `b3e419a3-edae-45ea-9b87-b3975f94202c`.

## Day 026 — 2026-07-21

**AI-First OS Update: Schedule Continuity, Not Isolated Requests**

Learning: Schedule continuation state using dependencies, residency, and expected reuse.

Anunix comparison: The Twin captures engines and queue depths. It does not capture the complete continuation or KV state.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Prefer usable resident state without selecting an engine that violates admission constraints.

Status: Queued. Source message: `8f38db3a-f37c-44db-be06-1612a846def1`.

## Day 027 — 2026-07-22

**AI-First OS Update: Skills Become Schedulable Behavioral Packages**

Learning: Treat skills as versioned behavioral packages with dependencies, permissions, and evaluations.

Anunix comparison: Workflow bundles and capabilities provide packaging components. A skill manifest alone supplies no enforcement.

Evidence: [kernel/core/workflow/wf_bundle.c](../../kernel/core/workflow/wf_bundle.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c).

Planned acceptance: Reject a behavioral package with unsatisfied dependencies or a wider permission request.

Status: Queued. Source message: `f78a94e9-de8d-40bb-ab51-35b163f06800`.

## Day 028 — 2026-07-23

**AI-First OS Update: Agent Trajectories Become Security and Scheduling Objects**

Learning: Authorize trajectories using accumulated lineage and effects, rather than isolated calls alone.

Anunix comparison: Provenance records relationships and effects have phases. Cross-call trajectory policy needs explicit state.

Evidence: [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Deny a prohibited sequence even when its individual operations are separately available.

Status: Queued. Source message: `42b27027-d6d6-4591-9e36-ad4a065f6657`.

## Day 029 — 2026-07-24

**AI-First OS Update: Causal Control and DAG-Aware Caching**

Learning: Preserve workflow intermediates according to causal importance and reconstruction cost.

Anunix comparison: Workflow graphs and object provenance exist. Memory placement does not by itself establish graph-aware eviction.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c).

Planned acceptance: Retain an intermediate needed by runnable descendants and reclaim one with no live consumer.

Status: Queued. Source message: `1786af0d-1948-4d58-aada-fa836093fddc`.

## Day 030 — 2026-07-25

**AI-First OS Update: Target Knowledge Becomes an Optimization Artifact**

Learning: Make target knowledge and task state explicit, versioned inputs to optimization.

Anunix comparison: ICM annotations and provenance can describe artifacts. The Twin models only a bounded part of execution.

Evidence: [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c).

Planned acceptance: Reject an optimization artifact whose target description no longer matches the tested environment.

Status: Queued. Source message: `de9ffcd8-6113-4a35-8372-3cba1927e61e`.

## Day 031 — 2026-07-26

**AI-First OS Update: The Harness Becomes the Optimization Kernel**

Learning: Trust the evaluation harness to establish correctness and measurement, independently of the generating agent.

Anunix comparison: Anunix has conformance and promotion gates. Candidate-controlled evidence must not become self-authorization.

Evidence: [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Reject missing or mismatched evidence and accept only an independently checked candidate.

Status: Queued. Source message: `2b5bed64-2acb-4d7b-a1a7-0ef932383afd`.

## Day 032 — 2026-07-27

**AI-First OS Update: Execution Scope and Loop Governance Become OS Primitives**

Learning: Grant bounded execution leases and enlarge them only through an explicit decision.

Anunix comparison: Cells expose recursion limits and budgets. Constraints need enforcement on every expansion path.

Evidence: [kernel/core/route/budget.c](../../kernel/core/route/budget.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Reject excessive recursion or child creation without allocating an extra child.

Status: Queued. Source message: `52886424-fea8-48fb-adb3-5793d7b23eb6`.

## Day 033 — 2026-07-28

**AI-First OS Update: Typed Kernel Policy and Explicit Revision Authority**

Learning: Compile policy and its boundary contracts together; distinguish parameter changes from authority changes.

Anunix comparison: Typed routing policies and capability lifecycle states exist. Revision classes need explicit enforcement.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/core/credential.c](../../kernel/core/credential.c).

Planned acceptance: Permit a bounded parameter change while rejecting a change to the policy's authority ceiling.

Status: Queued. Source message: `9497f51e-3371-473c-a157-70c6a0b61e6e`.

## Day 034 — 2026-07-29

**AI-First OS Update: Separate Speculative Execution from Verified Authority**

Learning: Speculate about resource preparation while withholding authority for durable effects.

Anunix comparison: Staged object writes and VM Objects exist. Speculative preparation must not grant effect authority.

Evidence: [kernel/core/state/stage.c](../../kernel/core/state/stage.c), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/vm/vm_object.c](../../kernel/core/vm/vm_object.c).

Planned acceptance: Abort speculative state without changing the committed object or dispatching an external effect.

Status: Queued. Source message: `0b7c44cc-1651-400d-828e-5eab296603de`.

## Day 035 — 2026-07-30

**AI-First OS Update: Semantic Data Movement and Proof-Carrying Scheduling**

Learning: Describe data movement semantically and validate the implementation's scheduling invariants.

Anunix comparison: State transfer and scheduler queues exist. Cross-device movement graphs need explicit lowering and checks.

Evidence: [kernel/core/state/xfer.c](../../kernel/core/state/xfer.c), [kernel/core/sched/scheduler.c](../../kernel/core/sched/scheduler.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c).

Planned acceptance: Preserve transfer permissions and data identity while rejecting an invalid destination.

Status: Queued. Source message: `6ee3d16a-44f6-4657-ae00-480794c4c9a6`.

## Day 036 — 2026-07-31

**AI-First OS Update: Federated Scheduling and Admission Contracts**

Learning: Delegate bounded scheduling authority to domains and distinguish reservation from readiness.

Anunix comparison: Engine leases and readiness classes exist. Nested scheduler ownership is not implied by flat queues.

Evidence: [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/sched/scheduler.c](../../kernel/core/sched/scheduler.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c).

Planned acceptance: Reject a child reservation beyond its parent grant and reclaim capacity after revocation.

Status: Queued. Source message: `7037ad65-bc52-48dc-a3c7-faa6f20e6f40`.

## Day 037 — 2026-08-01

**AI-First OS Update: The Agent Graph Becomes a Schedulable Resource**

Learning: Treat the agent graph as versioned execution state with bounded structural changes.

Anunix comparison: Workflow graphs and cell dependencies exist. Runtime graph changes need their own validity gate.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c).

Planned acceptance: Reject a cycle or invalid graph revision while preserving the runnable original graph.

Status: Queued. Source message: `8ee2a2ac-3033-40ce-9edf-df54b335bf33`.

## Day 038 — 2026-08-02

**AI-First OS Update: The Agent Runtime Needs a JIT**

Learning: Compile repeated trajectories into deterministic or hybrid workflows and demote them when conditions change.

Anunix comparison: Workflow packages and capability promotion support reusable execution. Automatic trajectory compilation is a separate feature.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/workflow/wf_bundle.c](../../kernel/core/workflow/wf_bundle.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Run a validated artifact and reject reuse after its declared preconditions become false.

Status: Queued. Source message: `d31e2d3b-65f3-4bf9-8af5-ce71dc356965`.

## Day 039 — 2026-08-03

**AI-First OS Update: Continuation State Becomes the New Process Image**

Learning: Represent a continuation across semantic history, tokens, model state, tools, and device placement.

Anunix comparison: Workflow continuations preserve workflow progress. They do not alone preserve every model or tool representation.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/include/anx/memory.h](../../kernel/include/anx/memory.h).

Planned acceptance: Resume a workflow from saved progress without repeating a completed effect.

Status: Queued. Source message: `583e07ee-0781-4425-81ed-0a3a942cebd5`.

## Day 040 — 2026-08-04

**AI-First OS Update: Compile Intent into Control Policy**

Learning: Compile natural-language intent into typed objectives and constraints before searching policy candidates.

Anunix comparison: The Twin accepts routing weights and budgets expose constraints. A natural-language policy compiler is outside these primitives.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/budget.c](../../kernel/core/route/budget.c), [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c).

Planned acceptance: Reject an infeasible typed objective before candidate execution or promotion.

Status: Queued. Source message: `e7cc63e2-9a8d-424f-9050-637fa34a1080`.

## Day 041 — 2026-08-05

**AI-First OS Update: Observation Coherence and Semantic I/O Scheduling**

Learning: Schedule observations by validity, provenance, cost, and supersession.

Anunix comparison: Objects and interface events carry structure. Catalog metadata alone does not establish observation coherence.

Evidence: [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/include/anx/interface_plane.h](../../kernel/include/anx/interface_plane.h).

Planned acceptance: Reject an observation superseded by a newer state transition.

Status: Queued. Source message: `5eb9115d-a4bb-4478-b033-ef28b0a7dc95`.

## Day 042 — 2026-08-06

**AI-First OS Update: Schedule Agent Roles and Phase Transitions**

Learning: Schedule agent roles and workflow phases instead of treating an entire campaign as one request.

Anunix comparison: Workflows have node kinds and continuations. Role changes need resource and authority accounting.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Release a finished phase's lease before starting a phase with different resource needs.

Status: Queued. Source message: `401f482f-a77c-4c64-949e-0fe1fd3af2b7`.

## Day 043 — 2026-08-07

**AI-First OS Update: Semantic Transactions + Workload Compilation**

Learning: Preserve a workflow's semantic environment across suspension and compile repeated work where valid.

Anunix comparison: Object staging and workflow suspension exist. Prompts, models, tools, and policies need a consistent version boundary.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/state/stage.c](../../kernel/core/state/stage.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Reject resume against changed semantic dependencies and preserve the original continuation.

Status: Queued. Source message: `f6d60d8a-4387-4496-96d4-d12bd2211617`.

## Day 044 — 2026-08-08

**AI-First OS Update: Treat Kernel Tuning as Profile Synthesis, Not Runtime Autonomy**

Learning: Synthesize kernel profiles offline and select validated profiles deterministically at runtime.

Anunix comparison: The Twin and regime detector cover routing policy experiments. They do not tune Linux build configuration.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/regime/regime.c](../../kernel/core/regime/regime.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Reject a profile outside its validated operating envelope and use the incumbent.

Status: Queued. Source message: `ad4ddc59-952c-4cfe-b496-8bb3ea56f9db`.

## Day 045 — 2026-08-09

**AI-First OS Update: Speculation Becomes a First-Class OS Primitive**

Learning: Prepare predicted futures cheaply and commit only after deterministic validation.

Anunix comparison: Object staging and hypothetical routing are separate mechanisms. Their composition must preserve authority.

Evidence: [kernel/core/state/stage.c](../../kernel/core/state/stage.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c).

Planned acceptance: Discard an incorrect prediction without changing committed state or invoking a handler.

Status: Queued. Source message: `e5c38077-c49b-485a-9a11-b8747064f5a4`.

## Day 046 — 2026-08-10

**AI-First OS Update: The AI Scheduler Is Becoming a Policy Compiler**

Learning: Use AI as a policy compiler while a deterministic controller builds, measures, and accepts candidates.

Anunix comparison: Routing simulation and measured promotion already exist. The complete build-and-measure loop needs artifact evidence.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Bind a candidate to its tested image and reject a candidate with absent validation.

Status: Queued. Source message: `e0ca45f5-7a92-4443-8448-b8cd685e712d`.

## Day 047 — 2026-08-11

**AI-First OS Update: Push the Policy Foundry Below the Kernel**

Learning: Apply bounded search to hardware policy while respecting physical capacity.

Anunix comparison: Anunix models engine resources. Hardware prefetch-policy synthesis needs an appropriate simulator or hardware backend.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Reject a policy exceeding its declared storage or resource budget before adoption.

Status: Queued. Source message: `25eede96-54b8-4ce0-9fe8-0ff73d6fab3d`.

## Day 048 — 2026-08-12

**AI-First OS Update: Make the Agent Working Set Semantic**

Learning: Manage the semantic working set by readiness, utility, reconstruction cost, provenance, and dependencies.

Anunix comparison: Readiness, memory tiers, and provenance exist in separate modules.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c).

Planned acceptance: Preserve useful state with live dependencies and reject reuse after its provenance becomes invalid.

Status: Queued. Source message: `a17f5f52-26fe-4618-8c6e-1438b85cd28e`.

## Day 049 — 2026-08-13

**AI-First OS Update: Separate Advice from Ownership**

Learning: Let AI supply advice while deterministic subsystems retain ownership of placement, eviction, and commit.

Anunix comparison: Memory and lease managers own native state. Staged writes separate pending data from publication.

Evidence: [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/state/stage.c](../../kernel/core/state/stage.c).

Planned acceptance: Establish that an advisory hint cannot override a hard memory or authority constraint.

Status: Queued. Source message: `64e753a4-3d01-4979-b642-b5fd448d0c86`.

## Day 050 — 2026-08-14

**AI-First OS Update: Virtualize Semantic Liveness, Not Just Memory**

Learning: Separate logical liveness from physical allocation so dead state does not pin an entire allocation.

Anunix comparison: The inference and memory layers provide starting points. Fine-grained KV liveness is not implied by object retention.

Evidence: [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c).

Planned acceptance: Reclaim dead logical state without invalidating a surviving reference.

Status: Queued. Source message: `880a7069-0965-4314-9d09-048664fc8ba6`.

## Day 051 — 2026-08-15

**AI-First OS Update: Decouple the Scaling Unit from the Agent**

Learning: Use different units for identity, isolation, scaling, scheduling, and failure.

Anunix comparison: Cells, model execution, and engine leases are distinct. Operator-level scaling needs explicit resource boundaries.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Resize a resource allocation without changing the logical task identity.

Status: Queued. Source message: `99f0ff6a-48dd-4690-8156-018b950d3c27`.

## Day 052 — 2026-08-16

**AI-First OS Update: Capacity Should Be Leased Just in Time**

Learning: Separate resource entitlement from current ownership and allocate physical capacity near use.

Anunix comparison: Anunix already exposes leases and budget profiles. Just-in-time activation needs lifecycle integration.

Evidence: [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/route/budget.c](../../kernel/core/route/budget.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c).

Planned acceptance: Release idle capacity while preserving entitlement; revalidate capacity before reacquisition.

Status: Queued. Source message: `aba76f45-4f15-42d7-a43b-d5f1d488eb4a`.

## Day 053 — 2026-08-17

**AI-First OS Update: Determinism Should Be a Schedulable Resource**

Learning: Declare consistency requirements explicitly and stage effects when stronger guarantees need them.

Anunix comparison: Execution contracts and staged object mutations exist. The cell runtime still contains validation and commit stubs.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/state/stage.c](../../kernel/core/state/stage.c), [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c).

Planned acceptance: Exercise staged visibility, conflict handling, and abort; reject unsupported transactional claims.

Status: Queued. Source message: `c32c6f5c-3fd5-4876-84a4-47e7958510a4`.

## Day 054 — 2026-08-18

**AI-First OS Update: Model Updates Need Transaction Semantics**

Learning: Publish model adaptations atomically and key caches by the served model version.

Anunix comparison: Anxml and object staging exist. JEPA training is distinct from transactional publication of serving adapters.

Evidence: [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/core/state/stage.c](../../kernel/core/state/stage.c).

Planned acceptance: Keep an old model version usable until a validated candidate and its cache identity publish together.

Status: Queued. Source message: `5cba8a41-798c-4f31-b00b-12653231ce94`.

## Day 055 — 2026-08-19

**AI-First OS Update: The Runtime Boundary Is Becoming an Authority Kernel**

Learning: Check operation authority, information flow, resource admission, and outcome evidence independently.

Anunix comparison: The protected-operation protocol and sensitivity checks exist. Actual boundary dispatch must use them.

Evidence: [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/state/access.c](../../kernel/core/state/access.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Deny disallowed egress despite call authority; retain an unknown outcome without blind retry.

Status: Queued. Source message: `86cfdecb-bae0-4623-9998-6f19c1f1247a`.

## Day 056 — 2026-08-20

**AI-First OS Update: Build a Resource Digital Twin Before a Learned Scheduler**

Learning: Use a bounded resource model to compare near-term scheduling choices before changing the live system.

Anunix comparison: The Twin snapshots engines and queue depths. It simulates weighted routing, not complete execution or restoration.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Compare frozen simulation with live routing and reject infeasible restoration demand.

Status: Queued. Source message: `ddc17c69-af7e-4297-921f-6146b196222e`.

## Day 057 — 2026-08-21

**AI-First OS Update: AI Should Be the Regime-Change Controller, Not the Default Scheduler**

Learning: Invoke adaptation when deterministic telemetry detects a departure from the validated regime.

Anunix comparison: The regime detector uses an envelope and the promotion gate checks candidate margins.

Evidence: [kernel/core/regime/regime.c](../../kernel/core/regime/regime.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c).

Planned acceptance: Exercise stationary telemetry, a regime exit, rejected adaptation, and return to the baseline.

Status: Queued. Source message: `e83e8807-e735-4161-b007-45ecbc8e3ed1`.

## Day 058 — 2026-08-22

**AI-First OS Update: Schedule the Cognitive Plan Before the Hardware**

Learning: Budget model choice, reasoning, skills, and context before physical placement.

Anunix comparison: Cognitive-envelope fields exist. Their header explicitly identifies missing integration with specialized anxml workflow dispatch.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/core/route/budget.c](../../kernel/core/route/budget.c).

Planned acceptance: Enforce a cognitive budget in the real inference dispatch path.

Status: Queued. Source message: `98626b40-d8a8-4dc7-939d-9461ca4b487c`.

## Day 059 — 2026-08-23

**AI-First OS Update: Late Binding May Be the Core Model-Native OS Abstraction**

Learning: Keep logical identity stable while binding physical representation and execution late.

Anunix comparison: Cells, engine routing, and model representations are separate. Late binding needs version and readiness checks.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Rebind a task to an eligible engine without changing its identity or violating dependencies.

Status: Queued. Source message: `0808d44d-d49e-4e35-a05c-d1ddb54743e5`.

## Day 060 — 2026-08-24

**AI-First OS Update: Build a Model-State Fabric, Not Just a Model Scheduler**

Learning: Treat model state as a movable resource with identity, validity, and transfer costs.

Anunix comparison: Tensor objects and state transfer exist. A general model-state transport fabric remains a separate integration.

Evidence: [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/core/state/xfer.c](../../kernel/core/state/xfer.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c).

Planned acceptance: Preserve model-state identity and access checks across a transfer.

Status: Queued. Source message: `a5ddd2ca-0545-4045-823d-931bd901212f`.

## Day 061 — 2026-08-25

**AI-First OS Update: Linux 7.3 Makes Hierarchical Scheduling a Real Kernel Primitive**

Learning: Delegate revocable CPU ownership through a scheduler hierarchy.

Anunix comparison: Anunix has flat priority queues and engine leases. Linux sub-scheduler support is a reference, not an available Anunix API.

Evidence: [kernel/core/sched/scheduler.c](../../kernel/core/sched/scheduler.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Revoke a child grant and establish that no task retains capacity outside its authority.

Status: Queued. Source message: `e2f849a1-d8d9-4741-9440-50fa333e1238`.

## Day 062 — 2026-08-26

**AI-First OS Update: Reasoning Itself Is Becoming a Schedulable Parallel Program**

Learning: Distinguish required parallel subtasks from competing trials in the execution graph.

Anunix comparison: Workflow graphs and cell dependencies exist. Trial completion and loser cancellation need explicit semantics.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Complete an OR group with one valid result and fence effects from losing branches.

Status: Queued. Source message: `274013f7-dc77-40cf-93ee-b33bd4c2fa08`.

## Day 063 — 2026-08-27

**AI-First OS Update: The Harness Is Becoming a JIT-Compiled Control Image**

Learning: Treat a generated harness as a bounded control program validated against a stable interface.

Anunix comparison: Workflow bundles provide an executable structure. Capability validation must remain outside the generated program.

Evidence: [kernel/core/workflow/wf_bundle.c](../../kernel/core/workflow/wf_bundle.c), [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Reject a generated control artifact with an invalid transition or undeclared operation.

Status: Queued. Source message: `ae5c3fd3-12f2-4b93-9152-5c30860468dd`.

## Day 064 — 2026-08-28

**AI-First OS Update: Trust Has to Survive from Intent to Physical Use**

Learning: Bind intent, delegation, effects, and the bytes used at execution time into one causal chain.

Anunix comparison: Effects reference cells and objects; provenance records lineage. Validation can become stale before physical use.

Evidence: [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Reject execution after an authorized object's version changes between preparation and dispatch.

Status: Queued. Source message: `76e08520-047b-4b16-ac49-9d6162e4dd1e`.

## Day 065 — 2026-08-29

**AI-First OS Update: Optimize Execution Shape Before Scheduling It**

Learning: Optimize execution shape before placement and measure the complete workflow.

Anunix comparison: Workflows express logical steps and the Twin models placement. Physical fusion is not part of that snapshot.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Compare equivalent execution plans while preserving dependencies and authority boundaries.

Status: Queued. Source message: `c4abc0ce-78d5-4add-ac3f-58e82e776015`.

## Day 066 — 2026-08-30

**AI-First OS Update: Compile Experience into Versioned Procedural Artifacts**

Learning: Compile experience into versioned procedural artifacts with traceable evidence.

Anunix comparison: ICM, workflow bundles, and provenance provide artifact storage. Automatic experience compilation needs independent validation.

Evidence: [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c), [kernel/core/workflow/wf_bundle.c](../../kernel/core/workflow/wf_bundle.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c).

Planned acceptance: Reject a procedural artifact with missing source evidence or an invalid dependency version.

Status: Queued. Source message: `dcdb3512-f8d3-4508-ba3b-f80e5a105b0f`.

## Day 067 — 2026-08-31

**AI-First OS Update: The Agent Runtime Should Be a Process Graph, Not a Monolithic Harness**

Learning: Keep durable continuation identity while composing replaceable execution processes.

Anunix comparison: Workflow continuations and cell identities exist. A runtime-independent persistent agent is a broader lifecycle object.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/credential.c](../../kernel/core/credential.c).

Planned acceptance: Replace an execution binding while preserving the continuation's authority and completed effects.

Status: Queued. Source message: `26a8ff69-2235-4a97-9c44-c1ba8d2f559f`.

## Day 068 — 2026-09-01

**AI-First OS Update: Suspension Needs to Become a First-Class OS State**

Learning: Distinguish long suspension from short I/O waits and reclaim expensive idle state.

Anunix comparison: Workflow suspension exists. Engine leases and model residency need a coordinated suspension policy.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Release reclaimable resources during suspension and revalidate authority before resume.

Status: Queued. Source message: `77557664-206e-4b62-b50b-b5960f80d9ce`.

## Day 069 — 2026-09-02

**AI-First OS Update: Resume from a Frontier, Not from a Fully Restored Image**

Learning: Resume from the minimum usable state frontier while restoring later dependencies incrementally.

Anunix comparison: Readiness classes and workflow dependencies exist. The Twin does not model incremental state materialization.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c), [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c).

Planned acceptance: Run only a ready frontier and reject execution whose immediate dependencies remain unavailable.

Status: Queued. Source message: `1afdd52b-e9ef-42cc-b612-97e2606d48d0`.

## Day 070 — 2026-09-03

**AI-First OS Update: Schedule Externalities, Not Just Resource Consumption**

Learning: Account for interference imposed on other workloads and exposure from external effects.

Anunix comparison: Routing costs and effect phases exist. They do not measure general interference or irreversible exposure.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/budget.c](../../kernel/core/route/budget.c), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c).

Planned acceptance: Reject a placement or effect that exceeds an explicit externality budget.

Status: Queued. Source message: `d1a26ace-fae8-4748-ba29-81c5edc427bd`.

## Day 071 — 2026-09-04

**AI-First OS Update: Separate the Logical Program from the Physical Machine Plan**

Learning: Preserve an authoritative logical graph while deriving a replaceable physical execution plan.

Anunix comparison: Workflow nodes and cell plans are distinct. Dynamic physical recompilation needs semantic equivalence checks.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Change placement while preserving logical dependencies, outputs, and effect order.

Status: Queued. Source message: `86fbcab7-8cf4-40a0-a928-8253e648b2b8`.

## Day 072 — 2026-09-05

**AI-First OS Update: Schedule Evidence and Intent, Not Just Compute**

Learning: Allocate independent evidence paths and durable future intentions alongside physical resources.

Anunix comparison: Provenance and workflows provide records. Evidence independence and durable intention triggers need explicit policies.

Evidence: [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c).

Planned acceptance: Reject duplicated evidence presented as independent and retain a pending intention through suspension.

Status: Queued. Source message: `1ddbc849-f8fc-464b-a905-d204e865fa77`.

## Day 073 — 2026-09-06

**AI-First OS Update: Make the Adaptive Kernel Proof-Carrying**

Learning: Attach independently checked evidence to adaptive policy and enforce context privilege monotonicity.

Anunix comparison: Promotion gates and object metadata exist. They do not constitute formal kernel proofs or a context privilege boundary.

Evidence: [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c), [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c), [kernel/core/state/access.c](../../kernel/core/state/access.c).

Planned acceptance: Reject missing policy evidence and unauthorized context role or scope promotion.

Status: Queued. Source message: `3693656d-55cf-4075-85be-4d4e1312fb62`.
