# Daily AI-first OS research review

The review covers 80 briefings from June 26 through September 13, 2026.
The user confirmed one briefing per day in this sequence.
The user extended the scope to include later briefings before final release preparation.
Days 074 through 080 have an initial comparison against `4c0bc25`; each day receives an updated comparison before implementation.

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

Status: Merged at `a7da2a2` after Jekyll validation. Tested commit: `387ddc4`. Source message: `c20ed7d5-7da3-4ba6-ba26-549efe59e0c5`.

Detailed comparison and results: [Day 019](day-019.md).

## Day 020 — 2026-07-15

**AI-First OS Update: Semantic Memory Metadata Is Becoming a Scheduling ABI**

Learning: Export semantic retention hints while the memory subsystem owns physical placement.

Anunix comparison: The baseline has tier records and decay without bounded hints or protected eviction. Day 20 ranks a bounded candidate pool while preserving controller-protected tiers.

Evidence: [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/include/anx/memory.h](../../kernel/include/anx/memory.h).

Planned acceptance: Exercise expiring retention advice and controlled tier eviction; preserve protected object content and reject unauthorized protection changes.

Status: Merged at `b3ed9c7` after Jekyll validation. Tested commit: `9d1c753`. Source message: `d2928218-8709-4a8b-8251-f76e37b0577c`.

Detailed comparison and results: [Day 020](day-020.md).

## Day 021 — 2026-07-16

**AI-First OS Update: Semantic Control Loops Are Moving from Theory to Working Systems**

Learning: Use typed actions in slower control loops; retain deterministic resource ownership.

Anunix comparison: The baseline validates simulated weights but hardcodes live scoring. Day 21 activates typed, serialized policy trials and restores the prior weights after rejection.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/regime/regime.c](../../kernel/core/regime/regime.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Reject malformed or stale tuning actions; observe live placement changes and restore the previous policy after a failed trial.

Status: Merged at `4d6e43c` after Jekyll validation. Tested commit: `e07f352`. Source message: `4d0e3645-bcdc-41c0-bb55-a3e4cd28e04e`.

Detailed comparison and results: [Day 021](day-021.md).

## Day 022 — 2026-07-17

**AI-First OS Update: Identity Becomes a Signed Lifecycle Boundary**

Learning: Keep persistent agent identity separate from model, process, and host identity.

Anunix comparison: Baseline 4d6e43c has cell permissions and capability grants but no signed identity commitment. Day 022 adds signed authority history and admission across cell replacement.

Evidence: [kernel/core/credential.c](../../kernel/core/credential.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Require a valid operator signature for authority changes; preserve revocation across replacement cells and identify the admission commitment in traces.

Status: Merged at `f0e4f56` after Jekyll validation. Tested commit: `7f776bc`. Source message: `e3b5929c-297d-4584-a409-2ba221be3fa4`.

Detailed comparison and results: [Day 022](day-022.md).

## Day 023 — 2026-07-18

**AI-First OS Update: The GUI Becomes a Semantic I/O Subsystem**

Learning: Expose graphical controls as typed objects with state and verifiable actions.

Anunix comparison: Baseline f0e4f56 has accessibility nodes but no observation generation or caller check. Day 023 adds authorized focus with stale-target rejection and a verified focus receipt.

Evidence: [kernel/include/anx/interface_plane.h](../../kernel/include/anx/interface_plane.h), [kernel/core/state/access.c](../../kernel/core/state/access.c).

Planned acceptance: Reject stale or unauthorized control actions and verify that an allowed focus action reaches the intended surface.

Status: Merged at `2b8df85` after Jekyll validation. Tested commit: `2b38e9f`. Source message: `18e4cfa8-3933-472b-900e-800cd425e87a`.

Detailed comparison and results: [Day 023](day-023.md).

## Day 024 — 2026-07-19

**AI-First OS Update: “Pause” Must Be an Effect Fence**

Learning: A pause must fence effects across the relevant execution scope.

Anunix comparison: Baseline 2b8df85 checks individual cell authority but lacks a shared run fence. Day 024 adds inherited holds, sticky terminal epochs, and governed dispatch checks.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c).

Planned acceptance: Hold one branch and block sibling effects in the same run while harmless computation and unrelated runs can continue.

Status: Merged at `5111209` after Jekyll validation. Tested commit: `e5ecdf3`. Source message: `38396083-009d-4719-87bb-27720ab59c9d`.

Detailed comparison and results: [Day 024](day-024.md).

## Day 025 — 2026-07-20

**AI-First OS Update: Tool Catalogs Become a Virtual I/O Namespace**

Learning: Expose a bounded tool namespace through capability handles instead of loading every schema into context.

Anunix comparison: Baseline 5111209 has capability records and a transport registry but no scoped catalog or revocable tool handle. Day 025 adds bounded discovery and current-grant invocation checks.

Evidence: [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c), [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c).

Planned acceptance: Bound discovery to authorized entries and reject invocation through revoked, stale, foreign, or altered tool handles.

Status: Merged at `654d3e8` after Jekyll validation. Tested commit: `a0bca2e`. Source message: `b3e419a3-edae-45ea-9b87-b3975f94202c`.

Detailed comparison and results: [Day 025](day-025.md).

## Day 026 — 2026-07-21

**AI-First OS Update: Schedule Continuity, Not Isolated Requests**

Learning: Schedule continuation state using dependencies, residency, and expected reuse.

Anunix comparison: Baseline 654d3e8 retains eligible session affinity indefinitely. Day 026 adds expiring, cost-aware reuse of validated logical state while preserving current engine permission checks.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Prefer readable, validated resident state only while the hint remains useful and unexpired, without selecting an ineligible engine.

Status: Merged at `3c650e5` after Jekyll validation. Tested commit: `a7b91d7`. Source message: `8f38db3a-f37c-44db-be06-1612a846def1`.

Detailed comparison and results: [Day 026](day-026.md).

## Day 027 — 2026-07-22

**AI-First OS Update: Skills Become Schedulable Behavioral Packages**

Learning: Treat skills as versioned behavioral packages with dependencies, permissions, and evaluations.

Anunix comparison: The prior validator accepted a missing required engine at score 75. Validation and installation now require bounded, unique dependencies with current readiness; the existing authority ceiling also applies.

Evidence: [kernel/core/workflow/wf_bundle.c](../../kernel/core/workflow/wf_bundle.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c).

Planned acceptance: Reject a behavioral package with unsatisfied dependencies or a wider permission request.

Status: Merged at `ca41ae4` after Jekyll validation. Tested commit: `8014d85`. Source message: `f78a94e9-de8d-40bb-ab51-35b163f06800`.

Detailed comparison and results: [Day 027](day-027.md).

## Day 028 — 2026-07-23

**AI-First OS Update: Agent Trajectories Become Security and Scheduling Objects**

Learning: Authorize trajectories using accumulated lineage and effects, rather than isolated calls alone.

Anunix comparison: The baseline checked declared object labels but forgot sensitive reads across calls. Bound run fences now retain the highest read sensitivity and constrain later protected effects across branches.

Evidence: [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Deny a prohibited sequence even when its individual operations are separately available.

Status: Merged at `ff868c8` after Jekyll validation. Tested commit: `99dfbfa`. Source message: `42b27027-d6d6-4591-9e36-ad4a065f6657`.

Detailed comparison and results: [Day 028](day-028.md).

## Day 029 — 2026-07-24

**AI-First OS Update: Causal Control and DAG-Aware Caching**

Learning: Preserve workflow intermediates according to causal importance and reconstruction cost.

Anunix comparison: The baseline could evict produced inputs needed by unfinished workflow nodes. Eviction, demotion, and forgetting now preserve those inputs across active and paused workflows until their consumers finish.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c).

Planned acceptance: Retain an intermediate needed by runnable descendants and reclaim one with no live consumer.

Status: Merged at `1936ce2` after Jekyll validation. Tested commit: `ee12ad9`. Source message: `1786af0d-1948-4d58-aada-fa836093fddc`.

Detailed comparison and results: [Day 029](day-029.md).

## Day 030 — 2026-07-25

**AI-First OS Update: Target Knowledge Becomes an Optimization Artifact**

Learning: Make target knowledge and task state explicit, versioned inputs to optimization.

Anunix comparison: The baseline bound tuning actions to policy generations only. Optional target contracts and sealed proposal artifacts now recheck the compiled profile, selected engine settings, and versioned knowledge and evaluation references.

Evidence: [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c).

Planned acceptance: Reject an optimization artifact whose target description no longer matches the tested environment.

Status: Merged at `7af3018` after Jekyll validation. Tested commit: `bb5dc0b`. Source message: `de9ffcd8-6113-4a35-8372-3cba1927e61e`.

Detailed comparison and results: [Day 030](day-030.md).

## Day 031 — 2026-07-26

**AI-First OS Update: The Harness Becomes the Optimization Kernel**

Learning: Trust the evaluation harness to establish correctness and measurement, independently of the generating agent.

Anunix comparison: The baseline accepted caller-authored evaluation payloads after checking target compatibility. Artifact trials now require an issued passing receipt from fixed routing checks, bound to the complete candidate action.

Evidence: [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Reject missing or mismatched evidence and accept only an independently checked candidate.

Status: Merged at `4c0bc25` after Jekyll validation. Tested commit: `e885c86`. Source message: `2b5bed64-2acb-4d7b-a1a7-0ef932383afd`.

Detailed comparison and results: [Day 031](day-031.md).

## Day 032 — 2026-07-27

**AI-First OS Update: Execution Scope and Loop Governance Become OS Primitives**

Learning: Grant bounded execution leases and enlarge them only through an explicit decision.

Anunix comparison: The baseline bounds concurrent children but returns slots after destruction. Day 32 adds a shared lifetime creation budget, explicit expansion, and permanent lease expiry.

Evidence: [kernel/core/route/budget.c](../../kernel/core/route/budget.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Reject a child beyond the shared lifetime budget without allocation; permit explicit expansion and fence new children and effects after expiry.

Status: Merged at `e534009` after Jekyll validation. Tested commit: `003f72c`. Source message: `52886424-fea8-48fb-adb3-5793d7b23eb6`.

Detailed comparison and results: [Day 032](day-032.md).

## Day 033 — 2026-07-28

**AI-First OS Update: Typed Kernel Policy and Explicit Revision Authority**

Learning: Compile policy and its boundary contracts together; distinguish parameter changes from authority changes.

Anunix comparison: The baseline has no revision grant between active-cell denial of tuning and permission to install capabilities. Day 33 adds parameter and implementation ceilings, inherited leases, and trial ownership.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c), [kernel/core/credential.c](../../kernel/core/credential.c).

Planned acceptance: Permit bounded parameter trials under an inherited grant while rejecting implementation changes, authority expansion, foreign trial acceptance, and revoked grants.

Status: Merged at `673125e` after Jekyll validation. Tested commit: `18d0b5a`. Source message: `9497f51e-3371-473c-a157-70c6a0b61e6e`.

Detailed comparison and results: [Day 033](day-033.md).

## Day 034 — 2026-07-29

**AI-First OS Update: Separate Speculative Execution from Verified Authority**

Learning: Speculate about resource preparation while withholding authority for durable effects.

Anunix comparison: The baseline stages writes but permits publication without current effect authority or actor checks. Day 34 gates publication and shadow writes while preserving explicit abort.

Evidence: [kernel/core/state/stage.c](../../kernel/core/state/stage.c), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/vm/vm_object.c](../../kernel/core/vm/vm_object.c).

Planned acceptance: Reject staged publication without effect permission or during a hold; abort preserves committed state and prevents external dispatch.

Status: Merged at `6b5a9e2` after Jekyll validation. Tested commit: `2324a14`. Source message: `0b7c44cc-1651-400d-828e-5eab296603de`.

Detailed comparison and results: [Day 034](day-034.md).

## Day 035 — 2026-07-30

**AI-First OS Update: Semantic Data Movement and Proof-Carrying Scheduling**

Learning: Describe data movement semantically and validate the implementation's scheduling invariants.

Anunix comparison: The baseline matches raw prefixes and skips policy checks at write and commit. Day 35 validates namespace boundaries, rejects truncation, and rechecks destination policy while preserving digest continuity.

Evidence: [kernel/core/state/xfer.c](../../kernel/core/state/xfer.c), [kernel/core/sched/scheduler.c](../../kernel/core/sched/scheduler.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c).

Planned acceptance: Reject invalid or revoked destinations before more bytes or a committed result; preserve the exact digest across interruption and resume.

Status: Merged at `72ab01b` after Jekyll validation. Tested commit: `874665b`. Source message: `6ee3d16a-44f6-4657-ae00-480794c4c9a6`.

Detailed comparison and results: [Day 035](day-035.md).

## Day 036 — 2026-07-31

**AI-First OS Update: Federated Scheduling and Admission Contracts**

Learning: Delegate bounded scheduling authority to domains and distinguish reservation from readiness.

Anunix comparison: The baseline accounts for flat global leases. Day 36 adds bounded parent grants, shared root accounting, and subtree revocation with explicit record cleanup.

Evidence: [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/sched/scheduler.c](../../kernel/core/sched/scheduler.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c).

Planned acceptance: Reject a child beyond its parent grant; preserve global accounting across nesting and restore ledger capacity after quiescent subtree revocation.

Status: Merged at `01512bc` after Jekyll validation. Tested commit: `1dafa66`. Source message: `7037ad65-bc52-48dc-a3c7-faa6f20e6f40`.

Detailed comparison and results: [Day 036](day-036.md).

## Day 037 — 2026-08-01

**AI-First OS Update: The Agent Graph Becomes a Schedulable Resource**

Learning: Treat the agent graph as versioned execution state with bounded structural changes.

Anunix comparison: The baseline executes workflow graphs but lacks controlled topology revisions. The candidate validates complete edge replacements against frozen nodes, requires sealed evidence from the current revision, and adds bounded rollback.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c).

Planned acceptance: Reject invalid graph revisions without changing the runnable original; execute the revised and restored orders; deny stale, over-budget, and paused changes.

Status: Merged at `533e417` after Jekyll validation. Tested commit: `adf1950`. Source message: `8ee2a2ac-3033-40ce-9edf-df54b335bf33`.

Detailed comparison and results: [Day 037](day-037.md).

## Day 038 — 2026-08-02

**AI-First OS Update: The Agent Runtime Needs a JIT**

Learning: Compile repeated trajectories into deterministic or hybrid workflows and demote them when conditions change.

Anunix comparison: The baseline validates templates but does not pin reuse assumptions. The candidate binds hybrid or deterministic forms to graph and object digests, reevaluates read access, and demotes stale or failed forms before reuse.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/workflow/wf_bundle.c](../../kernel/core/workflow/wf_bundle.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Run a deterministic state-to-output artifact, then reject changed or deleted preconditions before dispatch; check graph drift, read revocation, and sticky failure demotion.

Status: Merged at `c1a6f02` after Jekyll validation. Tested commit: `85945f0`. Source message: `d31e2d3b-65f3-4bf9-8af5-ce71dc356965`.

Detailed comparison and results: [Day 038](day-038.md).

## Day 039 — 2026-08-03

**AI-First OS Update: Continuation State Becomes the New Process Image**

Learning: Represent a continuation across semantic history, tokens, model state, tools, and device placement.

Anunix comparison: The baseline keeps progress in memory but only serializes graph text. The candidate seals and unloads a bounded continuation image, checks its graph and dependencies on restore, and prevents replay of a consumed checkpoint.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/include/anx/memory.h](../../kernel/include/anx/memory.h).

Planned acceptance: Restore unloaded workflow progress and retain the completed result object without repeating its creation; reject drift, revoked access, and checkpoint replay.

Status: Merged at `ff29ee6` after Jekyll validation. Tested commit: `b98bacc`. Source message: `583e07ee-0781-4425-81ed-0a3a942cebd5`.

Detailed comparison and results: [Day 039](day-039.md).

## Day 040 — 2026-08-04

**AI-First OS Update: Compile Intent into Control Policy**

Learning: Compile natural-language intent into typed objectives and constraints before searching policy candidates.

Anunix comparison: The baseline validates weights and evidence but has no typed policy task. The candidate checks declared domains and units, evaluates fixed observable constraints, and binds the complete task to required passing receipts.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/budget.c](../../kernel/core/route/budget.c), [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c).

Planned acceptance: Reject infeasible typed bounds before evaluation or activation; deny failing or mismatched evidence; admit and roll back a passing compiled task.

Status: Merged at `4f69932` after Jekyll validation. Tested commit: `338af2e`. Source message: `e7cc63e2-9a8d-424f-9050-637fa34a1080`.

Detailed comparison and results: [Day 040](day-040.md).

## Day 041 — 2026-08-05

**AI-First OS Update: Observation Coherence and Semantic I/O Scheduling**

Learning: Schedule observations by validity, provenance, cost, and supersession.

Anunix comparison: Object metadata and interface structure now feed a bounded observation catalog. Reads reject changed, missing, inaccessible, and superseded dependencies; historical bytes remain accessible under normal policy. An interrupt regression also fixes unsafe x86 compiler stack use.

Evidence: [kernel/core/icm/icm.c](../../kernel/core/icm/icm.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/include/anx/interface_plane.h](../../kernel/include/anx/interface_plane.h).

Planned acceptance: Reject an observation superseded by a newer state transition.

Status: Merged at `2d349b5` after Jekyll validation. Tested commit: `5bf6a37`. Source message: `5eb9115d-a4bb-4478-b033-ef28b0a7dc95`.

Detailed comparison and results: [Day 041](day-041.md).

## Day 042 — 2026-08-06

**AI-First OS Update: Schedule Agent Roles and Phase Transitions**

Learning: Schedule agent roles and workflow phases instead of treating an entire campaign as one request.

Anunix comparison: Engine leases now support controller-issued role contracts and one active phase per cell. Busy reservations block transitions; stale or excessive requests cannot replace the current lease. Cell lifetime remains pinned until phase closure and detachment.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Release a finished phase's lease before starting a phase with different resource needs.

Status: Merged at `e367e95` after Jekyll validation. Tested commit: `fb3ca11`. Source message: `401f482f-a77c-4c64-949e-0fe1fd3af2b7`.

Detailed comparison and results: [Day 042](day-042.md).

## Day 043 — 2026-08-07

**AI-First OS Update: Semantic Transactions + Workload Compilation**

Learning: Preserve a workflow's semantic environment across suspension and compile repeated work where valid.

Anunix comparison: A sealed semantic manifest now pins named artifact versions, compatibility declarations, schemas, and the workflow graph. Start, resume, save, and restore check the manifest before changing execution state; mismatches preserve the continuation and permit abort.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/state/stage.c](../../kernel/core/state/stage.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Reject resume against changed semantic dependencies and preserve the original continuation.

Status: Merged at `4447733` after Jekyll validation. Tested commit: `e001c00`. Source message: `f6d60d8a-4387-4496-96d4-d12bd2211617`.

Detailed comparison and results: [Day 043](day-043.md).

## Day 044 — 2026-08-08

**AI-First OS Update: Treat Kernel Tuning as Profile Synthesis, Not Runtime Autonomy**

Learning: Synthesize kernel profiles offline and select validated profiles deterministically at runtime.

Anunix comparison: A trusted compiler now seals finite routing cases and measured simulation results against a captured incumbent and engine environment. The live planner selects the issued profile per decision only while that envelope matches; all rejections retain incumbent weights.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/regime/regime.c](../../kernel/core/regime/regime.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Reject a profile outside its validated operating envelope and use the incumbent.

Status: Merged at `10fcbc7` after Jekyll validation. Tested commit: `3a4537d`. Source message: `ad4ddc59-952c-4cfe-b496-8bb3ea56f9db`.

Detailed comparison and results: [Day 044](day-044.md).

## Day 045 — 2026-08-09

**AI-First OS Update: Speculation Becomes a First-Class OS Primitive**

Learning: Prepare predicted futures cheaply and commit only after deterministic validation.

Anunix comparison: A bounded speculation broker now keeps candidate object replacements private. Publication requires matching action bytes, origin state, deadline, and current staged-write authority. Incorrect, stale, expired, or unauthorized candidates close without publishing their payload or dispatching an external handler.

Evidence: [kernel/core/state/stage.c](../../kernel/core/state/stage.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c).

Planned acceptance: Discard an incorrect prediction without changing committed state or invoking a handler.

Status: Merged at `c67f82f` after Jekyll validation. Tested commit: `afdfacf`. Source message: `e5c38077-c49b-485a-9a11-b8747064f5a4`.

Detailed comparison and results: [Day 045](day-045.md).

## Day 046 — 2026-08-10

**AI-First OS Update: The AI Scheduler Is Becoming a Policy Compiler**

Learning: Use AI as a policy compiler while a deterministic controller builds, measures, and accepts candidates.

Anunix comparison: Compiled source fingerprints now bind kernel and UEFI artifacts, guest regressions, and host records before a candidate receives a validation record.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Reject absent, contradictory, or mismatched image validation; accept the matching source candidate after both VM modes pass the complete regression range.

Status: Merged at `22bb730` after Jekyll validation. Tested commit: `700f28e`. Source message: `e0ca45f5-7a92-4443-8448-b8cd685e712d`.

Detailed comparison and results: [Day 046](day-046.md).

## Day 047 — 2026-08-11

**AI-First OS Update: Push the Policy Foundry Below the Kernel**

Learning: Apply bounded search to hardware policy while respecting physical capacity.

Anunix comparison: The native profile compiler now calculates storage and work requirements and rejects declared budgets before allocation or evaluation.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Reject insufficient artifact, scratch, or simulation budgets without publishing a profile; use an exact-budget candidate after ordinary validation.

Status: Merged at `be47141` after Jekyll validation. Tested commit: `abeec0d`. Source message: `25eede96-54b8-4ce0-9fe8-0ff73d6fab3d`.

Detailed comparison and results: [Day 047](day-047.md).

## Day 048 — 2026-08-12

**AI-First OS Update: Make the Agent Working Set Semantic**

Learning: Manage the semantic working set by readiness, utility, reconstruction cost, provenance, and dependencies.

Anunix comparison: Semantic manifests now protect declared live dependencies and pin controller-issued memory validation generations before reuse or resume.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c).

Planned acceptance: Keep paused dependency chains resident, reject contested or revalidated evidence under the old manifest, and recover through explicit rebinding.

Status: Merged at `cd5fc09` after Jekyll validation. Tested commit: `36b6051`. Source message: `a17f5f52-26fe-4618-8c6e-1438b85cd28e`.

Detailed comparison and results: [Day 048](day-048.md).

## Day 049 — 2026-08-13

**AI-First OS Update: Separate Advice from Ownership**

Learning: Let AI supply advice while deterministic subsystems retain ownership of placement, eviction, and commit.

Anunix comparison: Direct placement is now controller-only, owner cleanup preserves admission accounting, and L4 promotion accepts only provisional or validated state.

Evidence: [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/state/stage.c](../../kernel/core/state/stage.c).

Planned acceptance: Verify maximum hints cannot override placement authority or memory ceilings, while owners can release charges after permission revocation.

Status: Merged at `f228dae` after Jekyll validation. Tested commit: `a0af482`. Source message: `64e753a4-3d01-4979-b642-b5fd448d0c86`.

Detailed comparison and results: [Day 049](day-049.md).

## Day 050 — 2026-08-14

**AI-First OS Update: Virtualize Semantic Liveness, Not Just Memory**

Learning: Separate logical liveness from physical allocation so dead state does not pin an entire allocation.

Anunix comparison: Bounded logical views now preserve record identity and aliases while verified compaction reclaims native physical pages.

Evidence: [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c).

Planned acceptance: Reclaim dead records from fragmented pages while surviving aliases retain identical bytes and identity; preserve mappings after rejected copies.

Status: Merged at `b04649e` after Jekyll validation. Tested commit: `2906052`. Source message: `880a7069-0965-4314-9d09-048664fc8ba6`.

Detailed comparison and results: [Day 050](day-050.md).

## Day 051 — 2026-08-15

**AI-First OS Update: Decouple the Scaling Unit from the Agent**

Learning: Use different units for identity, isolation, scaling, scheduling, and failure.

Anunix comparison: Controllers can resize active phase reservations while retaining task and lease identity. Contract ceilings, shared capacity, recorded usage, and child reservations constrain each change.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Grow and shrink one phase without changing task identity; reject stale requests, overcommitment, and unsafe shrinkage.

Status: Merged at `1e70525` after Jekyll validation. Tested commit: `233518e`. Source message: `99f0ff6a-48dd-4690-8156-018b950d3c27`.

Detailed comparison and results: [Day 051](day-051.md).

## Day 052 — 2026-08-16

**AI-First OS Update: Capacity Should Be Leased Just in Time**

Learning: Separate resource entitlement from current ownership and allocate physical capacity near use.

Anunix comparison: A controller can park an active phase, releasing its reservation while preserving its private entitlement. Reactivation competes for current capacity and retains task identity.

Evidence: [kernel/core/route/lease.c](../../kernel/core/route/lease.c), [kernel/core/route/budget.c](../../kernel/core/route/budget.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c).

Planned acceptance: Release idle reservations, reject reacquisition under competing demand, and restore the saved request after capacity becomes available.

Status: Merged at `07bc005` after Jekyll validation. Tested commit: `482d70f`. Source message: `aba76f45-4f15-42d7-a43b-d5f1d488eb4a`.

Detailed comparison and results: [Day 052](day-052.md).

## Day 053 — 2026-08-17

**AI-First OS Update: Determinism Should Be a Schedulable Resource**

Learning: Declare consistency requirements explicitly and stage effects when stronger guarantees need them.

Anunix comparison: Runtime admission now rejects semantic, transactional, token-stable, and automatically staged contracts that lack enforcement. Explicit single-object staging retains visibility, conflict, and abort checks.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/state/stage.c](../../kernel/core/state/stage.c), [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c).

Planned acceptance: Reject unsupported contracts before handlers run; verify explicit staged visibility, conflict rejection, abort provenance, and successful publication.

Status: Merged at `7a45ecf` after Jekyll validation. Tested commit: `79d2ffb`. Source message: `c32c6f5c-3fd5-4876-84a4-47e7958510a4`.

Detailed comparison and results: [Day 053](day-053.md).

## Day 054 — 2026-08-18

**AI-First OS Update: Model Updates Need Transaction Semantics**

Learning: Publish model adaptations atomically and key caches by the served model version.

Anunix comparison: Anxml can stage and test private toy adapters before publishing their serving generation and cache identity together. Rollback restores the prior image under a fresh generation.

Evidence: [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/core/state/stage.c](../../kernel/core/state/stage.c).

Planned acceptance: Keep the old adapter serving until verified publication; check changed tokens, stale cache rejection, unrelated adapter isolation, and rollback.

Status: Merged at `6e063b6` after Jekyll validation. Tested commit: `aa82de4`. Source message: `5cba8a41-798c-4f31-b00b-12653231ce94`.

Detailed comparison and results: [Day 054](day-054.md).

## Day 055 — 2026-08-19

**AI-First OS Update: The Runtime Boundary Is Becoming an Authority Kernel**

Learning: Check operation authority, information flow, resource admission, and outcome evidence independently.

Anunix comparison: Direct calls now enforce fenced-run sensitivity. Explicit protected operations bind copied requests to destinations and retain ambiguous provider outcomes without redispatching the same identifier.

Evidence: [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/state/access.c](../../kernel/core/state/access.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Deny confidential egress without destination permission; verify approved dispatch, source and provider rechecks, and retained UNKNOWN outcomes that block redispatch.

Status: Merged at `8c237cb` after Jekyll validation. Tested commit: `5293121`. Source message: `86cfdecb-bae0-4623-9998-6f19c1f1247a`.

Detailed comparison and results: [Day 055](day-055.md).

## Day 056 — 2026-08-20

**AI-First OS Update: Build a Resource Digital Twin Before a Learned Scheduler**

Learning: Use a bounded resource model to compare near-term scheduling choices before changing the live system.

Anunix comparison: The Twin now checks declared restoration demand against frozen logical capacity. Live admission still rechecks reservations before use.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Compare frozen and live routing, then prove memory and accelerator contention can invalidate restoration feasibility.

Status: Merged at `5d84358` after Jekyll validation. Tested commit: `5089793`. Source message: `ddc17c69-af7e-4297-921f-6146b196222e`.

Detailed comparison and results: [Day 056](day-056.md).

## Day 057 — 2026-08-21

**AI-First OS Update: AI Should Be the Regime-Change Controller, Not the Default Scheduler**

Learning: Invoke adaptation when deterministic telemetry detects a departure from the validated regime.

Anunix comparison: Bound regions now retain calibrated limits across sustained drift and emit one investigation event per exit. Events grant no policy authority.

Evidence: [kernel/core/regime/regime.c](../../kernel/core/regime/regime.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/cap/promotion.c](../../kernel/core/cap/promotion.c).

Planned acceptance: Exercise stationary telemetry, sustained and repeated departures, stale events, rejected tuning, and return to the calibrated range.

Status: Merged at `e3c41b7` after Jekyll validation. Tested commit: `f1a6563`. Source message: `e83e8807-e735-4161-b007-45ecbc8e3ed1`.

Detailed comparison and results: [Day 057](day-057.md).

## Day 058 — 2026-08-22

**AI-First OS Update: Schedule the Cognitive Plan Before the Hardware**

Learning: Budget model choice, reasoning, skills, and context before physical placement.

Anunix comparison: Native Anxml now applies captured per-generation ceilings, including workflow and adapter calls. Effective ceilings also participate in adapter cache identity.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/core/route/budget.c](../../kernel/core/route/budget.c).

Planned acceptance: Run bounded workflow inference, direct and adapter generation, nested ceilings, prompt access denials, cancellation, and stale cache checks.

Status: Merged at `cd51346` after Jekyll validation. Tested commit: `6926c8f`. Source message: `98626b40-d8a8-4dc7-939d-9461ca4b487c`.

Detailed comparison and results: [Day 058](day-058.md).

## Day 059 — 2026-08-23

**AI-First OS Update: Late Binding May Be the Core Model-Native OS Abstraction**

Learning: Keep logical identity stable while binding physical representation and execution late.

Anunix comparison: Private model bindings now capture logical declarations and object versions, then recheck readiness and policy before changing engines. Backend equivalence remains a controller claim.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Preserve Cell and model identity across local engine changes; reject stale inputs, unavailable servers, revoked leases, and unauthorized placement.

Status: Merged at `5acf978` after Jekyll validation. Tested commit: `38db6bb`. Source message: `0808d44d-d49e-4e35-a05c-d1ddb54743e5`.

Detailed comparison and results: [Day 059](day-059.md).

## Day 060 — 2026-08-24

**AI-First OS Update: Build a Model-State Fabric, Not Just a Model Scheduler**

Learning: Treat model state as a movable resource with identity, validity, and transfer costs.

Anunix comparison: Immutable resource views now move between pools with the same owner. All aliases retain identity, copied bytes are verified before publication, and adapter consumers still reject stale derivation keys.

Evidence: [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c), [kernel/core/state/xfer.c](../../kernel/core/state/xfer.c), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c).

Planned acceptance: Move executable adapter state across physical pools; preserve aliases and owner access, reject failed copies, and invalidate stale model generations.

Status: Merged at `f39c6d4` after Jekyll validation. Tested commit: `0f2531e`. Source message: `a5ddd2ca-0545-4045-823d-931bd901212f`.

Detailed comparison and results: [Day 060](day-060.md).

## Day 061 — 2026-08-25

**AI-First OS Update: Linux 7.3 Makes Hierarchical Scheduling a Real Kernel Primitive**

Learning: Delegate revocable CPU ownership through a scheduler hierarchy.

Anunix comparison: Private scheduler domains now bound bootstrap-CPU eligibility, queue classes, priority, and expiry through a Cell hierarchy. Revocation blocks descendants at dequeue and runtime admission; enforcement remains cooperative.

Evidence: [kernel/core/sched/scheduler.c](../../kernel/core/sched/scheduler.c), [kernel/core/route/lease.c](../../kernel/core/route/lease.c).

Planned acceptance: Revoke a child domain and deny its queued and direct descendant work; run a healthy sibling and reject tool dispatch after live expiry.

Status: Merged at `7c3a1f5` after Jekyll validation. Tested commit: `95dd4e1`. Source message: `e2f849a1-d8d9-4741-9440-50fa333e1238`.

Detailed comparison and results: [Day 061](day-061.md).

## Day 062 — 2026-08-26

**AI-First OS Update: Reasoning Itself Is Becoming a Schedulable Parallel Program**

Learning: Distinguish required parallel subtasks from competing trials in the execution graph.

Anunix comparison: Native branch groups now distinguish required results from competing trials. They execute bounded toy-model candidates as pure child Cells, verify exact outputs, preserve parent fences, and expose only completed-group results.

Evidence: [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Accept a verified trial winner and cancel unstarted alternatives. Require every AND result, enforce the shared budget, and deny branch effects.

Status: Merged at `0bf1fce` after Jekyll validation. Tested commit: `b8b8240`. Source message: `274013f7-dc77-40cf-93ee-b33bd4c2fa08`.

Detailed comparison and results: [Day 062](day-062.md).

## Day 063 — 2026-08-27

**AI-First OS Update: The Harness Is Becoming a JIT-Compiled Control Image**

Learning: Treat a generated harness as a bounded control program validated against a stable interface.

Anunix comparison: Baseline 0bf1fce has workflows and verified model groups. This step adds a bounded control-image ABI, validation, private authority grants, and execution with immutable event history.

Evidence: [kernel/core/workflow/wf_bundle.c](../../kernel/core/workflow/wf_bundle.c), [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c), [kernel/core/cap/capability.c](../../kernel/core/cap/capability.c).

Planned acceptance: Reject malformed images and stale epochs before execution. Recover from one failed model group, return the verified second result, and preserve both outcomes.

Status: Merged at `4ae6479` after Jekyll validation. Tested commit: `178b3e8`. Source message: `ae5c3fd3-12f2-4b93-9152-5c30860468dd`.

Detailed comparison and results: [Day 063](day-063.md).

## Day 064 — 2026-08-28

**AI-First OS Update: Trust Has to Survive from Intent to Physical Use**

Learning: Bind intent, delegation, effects, and the bytes used at execution time into one causal chain.

Anunix comparison: Baseline 4ae6479 validates source dependencies but has no prepared inference receipt. This step binds sources and identity, verifies the CPU execution copy, and records consumed-image evidence.

Evidence: [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/state/provenance.c](../../kernel/core/state/provenance.c), [kernel/core/anxml/anxml.c](../../kernel/core/anxml/anxml.c).

Planned acceptance: Reject changed sources, identity commitments, and corrupted execution copies. Execute fresh records and verify their source, consumed-image, and output digests.

Status: Validated on Jekyll. Tested commit: `fb7fb0e`. Source message: `76e08520-047b-4b16-ac49-9d6162e4dd1e`.

Detailed comparison and results: [Day 064](day-064.md).

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

## Day 074 — 2026-09-07

**AI-First OS Update: Make Resource Shape a Schedulable Object**

Learning: Separate stable logical state from a versioned resource shape and recheck placement after reconfiguration.

Anunix comparison: Engine topology hints and routing target fingerprints exist. Worker reshaping and shared resource-shape epochs need explicit control.

Evidence: [kernel/include/anx/engine.h](../../kernel/include/anx/engine.h), [kernel/core/route/planner.c](../../kernel/core/route/planner.c), [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c).

Planned acceptance: Reject placement against a stale resource shape; preserve logical identity after an authorized shape revision.

Status: Queued. Source message: `a3296827-dc83-4a98-ab4d-a033c2db25f9`.

## Day 075 — 2026-09-08

**AI-First OS Update: Define Semantic ABIs Between Models, Memory, Teams, and Hardware**

Learning: Give planners explicit execution constraints and versioned compatibility contracts for memory, models, coordination, and quality.

Anunix comparison: Cells expose execution constraints, and optimization artifacts bind a target. Memory compatibility across model changes needs a declared contract.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/include/anx/memplane.h](../../kernel/include/anx/memplane.h), [kernel/core/route/optimization.c](../../kernel/core/route/optimization.c).

Planned acceptance: Reject incompatible memory reuse after an executor change; permit reuse under a matching declared contract.

Status: Queued. Source message: `1596f5f2-799b-44ea-9b3e-36b9d0f0c6ee`.

## Day 076 — 2026-09-09

**AI-First OS Update: The Memory Scheduler Is Becoming as Important as the CPU Scheduler**

Learning: Expose logical memory objects and their consumers, then choose physical representations according to topology and measured benefit.

Anunix comparison: Memory tiers and workflow liveness exist. Topology-specific sharing or copying needs a checked representation plan.

Evidence: [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c), [kernel/include/anx/memplane.h](../../kernel/include/anx/memplane.h), [kernel/core/workflow/workflow_exec.c](../../kernel/core/workflow/workflow_exec.c).

Planned acceptance: Reject an inapplicable memory transformation and retain the original object representation; admit a compatible plan.

Status: Queued. Source message: `b6df868e-dc4e-4388-b987-eb9f2591d320`.

## Day 077 — 2026-09-10

**AI-first OS update — September 10: schedule the amount of computation, not only where it runs**

Learning: Control computation depth, speculation, and parallelism within explicit quality and resource limits, with deterministic enforcement.

Anunix comparison: Routing scores and cognitive budgets exist. Joint execution-shape changes need explicit quality bounds and admission checks.

Evidence: [kernel/core/route/planner.c](../../kernel/core/route/planner.c), [kernel/core/route/budget.c](../../kernel/core/route/budget.c), [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h).

Planned acceptance: Reject an execution-shape proposal outside its quality or resource contract before changing the active plan.

Status: Queued. Source message: `598f8149-e0fb-47a4-a0bb-975e0b26d183`.

## Day 078 — 2026-09-11

**AI-first OS update — September 11: treat agent idle time and lineage as schedulable resources**

Learning: Use agent phases, idle intervals, lineage, and shared origins to schedule state movement while preserving capacity and correctness.

Anunix comparison: Cell lineage and memory tiers exist. State movement does not yet combine idle phases with lineage-aware eligibility.

Evidence: [kernel/include/anx/cell.h](../../kernel/include/anx/cell.h), [kernel/core/mem/memplane.c](../../kernel/core/mem/memplane.c), [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c).

Planned acceptance: Permit eligible state movement during a declared idle phase and reject a stale or incompatible phase assumption.

Status: Queued. Source message: `04ea316e-9606-4726-8f4f-cf54680e81c2`.

## Day 079 — 2026-09-12

**AI-first OS update — September 12: schedule phases, progress, and state versions**

Learning: Bind scheduling decisions to execution phases and state versions, then revalidate those assumptions at commit.

Anunix comparison: Routing trials use policy generations, and effects use fence epochs. Decisions spanning multiple changing resources need a common validation boundary.

Evidence: [kernel/core/twin/twin.c](../../kernel/core/twin/twin.c), [kernel/core/route/tuning.c](../../kernel/core/route/tuning.c), [kernel/core/cap/effect_fence.c](../../kernel/core/cap/effect_fence.c).

Planned acceptance: Reject a decision after any required state version changes; accept an unchanged observation at the commit boundary.

Status: Queued. Source message: `01591a3b-c89f-42b3-bdfb-2c93786e2f23`.

## Day 080 — 2026-09-13

**AI-first OS update — September 13: reversible execution is becoming a kernel primitive**

Learning: Treat speculation as a bounded transaction with isolated mutable state, buffered external effects, and explicit commit or abort.

Anunix comparison: Staged mutations, pending effects, and run fences exist. A shared speculation domain must coordinate state and external-effect admission.

Evidence: [kernel/core/cap/effect.c](../../kernel/core/cap/effect.c), [kernel/core/state/stage.c](../../kernel/core/state/stage.c), [kernel/core/exec/runtime.c](../../kernel/core/exec/runtime.c).

Planned acceptance: Abort a speculative branch without dispatching its buffered effects; admit only the validated branch at commit.

Status: Queued. Source message: `8e6b3d50-036a-46c2-b5f4-8c5127cf88f1`.

## Day 081 — 2026-09-14

**AI-first OS update — September 14: compile intelligence into a closed policy space, then let AI select**

Learning: Keep runtime policy selection inside a previously validated, finite action space.

Anunix comparison: Finite routing profiles and typed tuning actions exist. A versioned policy catalog with indexed selection still needs an explicit runtime boundary.

Evidence: [kernel/core/route/profile.c](../../kernel/core/route/profile.c), [kernel/core/route/tuning.c](../../kernel/core/route/tuning.c), [kernel/core/regime/regime.c](../../kernel/core/regime/regime.c).

Planned acceptance: Select only issued policies through a bounded index; prove invalid or stale selections retain the validated fallback.

Status: Queued. Source message: `2b0646b7-8ce0-4118-a447-80e8979d269b`.
