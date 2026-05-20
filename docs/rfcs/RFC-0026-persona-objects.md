# RFC-0026: Persona Objects — Agent Identity, Custody, and Governance

| Field      | Value                                                                                            |
|------------|--------------------------------------------------------------------------------------------------|
| RFC        | 0026                                                                                             |
| Title      | Persona Objects — Agent Identity, Custody, and Governance                                        |
| Author     | Adam Pippert                                                                                     |
| Status     | Draft                                                                                            |
| Created    | 2026-05-12                                                                                       |
| Updated    | 2026-05-12                                                                                       |
| Depends On | RFC-0002, RFC-0003, RFC-0005, RFC-0007, RFC-0008, RFC-0009, RFC-0013, RFC-0018, RFC-0020, RFC-0025 |

---

## Executive Summary

An agent that can act on the world needs more than a process and a model. It needs an **identity** the world can verify, a **set of credentials** it can spend, a **set of capabilities** describing what it is permitted to do, a **set of skills** describing what it knows how to do, a **governance contract** describing the limits under which it operates, and a **continuous evaluation loop** that detects when it has drifted outside those limits. Today, Anunix has primitives for each of these pieces — Credentials (RFC-0008), Capability Objects (RFC-0007), Kits (RFC-0025), Workflows (RFC-0018), Memory (RFC-0009), and the IBAL energy loop (RFC-0020). It has no primitive that *binds them together as a single accountable actor*.

The kernel knows who owns a credential, but not which agent is allowed to read it under which conditions. The kernel knows which capabilities a session holds, but not which evaluator is responsible for revoking them when behavior degrades. The kernel knows which skill kits are enabled, but not which agent is permitted to invoke them. Without a Persona, every agent is implicitly the root user — distinguished only by which cell currently happens to hold the API key.

RFC-0026 introduces the **Persona** — a new `ANX_OBJ_PERSONA` State Object kind, an `anx_persona_*` kernel API, a `persona session` runtime envelope that threads through cell admission, and a uniform audit trail. A Persona binds an Ed25519 identity, a credential bundle with per-credential custody policy, a granted capability set, an installed kit set, a governance bundle (spend/rate/eval thresholds), one or more attached evaluation workflows, and a scoped memory binding. Every kernel operation performed by an agent flows through its active persona session; credential reads check custody policy, kit leases check installation, eval workflows sample actions, and threshold breaches automatically suspend the persona.

User accounts (RFC-0008 multi-key auth) are the degenerate case of a persona where the actor is a human. Agent personas are the general case. Treating both as personas unifies "who did what" across the OS — every action, human or agent, has a persona OID in its provenance entry.

The userland tooling that authors personas, holds custody material, runs evaluation suites, and packages skills lives in a new sibling repository: **Anunix-Persona**, scaffolded under `/home/adam/Development/Anunix/Anunix-Persona/` with its own git history and `make kit` target that produces kit bundles consumable by Anunix's `anx_kit_register`. The kernel primitive is in Anunix; the ecosystem is in Anunix-Persona.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert
**Depends on:** RFC-0002 (State Objects), RFC-0003 (Execution Cells), RFC-0005 (Routing / Engine leases), RFC-0007 (Capability Objects), RFC-0008 (Credential Objects), RFC-0009 (Agent Memory), RFC-0013 (Tensor Objects, for embedding-based eval), RFC-0018 (Workflows, for eval pipelines), RFC-0020 (IBAL energy scoring), RFC-0025 (Kits, for skill packages)
**Blocks:** Subsequent RFCs covering persona-to-persona delegation, multi-party custody, and federated personas.

---

## 2. Problem Statement

### 2.1 The Implicit Root User

Anunix today authenticates *connections* (RFC-0008 SSH keys, passwords, signed HTTP headers) but does not authenticate *actors* in the agent sense. A cell that holds an API credential can spend it. A cell that holds a kit lease can invoke the engine. A cell that touches memory can read everything in scope. The granularity of accountability is the cell — but cells are anonymous, ephemeral, and freely spawn child cells.

For human work this is acceptable. For agent work it is not. An agent that runs unattended for hours, calls dozens of tools, spends real money, and accumulates state must be a *named, accountable, governable* actor — not an anonymous cell chain. Without a Persona, "the agent did X" is an attribution claim with no kernel-level evidence.

### 2.2 Composition by Convention

Today, to give an agent the ability to "research the web with a budget cap and shut down if it starts hallucinating," an operator would have to:

1. Manually create a credential bundle and give the agent's cell a capability to read it.
2. Manually enable the browser kit and the python kit (once RFC-0025 lands).
3. Manually configure a separate process to monitor outputs and kill the agent on a heuristic.
4. Manually correlate the four resulting log streams (credential reads, cell spawns, kit leases, monitor decisions) to reconstruct what happened.

This is composition by convention. It works exactly as well as the operator's discipline allows. It cannot be audited because the binding between credential, capability, kit, eval, and actor exists only in the operator's head.

A Persona is the kernel object that makes the binding explicit, durable, and machine-checkable.

### 2.3 The Custody Gap

Agents that act in the world need to spend money — calling paid APIs, paying for compute, executing transactions. RFC-0008 stores credentials securely but treats them all the same: any cell with the capability to read a credential can read it without further check. For an API key with a $50/month cap, that is fine. For a hot wallet, a bank account, or a signing key that can move real funds, it is not.

Credentials need a **custody policy** — a per-credential rule that determines under what conditions the kernel will reveal the credential to a reading cell. Auto-use for low-stakes keys. Audit-on-use for medium-stakes. Step-up consent (fresh signature from the persona's identity key) for sensitive keys. Per-transaction human approval for the most sensitive. The policy lives on the *credential as bound to the persona*, not on the credential globally — different personas may have different policies for the same underlying credential material.

### 2.4 Drift, Evals, and Revocation

A persona's behavior changes over time as the world changes, the underlying models change, and the persona accumulates memory. A persona that was safe yesterday may not be safe today. Without continuous evaluation, drift is invisible until it produces a visible failure. With continuous evaluation — a small fraction of every action scored against retrieval-faithfulness, answer-relevance, harmfulness, and distribution-drift detectors — drift becomes a kernel-observable quantity. The persona's EBM energy (RFC-0020) reflects its current health; a breach automatically suspends the persona before the next sensitive action.

This is the same idea as runtime tests for code: the persona is a long-running entity, and like any long-running entity it needs an integrated health probe.

---

## 3. Goals

### 3.1 Primary Goals

1. **Persona as kernel primitive.** A new `ANX_OBJ_PERSONA` State Object kind. Personas are content-addressed, signed, provenance-tracked, persistent.

2. **One actor, one identity.** Every persona has an Ed25519 keypair. The public key is the persona's external identity (used to sign actions, authenticate to remote services, verify audit logs). The private key is custody material.

3. **Persona session envelope.** When a persona acts, it does so inside an `anx_persona_session` that propagates through cell admission. Every credential read, kit lease, capability check, and memory access inside the session is policy-checked against the persona.

4. **Per-credential custody policy.** Each credential bound to a persona has a custody policy: `auto`, `audit`, `step-up`, `human-approval`. The policy is enforced inside the persona session at every read.

5. **Granted capabilities and installed kits.** A persona has an explicit grant of which capabilities it holds and which kits it may lease. Globally-enabled kits are not implicitly usable by every persona; the persona must have the kit installed.

6. **Continuous evaluation.** A persona may have one or more eval Workflows (RFC-0018) attached, each with a threshold and a sample rate. Eval scores feed the persona's EBM energy (RFC-0020). Threshold breach auto-suspends the persona.

7. **Uniform audit.** Every action under a persona session produces a provenance entry tagged with the persona OID. `persona audit <uri>` is a query over RFC-0002 provenance — not a separate log.

8. **User accounts are personas.** The existing multi-key auth model (RFC-0008) becomes the `kind=user` case of a persona. Migration is mechanical: each user account spawns a persona during boot and the user's keys become the persona's identity.

### 3.2 Non-Goals

- **Persona portability across hosts.** A persona's private key never leaves its custody enclave. Federating a persona to a remote Anunix instance is a future RFC and requires either a shared HSM or a threshold-signing scheme.
- **A persona marketplace.** This RFC defines the primitive, not a directory or trust network of personas.
- **Replacing kits with personas.** Kits package code; personas authorize actors. A persona uses kits; it does not subsume them.
- **Forced evaluation.** A persona MAY have eval workflows attached. The kernel does not impose a default eval — that policy lives in the userland (Anunix-Persona) where default eval suites ship with each example persona.
- **End-user UI for authoring personas.** The kernel exposes the primitive; the UI lives in Anunix-Persona's `anxpersona` CLI and (later) a WM persona-manager surface.

---

## 4. Core Definitions

### 4.1 Persona Object (`ANX_OBJ_PERSONA`)

A **Persona Object** is a State Object of type `ANX_OBJ_PERSONA`. It contains:

- The persona's URI (`anx:persona/<name>`)
- The persona's identity (Ed25519 public key, plus an OID reference to the private key held as a Credential with custody policy)
- The credential binding map: credential URI → custody policy entry
- The granted capability set (array of `cap_oid`)
- The installed kit set (array of `kit_uri`)
- The governance bundle (spend limits, rate limits, audit retention)
- The attached eval workflows (array of `{workflow_oid, threshold, sample_rate}`)
- The memory scope binding (RFC-0009 memory namespace assigned to this persona)
- The current state (NASCENT / PROVISIONING / ACTIVE / SUSPENDED / REVOKED)
- The kind (`user` | `agent`)
- Signed metadata: created_at, created_by, signing chain

The persona object is small (typically <8 KiB). Heavy material — keypair, credentials, attached eval workflows, accumulated memory — lives in referenced State Objects.

### 4.2 Persona Session (`struct anx_persona_session`)

A **Persona Session** is the runtime envelope of an active persona. It carries:

- `persona_oid` — the persona this session belongs to
- `intent` — the natural-language description of what the persona is currently doing
- `consent_tokens` — fresh consent tokens collected during this session (for step-up credentials)
- `budget_state` — current spend, rate-limit counter, eval-sample state
- `parent_session_id` — for nested sessions inside an agent's child cells
- `started_at`, `cell_chain` — for provenance

The session is created by `anx_persona_act(p, intent, &session)` and ended by `anx_persona_session_end(session)`. Cells admitted while a session is open inherit the session ID. Cells admitted without an active session run under the system persona (`anx:persona/system`) and are tagged accordingly in provenance.

### 4.3 Custody Policy

Per-credential custody policy attached to a persona's credential binding:

```c
enum anx_persona_custody {
    ANX_CUSTODY_AUTO          = 0,  /* read without prompting */
    ANX_CUSTODY_AUDIT         = 1,  /* read + emit audit event */
    ANX_CUSTODY_STEP_UP       = 2,  /* requires fresh consent token */
    ANX_CUSTODY_HUMAN_APPROVE = 3,  /* requires explicit per-use human approval */
};

struct anx_persona_cred_binding {
    char                       cred_uri[ANX_CRED_URI_MAX];
    enum anx_persona_custody   policy;
    uint64_t                   per_use_cap;    /* e.g. max spend per call, 0 = unlimited */
    uint64_t                   per_day_cap;    /* daily spend cap */
    uint64_t                   spend_today;    /* running counter */
};
```

The kernel enforces the policy at the credential-read site: `anx_credential_read` consults the active persona session, looks up the binding, and applies the policy.

### 4.4 Governance Bundle

```c
struct anx_persona_governance {
    uint32_t  max_concurrent_cells;       /* concurrency cap */
    uint32_t  max_actions_per_hour;       /* rate cap */
    uint64_t  total_spend_cap;            /* lifetime $-value cap; refresh requires operator action */
    uint32_t  audit_retention_days;       /* provenance pinning duration */
    uint32_t  eval_sample_percent;        /* what fraction of actions are evaluated */
    bool      auto_suspend_on_breach;
    bool      auto_revoke_on_repeated_breach;
    uint32_t  repeated_breach_threshold;
};
```

### 4.5 Eval Attachment

```c
struct anx_persona_eval_attachment {
    anx_oid_t  workflow_oid;       /* RFC-0018 workflow that scores an action */
    float      threshold;          /* score below threshold = breach */
    uint32_t   sample_rate_ppm;    /* parts-per-million sampling */
    uint32_t   consecutive_breach_count;
};
```

Each persona may attach up to `ANX_PERSONA_MAX_EVALS` (initial value: 8) eval workflows. Score semantics are workflow-defined; a workflow that scores 0..1 with "lower = worse" sets `threshold` accordingly.

---

## 5. Object Type and Errno Additions

```c
/* kernel/include/anx/state_object.h */
ANX_OBJ_PERSONA = 15,	/* Persona Object (RFC-0026) */

/* kernel/include/anx/errno.h */
#define ANX_EPERSONA       -190 /* No active persona session */
#define ANX_EPERSONASUSP   -191 /* Persona is suspended */
#define ANX_EPERSONAREVK   -192 /* Persona is revoked */
#define ANX_EPERSONACUST   -193 /* Custody policy refuses access */
#define ANX_EPERSONABUDG   -194 /* Persona budget exhausted */
#define ANX_EPERSONAKIT    -195 /* Kit not installed on this persona */
#define ANX_EPERSONACAP    -196 /* Capability not granted to this persona */
```

---

## 6. State Machine

```
              +-----------+
              |  NASCENT  |
              +-----+-----+
                    |
                anx_persona_bind_identity()
                    v
              +-------------+
              | PROVISIONING|
              +-----+-------+
                    |
                anx_persona_finalize()
                    v
              +-----------+              eval breach   +-----------+
              |  ACTIVE   +----------------------------> SUSPENDED |
              +-----+-----+    anx_persona_suspend()   +-----+-----+
                    ^                                        |
                    |     anx_persona_resume()                |
                    +----------------------------------------+
                    |
              repeated breach OR anx_persona_revoke()
                    v
              +-----------+
              |  REVOKED  |   (terminal: credentials wiped, keys destroyed,
              +-----------+    audit log sealed, OID preserved for forensics)
```

State semantics:

| State        | Sessions Allowed? | Credential Reads? | Kit Leases? | Resumable? |
|--------------|-------------------|-------------------|-------------|------------|
| NASCENT      | No                | No                | No          | n/a        |
| PROVISIONING | No                | No                | No          | n/a        |
| ACTIVE       | Yes               | Per custody       | Per install | n/a        |
| SUSPENDED    | No                | No                | No          | Yes        |
| REVOKED      | No                | No                | No          | No         |

Revocation is irreversible by kernel policy. To recover a revoked persona's work, an operator may clone its audit log into a new persona but never reactivate the revoked OID.

---

## 7. Kernel API

```c
/* kernel/include/anx/persona.h */

int  anx_persona_init(void);

int  anx_persona_create(const char *name,
                        enum anx_persona_kind kind,
                        struct anx_persona **out);

int  anx_persona_bind_identity(struct anx_persona *p,
                               const anx_oid_t *keypair_cred_oid,
                               enum anx_persona_custody key_custody);

int  anx_persona_bind_credential(struct anx_persona *p,
                                 const char *cred_uri,
                                 const struct anx_persona_cred_binding *binding);

int  anx_persona_grant_capability(struct anx_persona *p,
                                  const anx_oid_t *cap_oid);

int  anx_persona_install_kit(struct anx_persona *p,
                             const char *kit_uri);

int  anx_persona_set_governance(struct anx_persona *p,
                                const struct anx_persona_governance *gov);

int  anx_persona_attach_eval(struct anx_persona *p,
                             const struct anx_persona_eval_attachment *att);

int  anx_persona_finalize(struct anx_persona *p);   /* NASCENT/PROVISIONING -> ACTIVE */

/* Acting */
int  anx_persona_act(struct anx_persona *p,
                     const char *intent,
                     struct anx_persona_session **session_out);
int  anx_persona_session_consent(struct anx_persona_session *session,
                                 const char *cred_uri,
                                 const uint8_t *signed_token, size_t token_len);
int  anx_persona_session_end(struct anx_persona_session *session);
struct anx_persona_session *anx_persona_session_current(void);

/* Lifecycle ops (require cap:persona-admin) */
int  anx_persona_suspend(const char *uri, const char *reason);
int  anx_persona_resume(const char *uri);
int  anx_persona_revoke(const char *uri, const char *reason);

/* Inspection */
int  anx_persona_list(const char **uris_out, uint32_t max, uint32_t *found_out);
int  anx_persona_status(const char *uri, struct anx_persona_status *out);
int  anx_persona_audit(const char *uri, uint64_t since_ts,
                       struct anx_provenance_query_result *out);

/* Internal helpers used by credential/kit/cap code paths */
int  anx_persona_check_credential(struct anx_persona_session *s,
                                  const char *cred_uri);
int  anx_persona_check_kit(struct anx_persona_session *s,
                           const char *kit_uri);
int  anx_persona_check_capability(struct anx_persona_session *s,
                                  const anx_oid_t *cap_oid);
int  anx_persona_record_action(struct anx_persona_session *s,
                               const struct anx_provenance_entry *entry);
```

### 7.1 Integration With Existing Subsystems

- **Credential reads.** `anx_credential_read` consults `anx_persona_session_current()`. If a session exists and the requested credential is bound to the persona, custody policy applies. If no session exists, the existing legacy auth-session policy applies (system persona).
- **Kit leases.** `anx_kit_lease_by_tag` and `anx_kit_lease` check `anx_persona_check_kit` against the active session.
- **Capability checks.** Existing capability-gated kernel ops (e.g. `anx_kit_enable` requiring `cap:kit-admin`) consult `anx_persona_check_capability`.
- **Cell admission.** `anx_cell_admit` inherits the persona session from the spawning cell.
- **Memory access.** `anx_memory_read/write` enforces the persona's memory scope binding.
- **EBM energy.** Each persona instance maintains a current energy estimate derived from its attached eval workflows' recent scores. `anx_ebm_score` consults this when the active session belongs to the persona.

### 7.2 Boot Order

`anx_persona_init()` is called immediately after `anx_kit_init()` and before `anx_jepa_init()`, since JEPA agent workflows themselves will run under personas in the migrated world.

A built-in `anx:persona/system` persona is registered at boot. Anything running without an explicit persona session (kernel internals, current legacy paths) executes as system. The system persona has all credentials available with `AUTO` custody, all kits installed, no eval attachments. It exists so that the migration to persona-aware code can be incremental — un-migrated code paths continue to work, attributed to system.

---

## 8. External Surfaces

### 8.1 ansh

```
ansh> persona list
URI                              KIND    STATE     KITS  EVALS
anx:persona/system               user    ACTIVE      *     0
anx:persona/adam                 user    ACTIVE     12     2
anx:persona/researcher           agent   ACTIVE      6     4
anx:persona/trader               agent   SUSPENDED   8     5

ansh> persona show anx:persona/researcher
uri:        anx:persona/researcher
kind:       agent
state:      ACTIVE
identity:   ed25519:0xa1b2...
credentials: 4 bound
  anthropic-api-key      AUTO         cap=$50/mo  spent=$12.41
  brave-search-api-key   AUTO         cap=$10/mo  spent=$1.07
  github-pat             STEP_UP      cap=—       spent=—
  paypal-test-account    HUMAN_APPROVE cap=$100   spent=$0
capabilities: 14 granted
kits installed:
  anx:kit/browser-native
  anx:kit/skill-web-research
  anx:kit/skill-knowledge-base
  anx:kit/python-gdpy
  ...
governance: 100 actions/hour, 3 concurrent cells, retention=90d
evals: 4 attached
  retrieval-faithfulness   threshold=0.80   1/100 sample   last=0.91
  answer-relevance         threshold=0.75   1/100 sample   last=0.88
  harmfulness              threshold=0.99   1/10  sample   last=1.00
  distribution-drift       threshold=0.50   1/1000 sample  last=0.62

ansh> persona switch anx:persona/researcher
[persona] session opened on anx:persona/researcher (intent: "shell session")

ansh[researcher]> ask "summarize the latest IBAL release notes"
[persona] action #142 logged
[persona] eval retrieval-faithfulness sampled: 0.93

ansh[researcher]> persona suspend anx:persona/trader "spend cap exceeded — investigating"
[persona] anx:persona/trader -> SUSPENDED
```

### 8.2 HTTP API

```
GET    /api/v1/personas                    — list personas with state
GET    /api/v1/personas/{uri-encoded}      — full persona detail
POST   /api/v1/personas                    — create (body: signed manifest)
POST   /api/v1/personas/{uri}/bind         — bind credential / install kit / grant cap
POST   /api/v1/personas/{uri}/finalize     — PROVISIONING -> ACTIVE
POST   /api/v1/personas/{uri}/act          — open a persona session (returns session token)
POST   /api/v1/personas/{uri}/consent      — present a fresh consent signature
POST   /api/v1/personas/{uri}/suspend      — suspend
POST   /api/v1/personas/{uri}/resume       — resume
POST   /api/v1/personas/{uri}/revoke       — revoke (irreversible)
GET    /api/v1/personas/{uri}/audit        — provenance query
```

All POSTs that mutate the persona require an Ed25519-signed body with a header from a session holding `cap:persona-admin`. Agent HTTP clients sign requests with the persona's identity key.

### 8.3 WebSocket Events

```json
{"type": "persona", "kind": "state_change", "uri": "...", "from": "ACTIVE", "to": "SUSPENDED", "reason": "..."}
{"type": "persona", "kind": "consent_required", "uri": "...", "cred_uri": "...", "challenge": "..."}
{"type": "persona", "kind": "budget_breach", "uri": "...", "cred_uri": "...", "spent": ..., "cap": ...}
{"type": "persona", "kind": "eval_breach", "uri": "...", "eval": "harmfulness", "score": 0.43, "threshold": 0.99}
{"type": "persona", "kind": "action", "uri": "...", "intent": "...", "provenance_oid": "..."}
```

---

## 9. Security and Custody

### 9.1 Identity Custody

Persona private keys are held as Credentials with custody policy. The default and recommended custody for a persona's identity key is `ANX_CUSTODY_AUTO` — the kernel needs the key to sign every action without prompting — but the key material itself is encrypted at rest using a kernel-derived sealing key tied to the persona's URI. An exfiltrated raw persona-credential blob is useless without the kernel that holds the sealing key. For higher-assurance setups, identity keys may be backed by a hardware token via the custody backend interface (specified by Anunix-Persona repo userland).

### 9.2 Step-Up Consent

A credential bound with `ANX_CUSTODY_STEP_UP` requires the persona session to present a *fresh consent token* before each read. The token is the persona's identity-key signature over a challenge consisting of (session ID, credential URI, intent, timestamp). The challenge is generated by the kernel at read time and must be signed within a tight window (default 30 seconds). This guarantees liveness: a stolen persona identity-key blob cannot satisfy step-up consent without the human-in-the-loop authorizing each use.

### 9.3 Human Approval

A credential bound with `ANX_CUSTODY_HUMAN_APPROVE` emits a `consent_required` event on the WS stream and *blocks* the credential read until a human-issued approval lands at `POST /api/v1/personas/{uri}/consent`. Approval is single-use. The WM persona indicator surfaces pending approvals.

### 9.4 Budget Enforcement

Per-credential `per_use_cap` and `per_day_cap` are checked at read time inside the persona session. Breach returns `ANX_EPERSONABUDG`. Total spend across credentials is tracked against `governance.total_spend_cap`.

### 9.5 Revocation

`anx_persona_revoke` performs:

1. State → REVOKED
2. All in-flight persona sessions terminated; child cells receive `ANX_EVENT_PERSONA_REVOKED`
3. Credential bindings cleared (the underlying credentials are not deleted globally — they may belong to other personas — but this persona can no longer read them)
4. Identity key destroyed (zeroized in the credential store; public key retained for audit signature verification)
5. Audit log sealed (no further provenance entries; existing entries pinned for retention)

Revocation is irreversible. To re-establish a similar persona, an operator creates a new persona and may transfer credentials, capabilities, and kits — but not the identity. The audit chain explicitly breaks.

### 9.6 Provenance

Every credential read, capability check, kit lease, cell admission, and memory access under a persona session appends to the persona's provenance trail. The trail is itself signed by the persona's identity key, producing an attested audit log that survives revocation.

---

## 10. Relationship to User Accounts

Anunix today (RFC-0008 §authentication) has multi-key user accounts: a username, a password hash, optional SSH pubkeys, per-key scopes. This RFC subsumes user accounts as the `kind=user` case of a persona:

- A user account becomes a `kind=user` persona whose identity is the user's primary SSH pubkey.
- Per-key scopes become granted capabilities on the persona.
- The password becomes a Credential bound to the persona with `ANX_CUSTODY_AUTO` (used only for `login`).
- The user's shell session opens a persona session by default; the user need not explicitly `persona switch`.

Phase 1 of the implementation keeps user accounts and personas as separate primitives with explicit migration tooling. Phase 2 unifies them: `useradd` is rewritten as `persona create --kind=user`. Backward compatibility is preserved for the existing shell commands.

This unification means that "who did what" answers the same way for human users and agent personas: every action has a persona OID in its provenance entry.

---

## 11. Migration Plan and Phasing

Persona is foundational like Kit, but its rollout interleaves with other migrations:

| Phase | Scope                                                              | Repo                    | RFC Task |
|-------|--------------------------------------------------------------------|-------------------------|----------|
| P1    | Persona kernel API + State Object kind + session plumbing + tests  | Anunix                  | Task 11  |
| P2    | Anunix-Persona repo scaffold (README, layout, build, examples)     | Anunix-Persona          | Task 12  |
| P3    | Identity + custody backends (software, hardware token, wallet)     | Anunix-Persona          | Task 13  |
| P4    | RAGAS-style eval framework, default eval suites                    | Anunix-Persona          | Task 14  |
| P5    | Default Persona Skill Kits (skill-web-research, etc.)              | Anunix-Persona          | Task 15  |
| P6    | Persona CLI + persona-aware ansh/HTTP/WM                           | Anunix + Anunix-Persona | Task 16  |
| P7    | User account migration to kind=user personas                       | Anunix                  | (later)  |

Phases P1 and P2 ship without breaking any existing path; the system persona absorbs all legacy traffic. P3–P5 build the userland incrementally. P6 makes personas visible to the user. P7 unifies user accounts and is the last step.

The Persona track runs in parallel with the JEPA/Browser/Python Kit migrations (RFC-0025 Phases 3–5). P1 depends on the Kit shell/REST/WS surface landing (so `kit install` semantics exist) but not on any specific consumer migration completing.

---

## 12. Open Questions

1. **Cross-persona delegation.** Can persona A grant persona B time-bounded use of one of A's credentials without copying it? Initial implementation: no — credentials are bound to a single persona at a time. Future RFC may add delegation tokens.

2. **Federated personas.** A persona that acts on multiple Anunix hosts. Requires distributed identity custody (threshold signing or shared HSM). Deferred.

3. **Eval workflow trust.** A misbehaving eval workflow can suspend a healthy persona. Initial mitigation: eval workflows themselves run under the operator's persona (the persona that attached them). Long-term: eval attestations.

4. **Persona-to-persona communication.** When agent persona A wants to invoke a tool exposed by agent persona B, what is the contract? Initial: B exposes a kit; A installs it; standard kit-lease path. May need a richer protocol (mailbox cells) later.

5. **Memory ownership on revocation.** When a persona is revoked, what happens to its memory? Default: memory is pinned for `audit_retention_days` then archived. Operator may explicitly seal+release to a successor persona via the audit log clone path.

6. **System persona scope.** System persona has implicit "all credentials" — this is operationally necessary during migration but risky long-term. A future RFC should shrink system persona's default credential set to "nothing not explicitly granted by an operator."

7. **Concurrency model.** A persona may have many cells active at once. The governance bundle's `max_concurrent_cells` enforces a cap, but eval sampling and budget tracking must be lock-free (or coarse-lock) to scale. Implementation detail — to be addressed in Task 11.

---

## 13. Why Now

The agent-first OS thesis is empty without agent-first accountability. An agent that can act has to be a *legible* actor — visible to the operator, governable by policy, auditable after the fact. Anunix has all the primitives needed to compose this; what is missing is the object that *names* the composition and gives it kernel-enforced identity.

The Kit Subsystem (RFC-0025) is the right time to introduce Personas: kits are how skills enter the OS, personas are who is allowed to use them. The two RFCs are siblings — one specifies the supply of capabilities, the other the demand. Building either without the other would leave a year of churn re-fitting; building them together gives Anunix a story for "who an agent is" that is as well-specified as the story for "what an agent runs."

---

## 14. References

- RFC-0002 — State Object Model
- RFC-0003 — Execution Cell Runtime (persona sessions ride on cell admission)
- RFC-0005 — Routing Plane and Engines
- RFC-0007 — Capability Objects (granted to personas)
- RFC-0008 — Credential Objects (bound to personas with custody policy)
- RFC-0009 — Agent Memory (scoped per persona)
- RFC-0013 — Tensor Objects (embeddings for eval workflows)
- RFC-0018 — Workflow Objects (eval pipelines)
- RFC-0020 — Iterative Belief-Action Loop (EBM energy consumes eval scores)
- RFC-0025 — Kit Subsystem (skills are kits installed on personas)
