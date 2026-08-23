# RFC-0026: Kit Subsystem — Unified Loadable Subsystems

| Field      | Value                                                                              |
|------------|------------------------------------------------------------------------------------|
| RFC        | 0026                                                                               |
| Title      | Kit Subsystem — Unified Loadable Subsystems                                        |
| Author     | Adam Pippert                                                                       |
| Status     | Draft                                                                              |
| Created    | 2026-05-12                                                                         |
| Updated    | 2026-05-12                                                                         |
| Depends On | RFC-0002, RFC-0003, RFC-0005, RFC-0007, RFC-0008, RFC-0013, RFC-0018, RFC-0020     |

---

## Executive Summary

Three of Anunix's most important agent-facing capabilities — the JEPA world model, the browser, and the Python runtime — each reach the running kernel through a different mechanism. JEPA weights are compiled into the kernel as a static C header. The native browser engine is started unconditionally from `main.c`; HTTPS depends on a host-side Python proxy that the user must launch by hand; the Chromium peer is a third Python daemon invoked through a one-off shell command. GDPy, the Python runtime, is not loaded at all — it builds as a host binary in a sibling repository with no bridge into the OS. The three paths share nothing.

This is not a bug in any one subsystem. It is the absence of an OS primitive. Anunix has no concept of a *loadable subsystem* — a content-addressed, signed, manifest-driven bundle that the kernel can register, enable, drain, update, and unload through a single uniform API. Without that primitive, every new agent-facing capability invents its own loading path and its own failure mode, and the union of those paths becomes the user experience.

RFC-0026 introduces the **Kit** — a new `ANX_OBJ_KIT` State Object kind, an `anx_kit_*` kernel API, an `ansh kit` shell surface, an `/api/v1/kits` HTTP surface, and a kit-event channel on the existing WebSocket stream. A Kit carries a manifest (uri, version, what it provides, what it requires, host-side prerequisites, lifecycle hooks, signature) and a payload (one or more State Object OIDs holding the actual weights, binary, or driver). The same API loads a 200-byte boot-baked kit and a 7-gigabyte runtime-downloaded model checkpoint. The same lifecycle drains a JEPA world during hot-swap and a browser engine during a peer-daemon failover. The same prereq channel surfaces a missing host-side service whether the kit is anxbproxy, anxbrowserd, or a Brain Floating Point quantizer that hasn't been written yet.

The result is that JEPA, the browser, and Python — and everything that comes after them — load through one path. The UX work that follows (a WM Capability Tray, hot-swap UI, in-WM browser window) becomes possible because the underlying state is finally legible.

Kits are explicitly **not** Capability Objects (RFC-0007). Capability Objects are unforgeable access tokens that gate *who* can use a State Object. Kits are loadable subsystems that determine *what* the OS can do. A kit-enable operation typically *requires* a capability (`cap:kit-admin`), and a kit typically *publishes* engines whose use is gated by capabilities — but the two concepts are orthogonal. The "Kit" name is deliberately chosen to keep this distinction visible in the codebase.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert
**Depends on:** RFC-0002 (State Objects), RFC-0003 (Execution Cells), RFC-0005 (Routing + Engines), RFC-0007 (Capability Objects), RFC-0008 (Credentials), RFC-0013 (Tensor Objects), RFC-0018 (Workflows), RFC-0020 (IBAL)
**Blocks:** Future RFCs covering JEPA-on-Kits migration, Browser-on-Kits migration, GDPy integration, Capability Tray UX.

---

## 2. Problem Statement

### 2.1 Three Loading Paths, Three Failure Modes

The status quo, as of the 2026.4.24 release:

**World model (JEPA).** `Anunix-World/training/train_jepa.py` writes static `float` arrays into `standalone/include/anx/jepa_weights.h`. The kernel pulls these in at compile time through `pal_jepa.c`. At boot, `anx_jepa_init()` (called unconditionally from `kernel/core/main.c`) probes tensor capability and picks one of three built-in world profiles by hardware heuristic. Updating the weights means rebuilding the kernel. Loading a different checkpoint at runtime is technically possible through `anx_jepa_world_activate(uri, oid)` but there is no shell command, no REST endpoint, no UI affordance, and no provenance chain.

**Browser.** Two engines coexist with incompatible ergonomics. The native engine in `kernel/drivers/browser/` is started unconditionally at boot via `anx_browser_init(9191)`. HTTPS requires a separately-launched host-side `tools/anxbproxy.py` CONNECT proxy at `10.0.2.2:8118`. The Browser Renderer Cell is a TCP client that has to be opted into manually with `browser_init [host] [port]` and connects out to a host `anxbrowserd` Playwright/Chromium daemon. Each of these has a different start path, a different failure surface, and a different documentation page.

**Python runtime (GDPy).** Not loaded at all. `Anunix-Python` builds a standalone `gdpy` host binary. There is no `python` ansh command, no celf binary in `userland/`, no engine registration, no reference from `kernel/core/main.c`. The runtime exists but is invisible to the OS.

### 2.2 The Cost of Three Paths

The cost is not in any single line of code. The cost is structural:

- **Failure modes proliferate.** A user whose `anxbproxy` is not running hits an opaque TLS error in the browser. A user on a CPU-only machine gets `JEPA_DEGRADED` with no remediation hint. A user who wants Python gets nothing at all. Each path invented its own failure surface, so each path needs its own UX work.
- **There is no place to put new capabilities.** A future `anxml` inference runtime (RFC-0021), an XDNA NPU firmware blob, a downloaded LoRA, a fine-tuned safety classifier, a host-side OCR service — every one of these will need a fourth, fifth, sixth loading path.
- **Provenance is non-uniform.** Weights baked at compile time have no provenance chain. Peer-daemon connections have no provenance at all. RFC-0002 requires every meaningful state transition to be addressable; today's loading paths sidestep the requirement.
- **Hot-update is impossible.** Swapping JEPA weights or upgrading the browser engine requires a kernel rebuild and reboot. Anunix is supposed to be an agent-first OS; an agent that cannot update its own substrate at runtime is not agent-first.
- **The UX surface cannot consolidate.** A WM tray that surfaces "what is loaded, what is missing" cannot exist over three different loading models. It can only exist over one.

### 2.3 Why a New Primitive

The temptation is to fix this piecemeal: a `models/` directory for weights, a `services/` directory for daemons, a `prereqs.sh` for host setup. That approach scales the problem rather than solving it. Anunix already has the right machinery for content-addressing (State Objects), signing (Credentials), engine registration (Routing Plane), and lifecycle control (Engine states). The missing piece is a single object that *binds* those pieces together: a manifest that says "this OID is weights, this OID is code, this is what it provides, this is what it requires, this is how to enable it, this is how to drain it." That object is the Kit.

---

## 3. Goals

### 3.1 Primary Goals

1. **One loading path.** Every weights blob, engine binary, driver, theme, dataset, and host-side prerequisite that the OS depends on loads through `anx_kit_enable(uri)`. No subsystem initializes its own payload.
2. **Content-addressed, signed, provenance-tracked.** Every Kit is a `ANX_OBJ_KIT` State Object. Payload OIDs reference other State Objects. Manifests are Ed25519-signed against a Credential-stored trust root. Every state transition appears in the provenance chain.
3. **Uniform lifecycle.** REGISTERED → ENABLING → ENABLED → DRAINING → DISABLED, with PREREQ_FAILED and DEGRADED sub-states. Hot-update is a first-class operation: new kit is enabled alongside old, new leases route to new, old drains.
4. **Boot kits and runtime kits are the same.** Kits compiled into the boot image (today's JEPA static weights, today's in-kernel browser engine) load through the same lifecycle as runtime-downloaded kits. The kernel calls `anx_kit_register_builtin()` early in boot; everything after that is uniform.
5. **Host-side prerequisites are first-class.** A kit can declare `host-service:<endpoint>` or `host-binary:<name>` prereqs. The kit subsystem probes them, surfaces failures as remediation events, and (with user consent) drives a host-side helper to install them.
6. **Uniform external surface.** `ansh kit`, `/api/v1/kits`, and a kit-state WS event channel. Anything one surface can do, the other two can do.

### 3.2 Non-Goals

- **Package management.** Kits are not a substitute for distribution-level packaging. A Kit's payload may have come from anywhere; the Kit subsystem is content-addressed and signature-checking but does not maintain a repository graph, resolve transitive dependencies via SAT, or fetch from a network mirror. A future RFC may layer a registry on top.
- **Sandboxing the kit payload.** A Kit's `enable` hook runs in kernel context (or, for celf binaries, in the cell runtime under the kit's declared capability set). Kits are trusted code; signature verification is the gate. RFC-0007 capabilities gate *use* of kit-provided engines, not the enable hook itself.
- **Replacing the Engine model.** Engines (RFC-0005) remain the runtime abstraction for "a thing that can do work." Kits are how engines (and other payloads) get *into* the OS. A kit typically calls `anx_engine_register()` in its enable hook.
- **Cross-architecture binary translation.** A celf-payload kit is tagged with target architecture; mismatched kits refuse to enable.

---

## 4. Core Definitions

### 4.1 Kit Object (`ANX_OBJ_KIT`)

A **Kit Object** is a State Object of type `ANX_OBJ_KIT`. Its content is a canonical-encoded manifest (Section 6) and a set of payload OID references. The Kit Object is small (typically <4 KiB); the heavy payload (tensors, binaries, firmware) lives in separately-OID-addressed State Objects that the manifest references.

Kit Objects participate in the full State Object lifecycle (NASCENT → ACTIVE → SEALED → ARCHIVED/DELETED). A SEALED kit is immutable; updating a kit produces a new Kit Object at a new OID, and the registry's URI→OID pointer is moved atomically.

### 4.2 Kit URI

Each kit has a stable **URI** of the form `anx:kit/<name>`, e.g. `anx:kit/jepa-os-default` or `anx:kit/browser-https-proxy`. The URI is the user-facing handle; the OID changes on every update, the URI does not. The registry maintains a URI→OID map.

### 4.3 Kit Registry

The **Kit Registry** is the kernel-resident map from URI to current kit state (OID, lifecycle state, lease count, last health probe). The registry is held in `kernel/core/kit/` and persisted via the PAL persistence layer alongside the credential store and user object store.

### 4.4 Provider / Consumer

A kit **provides** named capabilities (string tags like `jepa-world`, `browser-engine`, `python-runtime`). A consumer subsystem queries the registry for kits providing a tag, leases the kit, and uses whatever the kit's enable hook published (usually an engine handle). Capability tags here are simple strings, not RFC-0007 Capability Objects.

### 4.5 Prereq

A **Prereq** is a structured declaration in a kit manifest of something external the kit needs in order to function. The kit subsystem evaluates prereqs at enable time and on demand via health probes; failure transitions the kit to `PREREQ_FAILED` and emits a remediation event.

---

## 5. Object Type Addition

This RFC adds `ANX_OBJ_KIT` to the State Object type enum (RFC-0002 Section 5):

```c
/* kernel/include/anx/state_object.h */
ANX_OBJ_KIT = 14,	/* Kit Object (RFC-0026) */
```

A new error code is added to `kernel/include/anx/errno.h`:

```c
#define ANX_EKITNOSIG    -180   /* Kit manifest signature invalid */
#define ANX_EKITPREREQ   -181   /* Kit prerequisite unmet */
#define ANX_EKITDEPS     -182   /* Kit dependency unsatisfied */
#define ANX_EKITARCH     -183   /* Kit target architecture mismatch */
#define ANX_EKITSTATE    -184   /* Kit not in a state that permits this op */
```

---

## 6. Manifest Schema

The manifest is the heart of the Kit. It is encoded canonically (sorted keys, no extra whitespace) so signatures over its bytes are deterministic. The on-disk representation is a TLV-style binary container; the canonical JSON form below is used in shell output, REST responses, and documentation.

```json
{
  "uri":        "anx:kit/jepa-os-default",
  "version":    "1.2.0",
  "kind":       "model-weights",
  "arch":       "any",
  "provides":   ["jepa-world"],
  "requires":   ["tensor-engine"],
  "prereqs":    [],
  "payload": {
    "encoder_weights":   "oid:0xabc...",
    "predictor_weights": "oid:0xdef...",
    "world_profile":     "oid:0x123..."
  },
  "hooks": {
    "enable":  "builtin:anx_jepa_kit_enable",
    "disable": "builtin:anx_jepa_kit_disable",
    "health":  "builtin:anx_jepa_kit_health"
  },
  "trust_root": "cred:anx-kit-root-2026",
  "signature":  "ed25519:0x..."
}
```

### 6.1 Required Fields

- **uri** — stable kit identifier. URIs in the `anx:kit/` namespace.
- **version** — semantic version. Used for hot-update ordering.
- **kind** — one of:
  - `model-weights` — tensor payload consumed by an existing engine
  - `engine-binary` — a celf executable that registers an engine on enable
  - `driver` — kernel-internal driver (probed against hardware)
  - `host-prereq` — wraps a host-side service, no kernel payload
  - `theme` — UI assets (RFC-0019 themes become kits in Phase 6)
  - `dataset` — read-only data referenced by other kits or cells
  - `composite` — meta-kit that references and orchestrates other kits
- **arch** — one of `any`, `x86_64`, `arm64`. Mismatches refuse to enable.
- **provides** — array of capability tag strings.
- **requires** — array of capability tag strings. The kit subsystem will not enable a kit whose requirements are unsatisfied.
- **payload** — map of role → State Object OID reference. Roles are kit-kind-specific (encoder_weights for JEPA, image for celf, etc.).
- **hooks** — map of `enable | disable | health | upgrade` → handler reference. Handler references are either `builtin:<symbol>` (kernel function pointer registered via `anx_kit_register_builtin_hook`) or `celf:<oid>` (entry point in a celf payload).
- **trust_root** — credential reference identifying the public key the signature must verify against.
- **signature** — Ed25519 signature over the canonical encoding of all preceding fields.

### 6.2 Optional Fields

- **prereqs** — array of prereq objects:
  ```json
  {"kind": "host-service", "endpoint": "tcp:10.0.2.2:8118", "probe": "tcp-connect", "remediation": "..."}
  {"kind": "host-binary", "name": "playwright", "probe": "which", "remediation": "..."}
  {"kind": "kernel-feature", "feature": "ANX_CAP_TENSOR_GPU", "probe": "engine-find"}
  ```
- **lease_drain_timeout_ms** — soft cap on how long DRAINING is allowed to wait for outstanding leases before forcing disable. Default 30000.
- **min_resources** — `{ram_bytes, vram_bytes, cpu_count}` floor below which enable is refused.
- **provenance_chain** — RFC-0002 standard provenance.

---

## 7. Lifecycle State Machine

```
                +-----------------+
                |  UNREGISTERED   |
                +--------+--------+
                         |
                anx_kit_register()
                         v
                +-----------------+      anx_kit_unregister()
                |   REGISTERED    +-----> UNREGISTERED
                +--------+--------+
                         |
                anx_kit_enable()
                         v
                +-----------------+      prereq fail
                |    ENABLING     +-----> PREREQ_FAILED
                +--------+--------+              |
                         |                       | anx_kit_health() recovers
                hook returns OK                  v
                         v                +-----------------+
                +-----------------+       |  PREREQ_FAILED  |
                |     ENABLED     +<------+-----------------+
                +--------+--------+
                         |
              hot-probe finds degraded
                         v
                +-----------------+
                |    DEGRADED     |
                +--------+--------+
                         |
                anx_kit_disable()
                         v
                +-----------------+
                |    DRAINING     |
                +--------+--------+
                  leases==0
                         v
                +-----------------+
                |    DISABLED     |
                +-----------------+
```

State semantics:

| State          | Hooks Permitted               | Engines Visible? | Leases Accepted? |
|----------------|-------------------------------|------------------|------------------|
| UNREGISTERED   | —                             | No               | No               |
| REGISTERED     | health-probe                  | No               | No               |
| ENABLING       | enable                        | No               | No               |
| ENABLED        | health, upgrade               | Yes              | Yes              |
| DEGRADED       | health, upgrade               | Yes (warned)     | Yes              |
| PREREQ_FAILED  | health (re-probe)             | No               | No               |
| DRAINING       | disable                       | Yes              | No (new)         |
| DISABLED       | enable (re-enter ENABLING)    | No               | No               |

DRAINING is bounded by `lease_drain_timeout_ms`. On timeout, outstanding leases are forcibly broken (the consumer receives an engine-revoked event) and the kit moves to DISABLED.

### 7.1 Hot-Update

`anx_kit_update(uri, new_oid)` is the atomic upgrade primitive:

1. Validate new manifest signature and dependency closure.
2. Run the new kit's enable hook to ENABLING → ENABLED. New engines register alongside the old.
3. Update the URI → OID pointer atomically.
4. Mark the *old* kit instance DRAINING. New routing decisions select the new engine via the existing Routing Plane preferences (higher version wins, ties broken by registration time).
5. When the old kit's lease count reaches zero, run its disable hook and transition to DISABLED, then UNREGISTERED.

A JEPA rollout in flight on the old weights completes on the old weights. A new rollout admitted after the update lands on the new weights. The consumer never sees a torn read.

---

## 8. Kernel API

```c
/* kernel/include/anx/kit.h */

/* Lifecycle */
int  anx_kit_init(void);
int  anx_kit_register(const anx_oid_t *manifest_oid, struct anx_kit **out);
int  anx_kit_register_builtin(const struct anx_kit_builtin_decl *decl,
                              struct anx_kit **out);
int  anx_kit_unregister(struct anx_kit *kit);

int  anx_kit_enable(const char *uri);
int  anx_kit_disable(const char *uri);
int  anx_kit_update(const char *uri, const anx_oid_t *new_manifest_oid);

/* Inspection */
enum anx_kit_state anx_kit_state_get(const char *uri);
int  anx_kit_status(const char *uri, struct anx_kit_status *out);
int  anx_kit_list(const char **uris_out, uint32_t max, uint32_t *found_out);
int  anx_kit_lookup(const char *uri, struct anx_kit **out);

/* Health and prereqs */
int  anx_kit_health(const char *uri, struct anx_kit_health_report *out);
int  anx_kit_prereq_check(const char *uri);

/* Consumer-side: lease a kit by capability tag */
int  anx_kit_lease_by_tag(const char *cap_tag, struct anx_kit_lease **out);
void anx_kit_lease_release(struct anx_kit_lease *lease);

/* Hook registration (builtin handlers) */
typedef int (*anx_kit_enable_fn)(struct anx_kit *kit);
typedef int (*anx_kit_disable_fn)(struct anx_kit *kit);
typedef int (*anx_kit_health_fn)(struct anx_kit *kit,
                                 struct anx_kit_health_report *out);
int  anx_kit_register_builtin_hook(const char *symbol,
                                   anx_kit_enable_fn enable,
                                   anx_kit_disable_fn disable,
                                   anx_kit_health_fn health);
```

`struct anx_kit_status` exposes: state, version, provides[], requires[], lease_count, last_health_ts, prereq_failures[].

### 8.1 Boot Order

`anx_kit_init()` is invoked from `kernel/core/main.c` immediately after `anx_pal_persist_load()` and *before* any current `anx_*_init()` call that loads a payload (JEPA, browser, audio, theme). Subsystems that today initialize their own payloads will, post-migration, register their boot kits via `anx_kit_register_builtin()` and call `anx_kit_enable()` to bring themselves online.

---

## 9. External Surfaces

### 9.1 ansh

```
ansh> kit list
URI                                STATE     VERSION  PROVIDES
anx:kit/jepa-os-default            ENABLED   1.2.0    jepa-world
anx:kit/browser-native             ENABLED   2026.4.24  browser-engine
anx:kit/browser-https-proxy        PREREQ_FAILED 1.0.0  https-proxy
anx:kit/python-gdpy                DISABLED  0.3.0    python-runtime

ansh> kit info anx:kit/browser-https-proxy
uri:        anx:kit/browser-https-proxy
state:      PREREQ_FAILED
version:    1.0.0
provides:   [https-proxy]
requires:   []
prereqs:    1 unmet:
  host-service tcp:10.0.2.2:8118 — connection refused
  remediation: python3 tools/anxbproxy.py &

ansh> kit enable anx:kit/python-gdpy
[kit] enabling anx:kit/python-gdpy ...
[kit] engine anx-gdpy registered (ANX_ENGINE_DETERMINISTIC_TOOL)
[kit] anx:kit/python-gdpy ENABLED

ansh> kit update anx:kit/jepa-os-default oid:0xdeadbeef...
[kit] hot-update jepa-os-default 1.2.0 -> 1.3.0
[kit] new engine registered; 0 leases on old; old drained
[kit] anx:kit/jepa-os-default ENABLED (1.3.0)
```

### 9.2 HTTP API

```
GET    /api/v1/kits                       — list all kits with state
GET    /api/v1/kits/{uri-encoded}         — kit detail (manifest + status)
POST   /api/v1/kits/{uri}/enable          — enable
POST   /api/v1/kits/{uri}/disable         — disable
POST   /api/v1/kits/{uri}/update          — body: {"oid": "..."}
GET    /api/v1/kits/{uri}/health          — health probe
POST   /api/v1/kits                       — register from OID (body: {"oid": "..."})
```

### 9.3 WebSocket Events

On the existing event stream (port 9191 for browser, 8080 for general):

```json
{"type": "kit", "kind": "state_change", "uri": "...", "from": "ENABLING", "to": "ENABLED"}
{"type": "kit", "kind": "prereq_failed", "uri": "...", "prereq": {...}, "remediation": "..."}
{"type": "kit", "kind": "health_degraded", "uri": "...", "reason": "..."}
{"type": "kit", "kind": "update_complete", "uri": "...", "from_version": "1.2.0", "to_version": "1.3.0"}
```

The WM Capability Tray (Task 8) subscribes to this channel and renders state in real time.

---

## 10. Security

### 10.1 Signature Verification

Every kit's manifest is Ed25519-signed against a trust root identified by the manifest's `trust_root` credential reference. The trust root credential is held in the Credential Store (RFC-0008) under a well-known name (`anx-kit-root-2026` for first-party kits; user-installed trust roots use other names). `anx_kit_register` refuses to register a kit whose signature does not verify.

The kernel ships with a baked-in first-party root public key as a fallback for cold boot (before the credential store is mounted from disk). Boot kits are signed against this key.

### 10.2 Capability Gating

`anx_kit_enable`, `anx_kit_disable`, and `anx_kit_update` require the calling cell to hold `cap:kit-admin`. Shell and HTTP entry points check this against the active session (RFC-0007 capability tokens). `anx_kit_list`, `anx_kit_status`, `anx_kit_health` require `cap:kit-read`, which is granted by default.

### 10.3 Host-Side Prereq Consent

When a kit's prereq is `host-service` or `host-binary` and the kit subsystem has a remediation script available, **no remediation runs without explicit user consent.** Consent is captured through the WM Capability Tray (a "Run on host?" dialog with the command text visible) or, in headless mode, by an explicit `kit remediate <uri>` ansh command. The host-side helper (`tools/anxhost.py`, designed in Task 7) listens on a Unix socket on the host and refuses commands without a fresh consent token.

### 10.4 Provenance

Every kit lifecycle transition appends to the kit's provenance chain: who enabled it, when, from what session, with what credential. `kit info <uri>` exposes this history. RFC-0002 invariants on State Object provenance apply unmodified.

---

## 11. Migration Plan

This RFC supersedes ad-hoc loading in three subsystems. Migration happens after the foundation is in place and tested, not interleaved:

| Phase | Scope                                                  | Repo                     | RFC Task |
|-------|--------------------------------------------------------|--------------------------|----------|
| 1     | Kit kernel subsystem + State Object kind + tests       | Anunix                   | Task 2   |
| 2     | Kit shell/REST/WS surface                              | Anunix                   | Task 3   |
| 3     | JEPA migration (weights become a kit, old API kept)    | Anunix + Anunix-World    | Task 4   |
| 4     | Browser migration (native + proxy + peer all kits)     | Anunix + Anunix-Browser  | Task 5   |
| 5     | GDPy as a kit + ansh `python` builtin                  | Anunix + Anunix-Python   | Task 6   |
| 6     | Host-side prereq channel + anxhost.py                  | Anunix + Anunix-tools    | Task 7   |
| 7     | WM Capability Tray                                     | Anunix                   | Task 8   |
| 8     | JEPA world hot-swap UI + WM browser window             | Anunix                   | Task 9   |

Phases 1–2 ship without changing any existing subsystem behavior; the kit registry exists but is empty (except for a placeholder boot kit used for self-test).

Phases 3–5 each migrate one subsystem. Each migration ships the kit and the consumer change *and* keeps the existing shell/API surfaces working as thin wrappers over the new path. `browser_init` still works; under the hood it calls `kit enable`. `jepa world set` still works; under the hood it calls `kit enable` on the named jepa-world kit. No external behavior breaks during migration.

Phase 6 retires the manual `socat`/`anxbproxy` invocation from documentation; failure to launch the proxy becomes a one-click fix.

Phases 7–8 are pure UX — they expose what the kit registry already knows.

---

## 12. Open Questions

1. **Manifest binary encoding.** Canonical JSON is convenient but verbose. A TLV/protobuf-lite binary form is preferred for signature speed and reproducibility; the canonical JSON is the documentation form, not the on-disk form. Final encoding to be picked in Task 2.

2. **Cross-kit dependency cycles.** A `requires`/`provides` graph can in principle contain cycles. Initial implementation: refuse to enable a kit whose requirement closure has a cycle. Long-term, we may permit declared late-binding edges.

3. **Lease semantics for engine-binary kits.** A celf-payload kit registers an engine, but the celf binary itself is the "kit body." If a kit is disabled while the celf process is mid-execution, what happens? Initial implementation: the celf cell receives an `ANX_EVENT_KIT_DRAIN` and has a `lease_drain_timeout_ms` to exit cleanly; after that it is killed.

4. **Boot-kit revocation.** A boot kit baked into the kernel image cannot be unregistered (it would un-init the kernel). We mark builtin kits with a `BUILTIN_LOCK` flag that refuses `anx_kit_unregister`. Updates are still permitted — the boot kit is the floor, and update produces a new active OID alongside the locked builtin.

5. **Composite kits.** A composite kit references and orchestrates other kits. Initial design treats this as a workflow (RFC-0018) — the composite's enable hook is a Workflow that enables its dependencies in topological order. To be detailed when the first composite case appears.

---

## 13. Why Now

Anunix is at the inflection point where the next several capabilities — `anxml` inference runtime (RFC-0021), GPU compute plane (RFC-0022), `amacs` editor (RFC-0023), audio engine (RFC-0024) — will each need a loading story. If we let each invent its own, we will spend the next year debugging four more ad-hoc paths and the user experience will be the union of their failure modes. The Kit primitive is small (one new State Object type, ~1500 LOC of kernel code, three new shell commands), introduces no new architectural concept that the OS does not already lean on (State Objects, Engines, Capabilities, Credentials, Workflows), and pays its cost back the first time we ship a runtime-downloadable model.

It is also the foundation for honest agent self-improvement: an agent that can `anx_kit_update` its own JEPA weights — with provenance, signature, drain, and rollback — is meaningfully self-improving. An agent that has to ask a human to rebuild the kernel is not.

---

## 14. References

- RFC-0001 — Architecture Thesis
- RFC-0002 — State Object Model (kit objects are state objects)
- RFC-0003 — Execution Cell Runtime (celf kits run as cells)
- RFC-0005 — Routing Plane (kit-published engines plug into routing)
- RFC-0007 — Capability Objects (gating; distinct from kits)
- RFC-0008 — Credential Objects (trust roots, signatures)
- RFC-0013 — Tensor Objects (model-weights kits reference tensors)
- RFC-0018 — Workflow Objects (composite kits orchestrate via workflows)
- RFC-0020 — Iterative Belief-Action Loop (consumer of JEPA kits)
