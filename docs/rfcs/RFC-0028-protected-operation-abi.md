# RFC-0028: Protected Operation ABI — Information-Flow Labels and the Prepare/Dispatch/Settle Effect Protocol

| Field      | Value                                                      |
|------------|-------------------------------------------------------------|
| RFC        | 0028                                                         |
| Title      | Protected Operation ABI — Information-Flow Labels and the Prepare/Dispatch/Settle Effect Protocol |
| Author     | Adam Pippert                                                 |
| Status     | Draft                                                        |
| Created    | 2026-08-22                                                   |
| Depends On | RFC-0002, RFC-0003 (Execution Contracts extension), RFC-0007, RFC-0008 |
| Blocks     | —                                                             |

---

## Executive Summary

Capability Objects (RFC-0007) and Credential Objects (RFC-0008) answer one
question: **can this actor invoke this operation.** They do not answer a
second, independent question: **may this data go to that destination.** A
cell holding a valid capability to make an external call is not thereby
authorized to send arbitrary sensitive State Object payloads through that
call — those are two separate authorizations, and collapsing them into one
check either becomes too permissive (any authorized caller can exfiltrate
anything) or too coarse (every operation needs its own bespoke data
whitelist instead of a reusable classification).

This RFC adds:

1. **Information-flow labels** — a `sensitivity` classification on State
   Objects (RFC-0002), inherited by derived objects rather than defaulting
   silently to public.
2. **Sinks** — named destinations with a declared sensitivity ceiling.
   `anx_sink_check_send()` is `CAN_SEND`, evaluated independently of
   whatever capability check already gates `CAN_CALL`.
3. **The prepare/dispatch/settle effect protocol** — a one-way state
   machine (`PREPARED → DISPATCHING → {COMMITTED, RESTORED, UNKNOWN}`) for
   any operation crossing a trust or process boundary, so that a timeout or
   fault mid-dispatch produces an explicit, terminal `UNKNOWN` state instead
   of being silently treated as success, failure, or a candidate for retry.

This is the native-C form of the same idea Agent libOS v3 (external
research, August 2026) uses in userspace: action surface, authority,
information flow, and evidence are four separate properties, and a new
tool or Skill can expand the action surface without automatically
expanding authority or information-flow reach.

---

## 1. Motivation

RFC-0007's Capability Objects model *installable engine competence* — a
capability is validated, installed, and becomes a routable engine. RFC-0008's
Credential Objects gate *secret access*. Neither answers "given that this
cell may perform side effects at all, which specific data is it allowed to
move to which specific destination." Today nothing in Anunix asks that
question — `anx_external_invoke()` (RFC-0003 §7) dispatches to a registered
scheme handler with no authority or data-flow check at all.

The gap matters once a cell can compose capabilities freely: a capability
to call an external API and a capability to read a confidential State
Object are each individually legitimate, but their combination — sending
the confidential object's payload through that API — is a decision neither
capability alone was scoped to make.

## 2. The Four-Property Model

| Property         | Question                                   | Anunix mechanism |
|-------------------|---------------------------------------------|-------------------|
| Action surface     | What can the model/cell ask to do?          | Cell types, tool/Skill registration (RFC-0003, RFC-0018) |
| Authority           | What protected resources may it affect?     | Capability Objects (RFC-0007), execution policy (RFC-0003 §14) |
| Information flow    | Where may derived information go?           | **This RFC** — sensitivity labels + Sinks |
| Evidence             | What proves what happened?                  | Provenance log (RFC-0002 §4.6), Execution Contracts staged mutation (RFC-0003 extension) |

The load-bearing rule: expanding the action surface (a new Skill, a new
external-call scheme) never automatically expands authority or
information-flow reach. Each of the four properties is checked
independently at the point an operation actually executes.

## 3. Information-Flow Labels

### 3.1 Sensitivity

```c
enum anx_sensitivity {
	ANX_SENSITIVITY_PUBLIC,
	ANX_SENSITIVITY_INTERNAL,
	ANX_SENSITIVITY_CONFIDENTIAL,
	ANX_SENSITIVITY_RESTRICTED,
};
```

Every `struct anx_state_object` carries a `sensitivity` field (default
`ANX_SENSITIVITY_PUBLIC`, the zero value — no cost for objects that never
declare otherwise, per RFC-0002 DG-8) and a `sensitivity_origin` OID
recording which parent the classification was inherited from, if any.

### 3.2 Inheritance at Creation

`anx_so_create()` inherits the **maximum** sensitivity among `parent_oids`
when no explicit override is given, rather than defaulting a derived
object to public regardless of what it was built from. A caller that
genuinely wants a derived object to be public despite sensitive parents
must call `anx_object_set_sensitivity()` explicitly after creation — the
zero-value default and the "explicit public" case share the same enum
value, so this is a deliberate act, not an accidental default.

### 3.3 API

```c
int anx_object_set_sensitivity(const anx_oid_t *oid, enum anx_sensitivity level);
enum anx_sensitivity anx_object_get_sensitivity(const anx_oid_t *oid);
```

Implemented in `kernel/core/state/flow.c`.

## 4. Sinks

A Sink names a destination and the sensitivity ceiling it is authorized to
receive:

```c
struct anx_sink {
	char name[64];
	enum anx_sensitivity max_sensitivity;
};

int anx_sink_register(const char *name, enum anx_sensitivity max_sensitivity,
		      struct anx_sink **out);
struct anx_sink *anx_sink_lookup(const char *name);
int anx_sink_check_send(const struct anx_sink *sink, const anx_oid_t *object_oid);
```

Implemented in `kernel/core/cap/sink.c`, registered at boot via
`anx_sink_registry_init()` (`kernel/core/main.c`). `anx_sink_check_send()`
returns `ANX_EPERM` when the object's sensitivity exceeds the Sink's
ceiling — the same permission-denied convention `anx_cap_install()` and
`anx_credential_read()` already use elsewhere in the kernel.

## 5. The Prepare/Dispatch/Settle Effect Protocol

### 5.1 States

```
PREPARED ──▶ DISPATCHING ──┬──▶ COMMITTED
                            ├──▶ RESTORED
                            └──▶ UNKNOWN
```

All three DISPATCHING successors are terminal. `UNKNOWN` exists because a
timeout or fault mid-dispatch genuinely does not tell you whether the
external side effect happened — treating it as success is wrong, treating
it as failure and retrying is potentially worse (a duplicate side effect).
The state machine makes "we don't know" a first-class, permanent answer
that nothing downstream can silently resolve later.

### 5.2 API

```c
int anx_effect_prepare(anx_cid_t cell, struct anx_sink *sink,
		       const anx_oid_t *object_oid,
		       struct anx_pending_effect **out);
int anx_effect_mark_dispatching(struct anx_pending_effect *effect);
int anx_effect_commit(struct anx_pending_effect *effect);
int anx_effect_restore(struct anx_pending_effect *effect);
int anx_effect_mark_unknown(struct anx_pending_effect *effect);
void anx_effect_destroy(struct anx_pending_effect *effect);
```

Implemented in `kernel/core/cap/effect.c`.

`anx_effect_prepare()` is the enforcement point for both new gates:

- **CAN_CALL** — the calling cell's `execution.allow_side_effects`
  (RFC-0003 §14 execution policy) must be true. Anunix has no per-cell,
  per-operation capability-check function distinct from a Capability
  Object's own installation lifecycle (RFC-0007 models *installable engine
  competence*, not *per-invocation actor authorization*), so this RFC
  reuses the execution-policy field that already exists for exactly this
  purpose rather than inventing a parallel authority primitive.
- **CAN_SEND** — `anx_sink_check_send()`, skipped only when the effect has
  no Sink at all (a pure control operation with no data-flow component).

Either denial is independent of the other: a cell with `allow_side_effects
= true` is still blocked from sending a `CONFIDENTIAL` object through a
Sink whose ceiling is `INTERNAL`.

### 5.3 Relationship to Execution Contracts (RFC-0003 extension)

RFC-0003's Execution Contracts extension gave State Object mutation a
stage → commit/abort path so a pending mutation is never visible until
resolved. This RFC's `anx_pending_effect` is the same idea applied to
effects that leave the object store entirely (network calls, external
processes, credential-gated devices) rather than to in-place object
mutation — both exist so that "in progress" and "resolved" are distinct,
observable states instead of an operation just... happening.

## 6. Scope Boundary: Not Wired Into Cell Dispatch Yet

`anx_external_invoke()` (RFC-0003 §7, `kernel/core/exec/external.c`) is a
real, working dispatch path — unlike some stubs elsewhere in the runtime,
`ANX_CELL_TASK_EXTERNAL_CALL` cells already reach it end-to-end. This RFC
deliberately does **not** modify that dispatch path to call
`anx_effect_prepare()` automatically. Doing so would change the behavior
of every existing external-call cell and test without a specific Sink
policy having been designed for each scheme handler. `anx_effect_prepare`,
`anx_sink_register`, and `anx_object_set_sensitivity` are real,
independently tested primitives available for `runtime.c`'s dispatch code
to adopt; wiring them in as the default enforcement path for
`ANX_CELL_TASK_EXTERNAL_CALL` is future work, tracked as an open question
below rather than bundled silently into this RFC.

## 7. Open Questions

1. When `anx_effect_prepare` is wired into `anx_external_invoke`'s
   runtime path, which Sink does a given scheme handler check against —
   one Sink per scheme, or a Sink declared per-cell alongside the call?
2. Should `UNKNOWN` effects be enumerable (a query surface: "list all
   effects this cell has left unresolved") so an operator or a later
   reconciliation pass can inspect them, rather than only being visible to
   whichever caller happened to hold the `struct anx_pending_effect *`?
3. `anx_pending_effect` records currently live only in kernel heap memory
   — they do not survive a reboot. The topic-plan research this RFC draws
   from (Agent libOS v3's prepare/dispatch/settle) assumes a *durable*
   pending-effect record so a crash mid-dispatch can be reconciled on
   restart. Making that durable requires backing `anx_pending_effect` with
   a disk-persisted State Object rather than a heap struct — deferred
   pending a concrete need, consistent with how the RFC-0003 Execution
   Contracts extension also stopped short of reboot-durability for staged
   mutations.

---

## 8. Decision Summary

1. Capability/authority (`CAN_CALL`) and information flow (`CAN_SEND`) are
   two independently-evaluated gates, never merged into one check.
2. State Objects carry a `sensitivity` label, defaulting to public and
   inherited as the maximum of derivation parents rather than defaulting
   down silently.
3. Sinks name destinations with a sensitivity ceiling; sending exceeding
   data is `ANX_EPERM`, matching the rest of the kernel's permission-denied
   convention.
4. Any operation crossing a trust boundary gets an explicit
   prepare/dispatch/settle lifecycle, with an ambiguous outcome
   (`UNKNOWN`) as a first-class terminal state rather than an implicit
   success or failure.
5. Wiring this protocol into the live external-call dispatch path is
   explicitly out of scope for this RFC — the primitives are real and
   tested; the integration is a deliberate, separate follow-on.
