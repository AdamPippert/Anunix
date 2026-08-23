# Design Note: Protected Operation ABI

Status: internal engineering note, not a governed spec. The governing RFC
text is RFC-0028, with cross-references added to RFC-0007 §29 and
RFC-0008 §18 — this document explains the reasoning behind that text; if
the two ever disagree, the RFC text wins.

## What this is

Three additions, building on Execution Contracts
([`execution-contracts.md`](execution-contracts.md)) rather than
introducing a parallel mechanism:

1. **Information-flow labels** — every State Object carries a
   `sensitivity` classification (`PUBLIC`/`INTERNAL`/`CONFIDENTIAL`/
   `RESTRICTED`), defaulting to public and inherited as the maximum of a
   derived object's parents.
2. **Sinks** — named destinations declaring the sensitivity ceiling they
   may receive. `anx_sink_check_send()` is `CAN_SEND`.
3. **Prepare/dispatch/settle** — a one-way state machine for any effect
   crossing a trust boundary: `PREPARED → DISPATCHING → {COMMITTED,
   RESTORED, UNKNOWN}`. `anx_effect_prepare()` enforces both `CAN_CALL`
   (existing RFC-0003 execution policy) and `CAN_SEND` before returning a
   pending-effect record the caller must resolve.

## Why this, and why now

This is the second topic implemented from
`docs/notes/ai-os-topic-plan.md` (topic T4), following T3's Execution
Contracts. The source research — Agent libOS v3, submitted August 2026 —
makes one distinction that most "agent OS" designs collapse: a tool
definition (the *action surface* a model sees) is not the same thing as
*authority* over a protected resource, which is not the same thing as
*information flow* (where derived data may travel), which is not the same
thing as *evidence* (what proves what happened). A new Skill or JIT tool
can expand the action surface without automatically granting filesystem,
network, or object authority — the protected primitive underneath stays
small, typed, and non-model-controlled.

Anunix already had two of the four properties. Capability Objects
(RFC-0007) and the RFC-0003 execution policy are authority. The
provenance log (RFC-0002 §4.6) plus T3's staged-mutation machinery are
evidence. Information flow was the missing property — nothing distinguished
"can call this operation" from "can send this specific data through it."

## Why capabilities and flow labels stay two checks, not one

The obvious shortcut is to fold sensitivity into the capability grant
itself — a capability that's scoped to "may only touch public data." That
collapses two orthogonal questions into one artifact: a capability would
need a separate variant for every sensitivity tier it might ever
encounter, and granting broader authority (e.g. installing a new engine
with a wider capability mask) would silently widen data-flow reach as a
side effect nobody asked for. Keeping them as two independent checks —
`CAN_CALL` unaffected by data sensitivity, `CAN_SEND` unaffected by which
capability authorized the call — means expanding one never silently
expands the other. This is the same principle T3 leaned on for staged
mutation: don't merge two guarantees that can be requested independently.

## Why CAN_CALL reuses execution policy instead of a new primitive

The topic plan's source material assumed a capability-check function
(`CAN_CALL(operation)`) would already exist to reuse. It doesn't — RFC-0007's
`anx_cap_*` functions model a Capability Object's own installation
lifecycle (draft → validated → installed, becoming a routable engine),
not "does this specific cell have permission to perform this specific
side effect right now." The primitive that already answers the second
question is RFC-0003's execution policy: `struct anx_execution_policy` has
carried `allow_side_effects` and `allow_network` since the Execution Cell
Runtime RFC was written. `anx_effect_prepare()` reuses that field rather
than inventing a parallel authority mechanism that would immediately need
reconciling with it.

## Why sensitivity is a raw struct field, not a metadata-store entry

`struct anx_state_object` already has a generic key-value metadata store
(`system_meta`) that could have carried `sys.flow.sensitivity` as a typed
entry, matching how other derived attributes are exposed. Two things
argued against it: first, every metadata-store write allocates an entry,
so even the default case would cost something unless callers were
disciplined about only writing non-default values — a raw field defaulting
to the zero value (`ANX_SENSITIVITY_PUBLIC`) costs literally nothing for
the common case, consistent with T3's `staged` pointer precedent. Second,
`object_type`, `version`, and `content_hash` — the fields the kernel is
expected to reason about without parsing the payload (RFC-0002 DG-2) — are
already raw struct fields, not metadata entries. Sensitivity belongs in
that company, not the open-ended user/system metadata namespace.

## Why inheritance takes the max, and why that creates a sentinel problem

A derived object built from a `RESTRICTED` parent and a `PUBLIC` parent
should not default to `PUBLIC` just because the create call didn't specify
otherwise — that would make sensitivity trivially droppable by deriving a
new object. Taking the maximum across `parent_oids` closes that gap. The
cost is that `ANX_SENSITIVITY_PUBLIC` (0) has to serve double duty as both
"explicitly declared public" and "no override given, please inherit." A
caller that actually wants to force a derived object down to public
despite sensitive parents has no way to express that at creation time —
they have to call `anx_object_set_sensitivity()` afterward. This is a
known, documented gap (see the `anx_so_create_params.sensitivity` comment
in `state_object.h`), not an oversight: adding a second field just to
disambiguate "explicit public" from "no opinion" felt like the wrong
tradeoff for a Draft-stage RFC, versus giving the escape hatch a one-line
comment and moving on.

## Why UNKNOWN is a dead end, not a retry state

The three-phase protocol's entire reason to exist is the observation
(from Agent libOS v3's prepare/dispatch/settle design) that a timeout
during an external effect's dispatch tells you nothing about whether the
effect happened. The two tempting simplifications are both wrong:
treating an ambiguous outcome as failure risks a duplicate side effect on
retry; treating it as success risks silently losing a failed effect. The
transition table in `effect.c` makes `UNKNOWN` a hard dead end — nothing
can transition out of it, by construction, not by convention. Anything
that later reads a `struct anx_pending_effect` and finds it `UNKNOWN` has
to make its own explicit decision about what to do next; the kernel will
not make that decision by default.

## What's deliberately out of scope

- **Wiring into `anx_external_invoke`'s dispatch path.** `runtime.c`'s
  `ANX_CELL_TASK_EXTERNAL_CALL` handling is real and already exercised by
  `tests/test_external_call.c` — unlike T3's `runtime_commit()` stub, this
  is not a case of "the write path doesn't exist yet." It was left
  unwired anyway, because turning on `anx_effect_prepare()` by default for
  every external call would change already-tested behavior without a
  concrete Sink policy having been designed per scheme handler (`pg://`,
  `http://`, `mock://`, ...). The primitives are real and independently
  tested; the integration is a deliberate follow-on tracked in RFC-0028 §7,
  not something to bolt on silently here.
- **Durable pending-effect records.** `struct anx_pending_effect` is a
  plain heap allocation — it does not survive a reboot. The research this
  RFC draws from assumes a durable record so a crash mid-dispatch can be
  reconciled on restart; building that requires backing the record with a
  disk-persisted State Object, which is a materially bigger change than
  this RFC's scope. T3's staged mutation made the same call for the same
  reason.
- **Per-scheme Sink policy.** This RFC gives Sinks a name and a ceiling;
  it does not decide which Sink a given `http://` or `pg://` call should
  check against. That's part of the same future wiring work above, not a
  gap in the primitive itself.
