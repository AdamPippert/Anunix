# RFC-0008: Credential Objects and Secrets Management

| Field      | Value                                                     |
|------------|-----------------------------------------------------------|
| RFC        | 0008                                                      |
| Title      | Credential Objects and Secrets Management                  |
| Author     | Adam Pippert                                              |
| Status     | Draft                                                     |
| Created    | 2026-04-15                                                |
| Updated    | 2026-04-15                                                |
| Depends On | RFC-0001, RFC-0002, RFC-0003, RFC-0005, RFC-0006, RFC-0007|

---

## Executive Summary

Anunix exists to be an AI-native operating system where intelligent agents operate autonomously — routing tasks to model services, executing multi-step workflows, and making decisions on behalf of users. Every one of these operations requires authentication. An agent that calls the Claude API needs an API key. An agent that retrieves documents from a private store needs credentials. An agent that commits to a remote repository needs a token. In a classical OS, these secrets are environment variables, plaintext files, or entries in a keyring that any process with the right UID can read. That model is fundamentally incompatible with an operating system where execution is delegated to autonomous cells with varying trust levels, where work crosses network boundaries between nodes in different trust zones, and where every operation is traced for provenance.

RFC-0008 introduces the **Credential Object**, a new State Object type purpose-built for secrets. A Credential Object stores sensitive material — API keys, tokens, certificates, passwords — under kernel-enforced invariants that no other object type provides: the payload is never serialized into traces or provenance logs, never exposed through the generic State Object read API, never transmitted across network trust boundaries without explicit policy, and only accessible to cells that have been granted a specific **credential binding** at admission time. The kernel mediates every access, enforces scope, and can revoke credentials in-flight.

This is not a bolt-on secrets manager. Credential Objects are first-class participants in the Anunix object model: they have OIDs, provenance records, access policies, and lifecycle states like any State Object. But they carry additional kernel-enforced constraints that make them fundamentally different from a byte blob that happens to contain a secret. The distinction is not metadata — it is enforcement. The kernel refuses to perform operations on Credential Objects that would be routine for ordinary State Objects, because the consequences of mishandling secrets are categorically different from the consequences of mishandling data.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert / public collaborators
**Depends on:** RFC-0001, RFC-0002, RFC-0003, RFC-0005, RFC-0006, RFC-0007
**Blocks:** —

---

## 2. Problem Statement

### 2.1 Secrets in an Agentic World

Classical operating systems treat secrets as an application concern. The kernel provides file permissions and process isolation; the application decides how to store, retrieve, and scope credentials. This works tolerably when a human operator manages credentials manually: they paste an API key into an `.env` file, set restrictive permissions, and trust that the application doesn't leak it.

In an agentic OS, this model collapses. Consider a task that requires an Execution Cell to call a remote model API, retrieve private documents from an authenticated store, and commit results to a version-controlled repository. Three different credentials, three different services, three different scoping requirements. The cell cannot simply inherit all credentials from the user — that violates the principle of least privilege. The cell should have exactly the credentials it needs for exactly the operations it performs, and no more.

Now consider that this cell might decompose its task into child cells, some of which may be routed to remote nodes (RFC-0006). Should a child cell running on a remote node receive the parent's API key? Obviously not — the remote node is in a different trust zone. But the child still needs to authenticate somehow. The system needs scoped delegation: the ability to derive a constrained credential from a broader one, with policy-controlled lifetime and scope.

### 2.2 What Existing RFCs Cannot Express

- **RFC-0002 (State Objects)** can store secret bytes as `ANX_OBJ_BYTE_DATA`, but provides no kernel enforcement against serialization, tracing, or unbounded access. A State Object containing an API key is indistinguishable from one containing a JPEG.

- **RFC-0003 (Execution Cells)** defines execution policies with `allow_network` and `allow_remote_models` flags, but has no mechanism for credential binding — a cell's execution policy says what it *may do*, not what secrets it *may access*.

- **RFC-0005 (Routing Plane)** can route to remote model engines, but has no way to attach authentication to a routed request. The route result selects an engine; it does not bind credentials for that engine.

- **RFC-0006 (Network Plane)** identifies credential scoping as a security requirement (Section 15.5) but does not specify how credentials are stored, bound, scoped, or delegated.

- **RFC-0007 (Capability Objects)** defines `input_cap_mask` for operational capabilities but not for credential requirements. A Capability can declare "I need summarization ability" but not "I need the Anthropic API key."

### 2.3 Why the Kernel Must Participate

Secrets management cannot be a userspace library because the invariants it must enforce cross subsystem boundaries:

1. **Provenance** must not log secret payloads, but provenance is a kernel subsystem.
2. **Routing** must attach credentials to remote requests, but routing is a kernel subsystem.
3. **Cell admission** must verify credential availability before committing resources, but admission is a kernel subsystem.
4. **Network serialization** must filter secrets from trust-boundary crossings, but serialization policy is enforced at the kernel level.
5. **Access control** must gate every read of a secret payload, but access control evaluation is a kernel function.

If any one of these subsystems is unaware that it is handling a secret, the secret leaks. The kernel is the only layer that can enforce cross-cutting invariants.

---

## 3. Goals

### 3.1 Primary Goals

1. **Secrets are State Objects with kernel-enforced invariants.** Credential Objects participate in the object model (OIDs, provenance, access policies) but carry additional restrictions that prevent accidental exposure.

2. **Payload opacity.** The raw bytes of a Credential Object are never serialized into provenance logs, execution traces, kprintf output, shell displays, or network messages. The kernel knows the OID, the name, and the access history — never the payload.

3. **Cell-scoped access.** A cell can only access a Credential Object if it was explicitly granted a **credential binding** at admission time. Bindings are name-scoped: a cell is admitted with "access to credential named `anthropic-api-key`," not "access to OID `abc...`."

4. **Least privilege by default.** No cell has access to any credential unless explicitly bound. The absence of a binding is denial. There is no ambient authority over secrets.

5. **Delegation with scoping.** A parent cell can delegate a subset of its credential bindings to child cells, with additional constraints: shorter lifetime, narrower scope (read-only, single-use), or restricted to specific engine classes.

6. **Revocation.** Credential bindings can be revoked while a cell is running. The next access attempt fails. This enables emergency credential rotation without killing in-flight work.

7. **Non-migratable by default.** Credential Objects do not cross network trust zone boundaries. Remote cells receive scoped proxy tokens (Section 10), not raw credentials.

8. **Audit completeness.** Every successful and failed access to a Credential Object is recorded in the provenance log. The record includes the accessor cell, the operation, the timestamp, and the result — but never the payload.

9. **Named addressing.** Credentials are addressed by name (e.g., `anthropic-api-key`) within a namespace, not by OID. This decouples cell admission policy from the specific OID of the current credential version, enabling rotation without policy changes.

10. **Provisioning flexibility.** Credentials can be provisioned from multiple sources: boot-time command line, interactive shell, network injection from a trusted peer, or programmatic creation by an authorized cell.

### 3.2 Non-Goals

- **Encryption at rest.** This RFC does not specify disk encryption or encrypted storage for credentials. Without a persistent storage layer, credentials exist only in memory. When persistent storage is added, encryption at rest becomes a separate concern.

- **Hardware security modules.** Integration with TPM, HSM, or secure enclaves is deferred. The abstractions in this RFC are compatible with future HSM backing but do not require it.

- **Certificate management.** X.509 certificate chains, CRL checking, and OCSP are out of scope. A TLS certificate can be stored as a Credential Object, but PKI lifecycle is a separate concern.

- **OAuth flows.** Interactive authentication protocols (OAuth 2.0, OIDC) require HTTP redirects and user interaction that are beyond the kernel's scope. The kernel stores the resulting tokens; the flow that obtains them is a userspace concern.

---

## 4. Core Definitions

### 4.1 Credential Object

A **Credential Object** is a State Object of type `ANX_OBJ_CREDENTIAL` with the following kernel-enforced properties:

- **Sealed on creation.** The payload is immutable from the moment the object enters the ACTIVE state. There is no write-after-create for credentials.
- **Opaque payload.** The payload is accessible only through `anx_credential_read()`, which enforces access policy. The generic `anx_so_read_payload()` returns `ANX_EPERM` for credential objects.
- **Non-traceable.** The kernel never includes credential payloads in provenance events, execution traces, kprintf calls, or serialized messages. Access is logged by OID only.
- **Named.** Every Credential Object has a unique name within its namespace (e.g., `anthropic-api-key`, `github-token`). Names are the stable identifier; OIDs change on rotation.

### 4.2 Credential Binding

A **Credential Binding** is an authorization granted to an Execution Cell at admission time, permitting it to access a named credential. Bindings specify:

- **Credential name:** The name of the credential this binding grants access to.
- **Scope:** The operations permitted — `READ` (retrieve the payload), `DELEGATE` (pass to child cells), or `INJECT` (automatically attach to outbound requests for a specific engine class).
- **Lifetime:** Optional expiration (absolute time or tick count). After expiration, the binding is invalid regardless of cell state.
- **Usage limit:** Optional maximum number of reads. After exhaustion, the binding is invalid.

### 4.3 Credential Namespace

The **Credential Namespace** is a flat, kernel-managed name→OID mapping. When a cell requests a credential by name, the kernel resolves the name to the current OID. This indirection enables credential rotation: updating the mapping changes which OID a name resolves to, without modifying any cell's admission policy.

### 4.4 Credential Proxy

A **Credential Proxy** is a short-lived, scoped token derived from a Credential Object for use across network trust boundaries (RFC-0006). Proxies are generated by the kernel when a cell with a credential binding is routed to a remote node. The proxy carries:

- A scoped token (not the raw secret)
- The name of the credential it represents
- An expiration time
- The set of permitted operations

The remote node's kernel validates the proxy against the originating node's trust level. Proxy generation is specified in Section 10.

---

## 5. Design Principles

### 5.1 Deny by Default

No cell, no subsystem, no shell command has access to any credential payload unless explicitly authorized through a binding. The kernel's default answer to "can I read this secret?" is no.

### 5.2 Secrets Are Not Data

A Credential Object is not a State Object that happens to contain a secret. It is a categorically different kind of object with different invariants. The kernel distinguishes them at the type level and enforces the distinction in every subsystem that handles objects: provenance, tracing, serialization, access control, and network transport.

### 5.3 Names Over OIDs

Credentials are addressed by name, not by OID. This is intentional. An API key is not a unique artifact — it is a role: "the key we use to call Claude." When the key rotates, the role persists. Cell policies bind to names ("this cell may access `anthropic-api-key`"), and the kernel resolves names to the current OID at access time.

### 5.4 Audit Everything, Expose Nothing

Every credential access — successful or failed — is audited. The audit record says who, when, what credential name, and whether the access was granted. It never says what the payload was. This is not configurable. The audit level for Credential Objects is always `ANX_AUDIT_ALL`, unconditionally.

### 5.5 Revocation Is Immediate

When a credential binding is revoked — whether by explicit command, expiration, or credential rotation — the next access attempt by the bound cell fails. There is no grace period, no cache, no stale read. The kernel holds the single copy of the payload and mediates every access.

---

## 6. Credential Object Schema

### 6.1 Object Structure

```c
struct anx_credential {
    /* State Object identity (inherited) */
    anx_oid_t oid;
    enum anx_object_state state;

    /* Credential identity */
    char name[128];               /* unique within namespace */
    enum anx_credential_type cred_type;

    /* Payload (kernel-private, never serialized) */
    void *secret;                 /* raw secret bytes */
    uint32_t secret_len;

    /* Metadata (safe to log and display) */
    char issuer[128];             /* who issued this credential */
    anx_time_t created_at;
    anx_time_t expires_at;        /* 0 = no expiration */
    anx_time_t last_accessed;
    uint32_t access_count;

    /* Governance */
    struct anx_access_policy access_policy;
    struct anx_prov_log *provenance;

    /* Kernel bookkeeping */
    struct anx_spinlock lock;
    struct anx_list_head store_link;
};
```

### 6.2 Credential Types

```c
enum anx_credential_type {
    ANX_CRED_API_KEY,         /* Bearer token / API key */
    ANX_CRED_TOKEN,           /* OAuth / session token */
    ANX_CRED_CERTIFICATE,     /* TLS client certificate (PEM) */
    ANX_CRED_PRIVATE_KEY,     /* Private key material */
    ANX_CRED_PASSWORD,        /* Username/password pair */
    ANX_CRED_PROXY,           /* Delegated proxy token (Section 10) */
    ANX_CRED_OPAQUE,          /* Untyped secret bytes */
};
```

### 6.3 Credential Binding Structure

```c
struct anx_credential_binding {
    char cred_name[128];          /* name of the credential */
    uint32_t scope;               /* bitmask: READ, DELEGATE, INJECT */
    anx_time_t expires_at;        /* 0 = no expiration */
    uint32_t max_reads;           /* 0 = unlimited */
    uint32_t read_count;          /* current count */
    enum anx_engine_class inject_class;  /* for INJECT scope: target engine class */
    bool revoked;
};

#define ANX_CRED_SCOPE_READ     (1 << 0)
#define ANX_CRED_SCOPE_DELEGATE (1 << 1)
#define ANX_CRED_SCOPE_INJECT   (1 << 2)
```

---

## 7. Lifecycle

### 7.1 State Transitions

```
                   ┌─────────┐
      create() ──▶ │ ACTIVE  │ ◀── (sealed on creation)
                   └────┬────┘
                        │
              ┌─────────┼─────────┐
              ▼                   ▼
        ┌──────────┐       ┌──────────┐
        │ ROTATED  │       │ REVOKED  │
        └──────────┘       └──────────┘
              │                   │
              ▼                   ▼
        ┌──────────┐       ┌──────────┐
        │ EXPIRED  │       │TOMBSTONE │
        └──────────┘       └──────────┘
```

- **ACTIVE:** Created and available. Bindings can be granted. Access is permitted for bound cells.
- **ROTATED:** A newer credential has taken this name. Existing bindings drain: currently-executing cells may complete, but new binding grants resolve to the replacement. After a configurable drain period, transitions to EXPIRED.
- **REVOKED:** Emergency revocation. All bindings are immediately invalid. No drain period. Provenance records the revoking actor and reason.
- **EXPIRED:** Terminal state. Payload is zeroed. Object retained for audit history.
- **TOMBSTONE:** Retained for provenance only. No payload, no bindings, no access.

### 7.2 Rotation Semantics

Credential rotation is a name-level operation: a new Credential Object is created with the same name, and the namespace mapping is updated atomically. The old credential transitions to ROTATED. Cells with active bindings to the name seamlessly read the new credential on their next access — no policy changes, no cell restarts.

---

## 8. Access Control

### 8.1 Credential-Specific Access Policy

Credential Objects use the existing `anx_access_policy` framework (RFC-0002 Section 6) with the following mandatory additions:

1. The audit level is always `ANX_AUDIT_ALL`. This is not configurable.
2. `ANX_ACCESS_READ_PAYLOAD` is rejected by `anx_access_evaluate()` for credential objects. All payload reads go through `anx_credential_read()`.
3. The default policy for a new credential is deny-all. The creator must explicitly grant access.

### 8.2 Binding Evaluation

When a cell calls `anx_credential_read()`:

1. The kernel resolves the credential name to the current OID via the namespace.
2. The kernel checks the cell's credential bindings for a matching name.
3. If no binding exists: `ANX_EPERM`. Logged as denied access.
4. If the binding is revoked, expired, or usage-exhausted: `ANX_EPERM`. Logged.
5. If the binding's scope does not include `READ`: `ANX_EPERM`. Logged.
6. The kernel evaluates the credential's access policy against the cell's identity.
7. If all checks pass: the payload is copied to the caller's buffer. Logged as successful access.
8. The binding's `read_count` is incremented.

---

## 9. Cell Integration

### 9.1 Credential Binding at Admission

Cell admission (RFC-0003 Section 10) is extended with a credential binding step:

```c
struct anx_cell_intent {
    /* existing fields ... */
    struct anx_credential_binding cred_bindings[ANX_MAX_CRED_BINDINGS];
    uint32_t cred_binding_count;
};
```

During admission, the kernel verifies:
- Each named credential exists in the namespace.
- The admitting cell (or the system) has authority to grant the binding.
- The binding scope does not exceed the parent cell's scope (no privilege escalation).

If any credential is unavailable or the binding is unauthorized, admission fails with `ANX_EPERM`.

### 9.2 Automatic Injection

When a credential binding has `ANX_CRED_SCOPE_INJECT` set and specifies an engine class, the routing plane automatically attaches the credential to requests routed to engines of that class. The cell does not need to explicitly read the credential — the kernel injects it into the outbound request header.

This is the primary mechanism for API authentication: a cell is admitted with a binding that says "inject `anthropic-api-key` into requests to `ANX_ENGINE_REMOTE_MODEL` engines." The routing plane handles the rest.

### 9.3 Delegation to Child Cells

When a parent cell creates a child cell, it may delegate credential bindings. The delegation rules:

1. A parent can only delegate bindings it holds with `ANX_CRED_SCOPE_DELEGATE`.
2. The child's binding scope cannot exceed the parent's scope.
3. The child's lifetime cannot exceed the parent's binding lifetime.
4. The child's usage limit cannot exceed the parent's remaining usage.

This ensures that delegation never grants more access than the delegating cell holds.

---

## 10. Network Boundary Enforcement

### 10.1 The Non-Migration Rule

Credential Objects do not cross network trust zone boundaries. When the routing plane selects a remote engine for a cell's task, and the cell holds credential bindings needed for that engine, the kernel does **not** send the raw credential to the remote node.

### 10.2 Proxy Generation

Instead, the kernel generates a **Credential Proxy** — a short-lived, scoped token — and sends the proxy to the remote node. The proxy contains:

1. A cryptographic proof that the originating kernel authorized this access.
2. The credential name (not the payload).
3. The scope: what the remote cell may do with the proxied credential.
4. An expiration time, bounded by the originating binding's lifetime.

The remote kernel validates the proxy against the originating node's trust zone (RFC-0006 Section 7). If the trust level is insufficient, the proxy is rejected.

### 10.3 Proxy Fulfillment

The originating kernel acts as a credential proxy server. When the remote node needs to use the proxied credential (e.g., to make an API call), it sends a proxy fulfillment request back to the originating node. The originating kernel reads the credential, attaches it to the outbound API call, and returns the result. The raw credential never leaves the originating node.

This is the **credential proxy pattern**: the originating node performs the authenticated request on behalf of the remote cell, using its local credential, and returns only the result.

---

## 11. Provisioning

### 11.1 Boot-Time Provisioning

Credentials can be provisioned at boot via the GRUB command line:

```
multiboot /boot/anunix-mb1.elf -- cred:anthropic-api-key=sk-ant-xxx
```

The kernel parses the command line during early init, creates Credential Objects for each `cred:` entry, and zeroes the command line memory after parsing.

### 11.2 Shell Provisioning

Interactive provisioning via the kernel monitor:

```
anx> secret set anthropic-api-key sk-ant-api03-...
secret: anthropic-api-key stored (36 bytes)
anx> secret list
  anthropic-api-key  api_key  36 bytes  0 accesses
anx> secret show anthropic-api-key
  name:     anthropic-api-key
  type:     api_key
  size:     36 bytes
  created:  2026-04-15T10:30:00Z
  accesses: 0
  (payload: [REDACTED])
```

The `secret set` command reads the value, creates a Credential Object, and discards the input from the shell's line buffer. The shell never echoes the value.

### 11.3 Programmatic Provisioning

An authorized cell can create credentials programmatically:

```c
int anx_credential_create(const char *name,
                           enum anx_credential_type cred_type,
                           const void *secret, uint32_t secret_len,
                           struct anx_credential **out);
```

The creating cell must hold the `ANX_CAP_CREDENTIAL_ADMIN` capability (see Section 13).

### 11.4 Network Provisioning

A trusted peer node (RFC-0006, trust zone LOCAL or LAN) can inject credentials into the local node's credential store over an authenticated channel. This enables centralized credential distribution in multi-node deployments.

---

## 12. Kernel API

### 12.1 Credential Store

```c
/* Initialize the credential store */
void anx_credstore_init(void);

/* Create a new credential (sealed immediately) */
int anx_credential_create(const char *name,
                           enum anx_credential_type cred_type,
                           const void *secret, uint32_t secret_len,
                           struct anx_credential **out);

/* Read the credential payload (enforces binding + access policy) */
int anx_credential_read(const char *name,
                         const anx_cid_t *cell,
                         void *buf, uint32_t buf_len,
                         uint32_t *actual_len);

/* Resolve a credential name to its current OID */
int anx_credential_lookup(const char *name, anx_oid_t *oid_out);

/* Rotate a credential (create new, transition old to ROTATED) */
int anx_credential_rotate(const char *name,
                           const void *new_secret, uint32_t new_len);

/* Revoke a credential immediately */
int anx_credential_revoke(const char *name);

/* List credential names (without payloads) */
int anx_credential_list(char names[][128], uint32_t max_names,
                         uint32_t *count);
```

### 12.2 Credential Binding

```c
/* Grant a credential binding to a cell */
int anx_credential_bind(const anx_cid_t *cell,
                         const struct anx_credential_binding *binding);

/* Revoke a credential binding */
int anx_credential_unbind(const anx_cid_t *cell,
                           const char *cred_name);

/* Check if a cell holds a valid binding for a credential */
int anx_credential_check_binding(const anx_cid_t *cell,
                                  const char *cred_name,
                                  uint32_t required_scope);
```

---

## 13. Amendments to Existing RFCs

### 13.1 RFC-0002: State Object Model

1. Add `ANX_OBJ_CREDENTIAL` to `enum anx_object_type`.
2. `anx_so_read_payload()` returns `ANX_EPERM` for objects of type `ANX_OBJ_CREDENTIAL`. All payload access goes through `anx_credential_read()`.
3. Provenance events for credential objects never include payload data, regardless of event type. The `ANX_PROV_ACCESSED` event records the credential name and accessor cell but not the payload bytes.

### 13.2 RFC-0003: Execution Cell Runtime

1. Add `struct anx_credential_binding cred_bindings[]` and `uint32_t cred_binding_count` to `struct anx_cell_intent`.
2. Cell admission (Section 10) verifies credential availability and binding authority before committing resources.
3. Cell termination releases all credential bindings held by the cell.
4. Add `ANX_CELL_CREDENTIAL_UNAVAILABLE` as an admission failure mode.

### 13.3 RFC-0005: Routing Plane and Unified Scheduler

1. When a route result selects an engine that requires authentication, the routing plane checks the cell's credential bindings for an `INJECT`-scoped binding matching the engine class.
2. If a credential binding with `INJECT` scope exists, the scheduler injects the credential into the request automatically.
3. If no matching binding exists, the route is marked as infeasible with reason `credential_missing`.

### 13.4 RFC-0006: Network Plane and Federated Execution

1. Credential Objects are added to the non-migratable object list (objects that must not be replicated across trust zone boundaries).
2. Remote execution requests that require credentials use the proxy pattern (Section 10). The originating node's kernel acts as the credential proxy.
3. Trust envelope construction (Section 9.3) must verify that no credential payloads are included in the serialized envelope.

### 13.5 RFC-0007: Capability Objects

1. Capabilities may declare required credentials in their payload schema:
   ```json
   "required_credentials": [
     { "name": "anthropic-api-key", "scope": "inject" }
   ]
   ```
2. Capability installation verifies that the required credentials exist in the local credential store.
3. When a capability is invoked, the routing plane creates credential bindings for the cell from the capability's declared requirements.

---

## 14. Shell Interface

```
secret set <name> <value>          Create a credential (API key type)
secret set-type <name> <type> <value>  Create with explicit type
secret list                        List credential names (no values)
secret show <name>                 Show metadata (never payload)
secret rotate <name> <new-value>   Rotate to a new value
secret revoke <name>               Revoke immediately
secret delete <name>               Delete (transitions to tombstone)
```

The `secret set` command does not echo the value to the console. The value is consumed from the command line buffer and zeroed after the Credential Object is created.

---

## 15. POSIX Compatibility

In the POSIX compatibility layer (RFC-0002 Section 17), credentials appear as files under a virtual `/secrets/` namespace:

```
/secrets/anthropic-api-key    → read returns payload (if binding exists)
/secrets/.list                → read returns credential names
```

The POSIX `read()` call on a credential file checks the calling cell's bindings. If no binding exists, `read()` returns `EPERM`. This enables legacy tools to consume credentials through the file interface while maintaining kernel-enforced access control.

---

## 16. Security Considerations

### 16.1 Memory Handling

Credential payloads are allocated from a dedicated memory pool that is:
- Not included in core dumps or memory snapshots.
- Zeroed on free (not just returned to the allocator).
- Not swappable to disk (when virtual memory is implemented).

### 16.2 Side Channel Resistance

Credential comparison operations (e.g., proxy validation) use constant-time comparison to resist timing attacks. This is a defense-in-depth measure; the primary protection is that credentials are never exposed to comparison by untrusted code.

### 16.3 Shell Echo Suppression

The `secret set` command suppresses echo of the value to the console output and framebuffer. The value is consumed from the internal line buffer, used to create the Credential Object, and zeroed. The shell's command history does not retain `secret set` commands.

### 16.4 Threat Model

The primary threats this design addresses:
1. **Credential leakage via traces/logs.** Mitigated by payload opacity — the kernel never serializes payloads.
2. **Over-privileged cells.** Mitigated by binding-at-admission — cells only access named credentials they're authorized for.
3. **Credential exfiltration via network.** Mitigated by non-migration rule and proxy pattern.
4. **Credential persistence after rotation.** Mitigated by immediate revocation and memory zeroing.
5. **Privilege escalation via delegation.** Mitigated by scope narrowing — children cannot exceed parents.

---

## 17. Reference Prototype Architecture

### Phase 1 — Credential Store and Shell (Immediate)

Implement the kernel credential store, shell commands (`secret set/list/show`), and the `anx_credential_read()` API. No cell binding enforcement yet — credentials are accessible by name from the shell and from `kernel_main`. This gets API key storage working for the HTTP client.

### Phase 2 — Cell Binding Enforcement

Implement credential bindings in `struct anx_cell_intent`, admission-time verification, and the `anx_credential_bind/unbind/check` APIs. After this phase, cells must be explicitly authorized to access credentials.

### Phase 3 — Routing Integration

Implement automatic injection: the routing plane reads credentials and attaches them to outbound HTTP requests when a cell has an `INJECT`-scoped binding for the target engine class.

### Phase 4 — Network Proxy

Implement the credential proxy pattern for remote execution. This requires the Network Plane transport layer to be operational.

### Phase 5 — Rotation and Lifecycle

Implement credential rotation, revocation, expiration, and the full lifecycle state machine.
