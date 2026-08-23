# RFC-0029: Resource Twin and Regime-Gated Scheduling Policy

| Field      | Value                                                                 |
|------------|------------------------------------------------------------------------|
| RFC        | 0029                                                                    |
| Title      | Resource Twin and Regime-Gated Scheduling Policy                       |
| Author     | Adam Pippert                                                           |
| Status     | Draft                                                                   |
| Created    | 2026-08-22                                                             |
| Depends On | RFC-0002, RFC-0005 (Routing Plane and Unified Scheduler), RFC-0007, RFC-0021 |
| Blocks     | —                                                                       |

---

## Executive Summary

The Routing Plane and Unified Scheduler (RFC-0005) make real, live decisions
today — `anx_route_score_engine()` scores engines against a cell's
requirements, `anx_route_plan()` picks a winner. Nothing currently stops an
AI-generated change to that scoring policy from being applied on the
strength of one good-looking run. This RFC adds the missing gate: a
scheduling/routing policy change is only ever considered when telemetry has
genuinely left its normal operating envelope, is only evaluated by
simulating it against a frozen snapshot rather than live state, and is only
promoted once it has beaten a frozen incumbent's own repeated-run noise
floor — not merely scored higher once.

This RFC adds four pieces, collapsed into one coherent design rather than
kept as four separate half-features:

1. **Resource Twin** — a value-copy snapshot of the engine registry and
   scheduler queue depths, supporting cheap what-if routing simulation
   under a candidate weight policy without touching live state.
2. **Regime Detector** — a small, deterministic two-EWMA change-point
   detector that decides *whether* telemetry has left a validated
   envelope. It never proposes or evaluates policy itself.
3. **Cognitive Envelope** — a reasoning/token budget attached to a cell at
   admission time, ahead of the anxml (RFC-0021) dispatch it may
   eventually reach.
4. **Measured-null promotion gate** — extends RFC-0007's Capability
   Object lifecycle: no candidate that supersedes an installed incumbent
   installs without first beating that incumbent's noise floor by a
   margin that scales with how many candidates were tried.

The topic plan's fifth idea, a Readiness Contract ("exists" / "usable" /
"ready" / "healthy under load"), is small enough to fold into the Resource
Twin as `enum anx_readiness` rather than becoming its own subsystem.

---

## 1. Motivation

An AI system proposing a scheduling or routing policy change is exactly
the kind of probabilistic reasoning this OS's design keeps out of the
privileged hot path (RFC-0001). But today, nothing in RFC-0005 or RFC-0007
distinguishes "a policy that is actually better" from "a policy that
happened to score better once, possibly from noise." Continuously
re-evaluating policy on every request is also wasteful and destabilizing —
most of the time the current deterministic policy is fine, and the
question worth asking is not "should we change policy right now" but
"has anything changed enough to be worth asking that question at all."

## 2. Resource Twin

```c
struct anx_resource_twin {
	struct anx_twin_engine_snapshot engines[ANX_TWIN_MAX_ENGINES];
	uint32_t engine_count;
	uint32_t queue_depth[ANX_QUEUE_CLASS_COUNT];
	anx_time_t taken_at;
};
```

`anx_twin_snapshot()` copies the engine registry (via the same
`anx_engine_find()` sweep across all engine classes that `anx_route_plan()`
performs) and the current scheduler queue depths into an immutable value
snapshot. `anx_twin_simulate()` then replays feasibility and weighted
scoring against that frozen snapshot for a given cell, under a caller-
supplied `struct anx_route_weight_policy` — the same scoring dimensions
`anx_route_score_engine()` applies (locality, local-first bonus, GPU/CPU
cost, degraded penalty, private-data bonus, topology affinity), but as
tunable weights instead of hardcoded constants.

**Scope boundary.** The Twin intentionally does not include the JEPA
route-score delta (`anx_jepa_route_score_delta()`) — a live, stateful,
non-reproducible predictor term that cannot be meaningfully replayed
against a frozen snapshot — nor RFC-0005's escalation heuristic. It
simulates the deterministic weighted-scoring component of routing only.
`anx_route_weight_policy_incumbent()` reproduces the live function's
current constants, so simulating with the incumbent policy against a
snapshot taken immediately before a live `anx_route_plan()` call should
reproduce the same winner — this is a useful cross-check, not just a
convenience default.

**Readiness Contract.** `enum anx_readiness` (`NONE` / `USABLE` / `READY`
/ `HEALTHY`) is derived per-engine from `enum anx_engine_status` at
snapshot time (`anx_readiness_from_status()`). It is descriptive metadata
on the snapshot, not an independent gate.

**Restoration working set.** The topic plan's notes distinguish *retained*
state from *restoration working set* (e.g., a large checkpoint doesn't
need its full size in free RAM to resume if restoration streams). Anunix
has no such restoration/checkpoint mechanism yet to model — this RFC does
not invent placeholder fields for a capability that does not exist. When
checkpoint/restore lands, the Twin's snapshot struct is the natural place
to add it.

## 3. Regime Detector

```c
enum anx_regime_state {
	ANX_REGIME_STABLE,
	ANX_REGIME_ESCALATED,
};

int anx_regime_observe(int64_t sample);
enum anx_regime_state anx_regime_current(void);
```

A single global detector (matching the singleton-with-init() convention
of `sched/scheduler.c` and `route/feedback.c`) tracks two EWMAs over a
caller-chosen integer metric — a slow baseline (window 32) and a fast
tracker (window 4). It escalates when the two diverge past
`ANX_REGIME_ESCALATE_THRESHOLD` and returns to stable once they
reconverge under the smaller `ANX_REGIME_STABLE_THRESHOLD` — a hysteresis
band, so the detector does not flap at the boundary. `ANX_REGIME_MIN_SAMPLES`
suppresses escalation until the slow baseline has had time to converge
from a cold start.

This is deliberately not an LLM and not a proposal mechanism — it answers
one question only: has telemetry left the envelope. Escalation is the
trigger to consider running a (separately gated) policy proposal; it is
not a promotion decision and carries no authority of its own.

The detector's input is a plain `int64_t` sample, not a live hardware
read — the caller (a future integration point, not built here) decides
what to feed it, for example `observed_latency_ns` from an
`anx_route_feedback` record (RFC-0005). This keeps the detector itself
fully testable with synthetic data in the host-native test harness, which
has no real scheduling hardware to observe.

## 4. Cognitive Envelope

```c
struct anx_cognitive_envelope {
	uint32_t max_tokens;		/* 0 = unset */
	uint32_t max_reasoning_depth;	/* 0 = unset; reserved */
};
```

Embedded in `struct anx_cell` alongside RFC-0003's execution contract
(T3), set via `anx_cell_set_cognitive_envelope()` — same precedent as
`anx_cell_set_contract()` and `anx_cell_set_topology()`: valid only while
the cell is `ANX_CELL_CREATED`, zero value means unset and reproduces
today's behavior exactly (RFC-0002 DG-8: minimal overhead for the
unused case).

**Scope boundary: not wired into anxml dispatch yet.** `anx_anxml_cell_dispatch()`
(RFC-0021) is reached through a function-pointer table in
`workflow_exec.c` with the signature `(intent, in_oids, in_count,
out_oid_out)` — no cell context reaches it today. Threading a `struct
anx_cell *` (or the envelope alone) through that whole dispatch table is
a broader signature change across every registered cell-dispatch
function, out of scope for this RFC. `anx_cell_set_cognitive_envelope()`
is a real, independently tested primitive; wiring it into the live
dispatch path is future work, tracked here rather than silently
fabricated. This mirrors the same honest scope-boundary precedent RFC-0028
set for its own dispatch-path question (RFC-0028 Section 6).

## 5. Measured-Null Promotion Gate

```c
struct anx_promotion_trial {
	int32_t incumbent_scores[ANX_PROMOTION_TRIAL_MAX];
	int32_t candidate_scores[ANX_PROMOTION_TRIAL_MAX];
	uint32_t n;
};

int anx_promotion_gate_evaluate(const struct anx_promotion_trial *trial,
				uint32_t num_candidates_tried,
				bool *promote_out);

int anx_cap_install_gated(struct anx_capability *cap,
			  const struct anx_promotion_trial *trial,
			  uint32_t num_candidates_tried);
```

This extends RFC-0007's existing `ANX_CAP_VALIDATED -> ANX_CAP_INSTALLED`
promotion lifecycle rather than building a parallel mechanism, per the
topic plan's explicit direction. `struct anx_capability` already has a
`supersedes_oid` field describing exactly the scenario this gate
protects — a candidate meant to replace an installed incumbent. Before
this RFC, that field was descriptive only and unchecked by
`anx_cap_install()`. Now:

- `anx_cap_install()` refuses (`ANX_EPERM`) any candidate with a non-nil
  `supersedes_oid` — it must go through the gated path.
- `anx_cap_install_gated()` requires a `struct anx_promotion_trial` of
  paired scores (candidate run *i* vs. incumbent run *i*, same
  conditions, caller's choice of metric) and only installs if every
  paired margin clears `ANX_PROMOTION_MIN_MARGIN_BASE * num_candidates_tried`
  — a Bonferroni-style linear scaling of the required signal, not a
  formal multiple-comparisons correction.
- A fresh install with no incumbent (`supersedes_oid` nil) is unaffected
  and installs through `anx_cap_install()` exactly as before RFC-0029.

**Why a worst-case min-margin test, not a t-test.** This kernel builds
with `-mgeneral-regs-only` by default; floating point is enabled only for
a small whitelist of directories (`jepa/`, `loop/`, `rlm/`, `ebm/`,
`exec/jepa_cell.c`) that accept the accompanying restriction that code
there must never run in interrupt context (no FPU state save/restore at
interrupt entry — see the Makefile's `JEPA_CFLAGS` comment). A capability
promotion decision has no reason to take on that restriction just to
compute a mean and standard deviation, so this gate is deliberately
pure-integer: it takes the *worst* paired margin across the trial and
requires it alone to clear the threshold, rather than a statistic over
the whole sample. That is more conservative than a proper significance
test (a single bad paired run sinks an otherwise-strong candidate) and
gives up formal false-positive-rate guarantees in exchange for exact,
deterministic, freestanding-safe integer arithmetic. If Anunix later
wants a real paired t-test here, it needs either a floating-point
carve-out for this gate (with the same interrupt-context discipline the
JEPA files already follow) or a fixed-point statistics implementation —
neither is built here.

**Multiplicity correction is real, not cosmetic.** Because the required
margin scales linearly with `num_candidates_tried`, the same trial can
promote when only one candidate was tried and fail to promote when
several were tried in the same round — this is tested directly (see
Section 7).

## 6. Relationship to RFC-0028 (Protected Operation ABI)

RFC-0028's prepare/dispatch/settle effect protocol and this RFC's
promotion gate are independent — a capability promotion is not itself
modeled as a "protected operation" crossing a trust boundary, and this
RFC does not require RFC-0028's machinery to function. They compose
naturally in the future (an audited external effect could itself be the
data source for a promotion trial's paired scores) but nothing in this
RFC depends on that.

## 7. Tests

`tests/test_resourced_twin_simulate.c`, `tests/test_regime_detector.c`,
`tests/test_sched_cognitive_envelope.c`, `tests/test_cap_measured_null_promotion.c`
— see the design note (`docs/design/regime-gated-scheduling.md`) and the
test files themselves for exact cases covered, including the stationary
vs. non-stationary regime traces and the multiplicity-correction proof.

## 8. Open Questions

1. Once checkpoint/restore state exists, how should the Twin represent
   retained-vs-restoration-working-set memory without duplicating the
   Memory Control Plane's (RFC-0004) own accounting?
2. Should the Regime Detector eventually observe more than one metric
   stream (e.g., latency and queue depth together), and if so, does
   escalation require agreement across streams or any-one-trips-it?
3. What is the right integration point to thread cell/cognitive-envelope
   context through `workflow_exec`'s dispatch table without a broad
   signature change to every registered dispatch function?
4. Should `anx_promotion_gate_evaluate()` eventually support a real
   paired significance test behind a floating-point carve-out, or does
   the conservative worst-case margin test remain preferable specifically
   because it is simpler to reason about under adversarial candidate
   generation?

## 9. Decision Summary

1. The Resource Twin simulates RFC-0005's deterministic weighted routing
   score against a frozen snapshot; it does not model the JEPA delta or
   the escalation heuristic.
2. The Regime Detector is a small, deterministic, testable two-EWMA
   change-point detector — never an LLM, never a policy proposer.
3. The Cognitive Envelope is a real, tested, cell-admission-time
   primitive; wiring it into live anxml dispatch is explicitly deferred,
   not fabricated.
4. The measured-null promotion gate is mandatory for any capability that
   declares an incumbent to supersede, extends RFC-0007 rather than
   duplicating it, and is implemented as pure-integer arithmetic by
   deliberate choice given this kernel's FPU/interrupt-context
   constraints.
