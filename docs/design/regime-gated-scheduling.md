# Design Note: Regime-Gated Scheduling Policy

Status: internal engineering note, not a governed spec. The governing RFC
text is RFC-0029, with a cross-reference added to RFC-0007 §30 — this
document explains the reasoning behind that text; if the two ever
disagree, the RFC text wins.

## What this is

Four additions, collapsed into one coherent design rather than kept as
four separate half-features (the topic plan's fifth idea, a Readiness
Contract, folds into the first):

1. **Resource Twin** — a value-copy snapshot of the engine registry and
   scheduler queue depths, supporting what-if routing simulation under a
   candidate scoring policy without touching live state.
2. **Regime Detector** — a small, deterministic two-EWMA change-point
   detector that decides *whether* a policy proposal is worth considering
   at all.
3. **Cognitive Envelope** — a reasoning/token budget attached to a cell
   at admission time.
4. **Measured-null promotion gate** — extends RFC-0007's capability
   promotion lifecycle so a candidate that supersedes an installed
   incumbent must beat its noise floor, not just score higher once.

## Why the Twin simulates routing specifically, not "scheduling" in general

RFC-0005's two live, concrete decisions are `anx_route_score_engine()`
(which engine wins) and `anx_sched_enqueue()`/`anx_sched_dequeue()`
(FIFO-within-priority queueing — nothing there is a scored decision worth
simulating). The Twin targets the first because it is the one place a
candidate *policy* — as opposed to a candidate *priority number* — could
plausibly want evaluating before being trusted. `anx_twin_simulate()`
mirrors `anx_route_score_engine()`'s exact scoring dimensions (locality,
local-first bonus, GPU/CPU cost, degraded penalty, private-data bonus,
topology affinity) as a `struct anx_route_weight_policy` of tunable
weights, and `anx_route_weight_policy_incumbent()` reproduces the live
function's current hardcoded constants — so simulating with the incumbent
policy against a snapshot taken immediately before a real
`anx_route_plan()` call should reproduce the same winner. That
equivalence is exercised directly in `tests/test_resourced_twin_simulate.c`
and is the thing that makes the Twin trustworthy as a stand-in for "what
would actually happen," not just a plausible-looking parallel
implementation.

**What's deliberately left out.** `anx_jepa_route_score_delta()` — a
live, stateful, non-reproducible predictor term — is not part of the
simulated score. Replaying a snapshot is only meaningful if replaying it
twice gives the same answer; a live model call breaks that. RFC-0005's
escalation heuristic (ambiguous top-two candidates, low best score) is
also not modeled. The Twin answers "who wins under this weight policy,"
not "should this route be escalated to a slower planning stage" — a
different, and already-served, question.

## Why the Regime Detector is two EWMAs with hysteresis, not something fancier

The requirement was: deterministic, testable with synthetic data (no
real time source in the host-native harness), and correctly returns to
stable once a shift has settled rather than staying tripped forever. A
slow EWMA (window 32) tracks the long-run baseline; a fast EWMA (window
4) tracks recent behavior. Escalate when they diverge past 200 units;
return to stable only once they reconverge under 50 units — the gap
between those two thresholds is hysteresis, so noise sitting right at
the boundary doesn't flap the state back and forth every sample.
`ANX_REGIME_MIN_SAMPLES` (32, matching the slow window) suppresses
judgment during cold start, before the slow baseline has had time to
mean anything.

This is not a general-purpose statistics library and doesn't try to be.
It answers one question — has telemetry left the envelope — and hands
that off; it never proposes, scores, or approves a policy itself. What
metric actually feeds it (route feedback latency, queue depth, something
else) is intentionally left to the caller — `anx_regime_observe()` takes
a plain `int64_t`, not a live hardware read, specifically so the detector
itself stays fully unit-testable.

## Why the Cognitive Envelope isn't wired into anxml dispatch

`anx_cell_set_cognitive_envelope()` is real and independently tested —
default zero means unset, matching RFC-0002 DG-8's minimal-overhead
discipline and the exact precedent T3's `anx_cell_set_contract()` and the
pre-existing `anx_cell_set_topology()` set. What it does *not* do yet is
change what `anx_anxml_cell_dispatch()` actually does, because that
function is reached through a function-pointer table in
`workflow_exec.c` with signature `(intent, in_oids, in_count,
out_oid_out)` — no cell context reaches it. Threading cell (or even just
the envelope) through every registered cell-dispatch function is a
broader signature change than this RFC's scope. This mirrors the same
scope boundary RFC-0028 drew around `anx_external_invoke()`'s dispatch
path (RFC-0028 §6): build the real, tested primitive; document the
integration gap; don't fabricate a call site that isn't there.

## Why the promotion gate is a worst-case min-margin test, not a t-test

This kernel builds with `-mgeneral-regs-only` by default. Floating point
is enabled only for `jepa/`, `loop/`, `rlm/`, `ebm/`, and
`exec/jepa_cell.c` — and that whitelist comes with a real constraint
those files accept: JEPA must never run in interrupt context, because
there's no FPU state save/restore at interrupt entry (see the Makefile's
`JEPA_CFLAGS` comment). A capability promotion decision has no reason to
inherit that constraint just to compute a mean and standard deviation.

So `anx_promotion_gate_evaluate()` is pure integer arithmetic: given
paired scores (candidate run *i* vs. incumbent run *i*, same conditions),
it takes the single *worst* margin across the whole trial and requires
that alone to clear `ANX_PROMOTION_MIN_MARGIN_BASE * num_candidates_tried`.
This is strictly more conservative than a proper paired significance
test — one bad run sinks an otherwise-strong candidate, where a t-test
might still pass on the strength of the average. That's a real,
acknowledged tradeoff: exact and deterministic in exchange for giving up
a formal false-positive-rate guarantee. `tests/test_cap_measured_null_promotion.c`
proves both halves of the mechanism directly: a trial with one negative
paired margin never promotes regardless of a decent mean (Case 2), and
the identical trial promotes at `num_candidates_tried=1` but not at
`num_candidates_tried=4` (Case 3) — the multiplicity correction changes
the actual outcome, not just a number nobody reads.

If Anunix later wants a real paired t-test here, it needs either a
floating-point carve-out for this one gate (with the same interrupt-
context discipline the JEPA files already follow) or a fixed-point
statistics implementation. Neither is built in this RFC.

## Why the gate lives in `anx_cap_install_gated()`, not a new promotion subsystem

`struct anx_capability` already has a `supersedes_oid` field describing
exactly the scenario this gate protects — a candidate meant to replace an
installed incumbent. Before RFC-0029 that field was descriptive only and
unchecked. Adding the gate as a hard requirement on that existing field,
rather than a parallel "promotion service," means every future capability
that declares an incumbent automatically goes through it — there's no
second code path to remember to call. A fresh install with no incumbent
is completely unaffected: `anx_cap_install()` still works exactly as it
did before this RFC, for exactly the case it always handled.

## What this does not claim

The Regime Detector's thresholds (200/50 units, windows 32/4) are
reasonable defaults for a first implementation, not tuned against any
real Anunix workload — there is no real workload driving them yet. The
promotion gate's `ANX_PROMOTION_MIN_MARGIN_BASE` (5) is similarly a
starting point. Both are constants specifically so they're easy to find
and revisit once real telemetry exists to tune against.
