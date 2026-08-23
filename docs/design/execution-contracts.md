# Design Note: Execution Contracts and Staged Mutation

Status: internal engineering note, not a governed spec. The governing
RFC text is RFC-0002 Section 4.4.3 and RFC-0003 Section 6.4 / 21.1 —
this document explains the reasoning behind those sections; if the two
ever disagree, the RFC text wins.

## What this is

Two additions, both extending existing subsystems rather than
introducing new ones:

1. **Execution Contract** — a cell can declare a `consistency` class
   (`best_effort`, `semantic`, `transactional`) and an `effect_mode`
   (`direct`, `staged`) before it is admitted. Default is
   `best_effort` / `direct`, which is exactly today's behavior.
2. **Staged mutation** — a State Object handle can open a stage
   (`anx_object_stage`), write against a private shadow copy, and
   resolve it with `anx_object_commit` or `anx_object_abort`. Nothing
   is visible to any other reader until commit.

## Why this, and why now

This is the first topic implemented from a review of AI-first-OS
research collected in `docs/notes/ai-os-topic-plan.md` (topic T3).
Three papers converged on the same idea from different angles:

- **YoloFS** ("Don't Let AI Agents YOLO Your Files") built a Linux
  filesystem around three primitives — introspect, undo, gate — so an
  agent's filesystem effects are staged and reversible instead of
  applied blind.
- **Agentic Transaction** defined four agent-level properties —
  atomicity, consistency, isolation, durability — as the right
  guarantees for agent effects, independent of whether the agent's
  *reasoning* is reproducible.
- **CoRun** showed that inference-level determinism (same weights,
  same batch shape, same RNG state → byte-identical output) is a
  separate, expensive, workload-specific property that should be
  requested, not assumed.

None of these are Anunix-shaped as written — YoloFS is a Linux kernel
filesystem, Agentic Transaction assumes an external agent framework
sitting on top of a conventional OS, CoRun is about GPU batch
scheduling in a model-serving engine. But the underlying claim in all
three — *effects should be stageable and resolvable, independent of
whether the process that produced them is deterministic* — maps
directly onto something Anunix already has: `struct anx_state_object`
already carries an explicit `version` counter and an append-only
provenance log. Staging didn't need a new filesystem; it needed one
more pointer field and three functions.

## Why an out-of-line shadow, not an inline field

`struct anx_staged_mutation` is allocated separately and referenced by
a nullable pointer (`obj->staged`), rather than embedding shadow-copy
fields directly in `struct anx_state_object`. RFC-0002's Design Goal
DG-8 ("minimal overhead for simple cases") is explicit about this: an
object that never stages should not pay for the feature. A null
pointer is the entire cost. The alternative — embedding
`shadow_payload` / `shadow_size` / etc. directly — would add three
fields to every State Object in the system, the overwhelming majority
of which will never stage anything.

## Why one stage at a time, not MVCC

The obvious next question is "what if two cells want to stage the
same object concurrently?" The answer here is: they don't get to. A
second `anx_object_stage` call on an already-staged object returns
`ANX_EBUSY`. This is a deliberate simplification, not a placeholder
for a future MVCC design — full multi-version concurrency control
(multiple concurrent shadow copies, conflict detection, merge or
last-writer-wins semantics) is a materially harder problem than
"stage, commit, abort," and nothing in the source research demanded
it. If concurrent staged writers on the same object turn out to be a
real requirement, that's a new design, not an extension of this one.

## Why abort still writes a provenance event

The tempting simplification is: if a stage is aborted, nothing
happened, so there's nothing to record. YoloFS's authors explicitly
reject this — their "travel" operation appends a history marker
instead of truncating the journal, on the principle that *rollback is
not the same as erasing evidence*. An aborted stage is exactly the
kind of thing an audit trail should show: an agent attempted a
mutation and it did not happen. `ANX_PROV_STAGE_ABORTED` exists for
this reason. It costs one provenance log entry, which is cheap, and it
means "why does this object have a gap in its version history" is
never an unanswerable question.

## Why seal/delete are rejected while staged

The alternative was silently discarding the pending stage when an
object is sealed or deleted out from under it. That's the wrong
failure mode for exactly the reason above: a discarded effect should
never disappear without a trace. Requiring the stage to be resolved
first (commit or abort) before the object's lifecycle can advance
means there is always an explicit decision recorded for what happened
to a pending mutation — never an implicit one made by whichever
operation happened to run first.

## What's deliberately out of scope

- **Inference determinism** (the CoRun thread) is not part of this.
  Pinning model weights, batch shape, and RNG state for byte-identical
  inference output is a property of the model-serving runtime
  (RFC-0021, `anxml`), not the kernel's object or cell model. A
  cell's `consistency` class governs its *effects*; it says nothing
  about whether the model that produced those effects is itself
  reproducible. If `anxml` later wants to expose a determinism knob,
  it can be layered on top of `semantic`/`transactional` consistency,
  not folded into it.
- **Runtime wiring in `anx_cell_run`.** `runtime_commit()` in
  `kernel/core/exec/runtime.c` is currently a stub — it does not yet
  write output State Objects; that wiring is still pending the Memory
  Control Plane (RFC-0004) integration. The Execution Contract field
  and the staged object API are both real and independently testable
  today, but the actual "commit calls `anx_object_commit` on staged
  outputs" integration is future work, tracked by the existing
  `runtime_commit` stub comment, not invented here. Building that
  integration now would mean writing code against a write path that
  doesn't exist yet — better to land the primitive and wire it when
  the commit path is real.
