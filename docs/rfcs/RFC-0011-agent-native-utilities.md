# RFC-0011: Agent-Native Utilities and Hardware Discovery

| Field      | Value                                          |
|------------|------------------------------------------------|
| RFC        | 0011                                           |
| Title      | Agent-Native Utilities and Hardware Discovery  |
| Author     | Adam Pippert                                   |
| Status     | Draft                                          |
| Created    | 2026-04-16                                     |
| Updated    | 2026-04-16                                     |
| Depends On | RFC-0001 through RFC-0010                      |

---

## Executive Summary

This RFC specifies **agent-native utilities**: programs that exist only because Anunix has first-class AI agent constructs. These utilities have no meaningful POSIX equivalent — they expose Execution Cells, Engines, State Object provenance, Capability lifecycle, Memory tier dynamics, and Routing decisions as first-class inspectable and controllable surfaces.

The RFC also specifies the **`hwd` hardware discovery agent**, which probes hardware at boot and on-demand, generates structured hardware profiles and driver stubs via model engines, and posts results to a central server library (`superrouter`). The `superrouter` server design is included here.

---

## 1. Status

**Status:** Draft  
**Author:** Adam Pippert  
**Depends on:** RFC-0001 through RFC-0010  
**Blocks:** None currently

---

## 2. What Makes a Utility "Agent-Native"

A POSIX utility reads stdin, writes stdout, runs a job, exits. An agent-native utility differs in three ways:

1. It expresses its work as an **Execution Cell with intent, plan, and trace** — every invocation is auditable and reproducible.
2. It **routes dynamically** to the appropriate engine (deterministic, local model, remote model) rather than hardcoding an implementation.
3. Its outputs are **State Objects with type, schema, provenance, and validation state** — not byte streams that vanish when the pipe closes.

---

## 3. `hwd` — Hardware Discovery Agent

### 3.1 Purpose

`hwd` is the hardware discovery agent. It:

1. Probes machine hardware using deterministic tools (PCI bus scan, ACPI tables, device tree on ARM64)
2. Routes a synthesis cell to a model engine to produce a structured `anx:hw-profile/v1` State Object
3. Generates driver stub source objects (`anx:driver-stub/v1`) for unknown device classes
4. Posts the profile and stubs to the `superrouter` server library via `anx-fetch`
5. Installs a Capability Object for each device class with a generated stub

On subsequent runs (`hwd rescan`), diffs the new inventory against the stored profile and only posts a delta.

### 3.2 Cell Decomposition

Root cell: `ANX_CELL_TASK_EXECUTION`, intent `"hardware-discovery"`.

Child cells:

| Cell Intent | Type | Engine Class | Inputs | Outputs |
|---|---|---|---|---|
| `probe-pci` | `TASK_DECOMPOSITION` | `DETERMINISTIC_TOOL` | — | `ANX_OBJ_BYTE_DATA` (raw PCI list) |
| `probe-acpi` | `TASK_DECOMPOSITION` | `DETERMINISTIC_TOOL` | — | `ANX_OBJ_BYTE_DATA` (ACPI tables) |
| `probe-devicetree` | `TASK_DECOMPOSITION` | `DETERMINISTIC_TOOL` | — | `ANX_OBJ_BYTE_DATA` (DTB, ARM64 only) |
| `synthesize-profile` | `TASK_DECOMPOSITION` | `LOCAL_MODEL` or `REMOTE_MODEL` | PCI + ACPI + DTB objects | `ANX_OBJ_STRUCTURED_DATA` (hw-profile) |
| `generate-stubs` | `TASK_DECOMPOSITION` | `LOCAL_MODEL` or `REMOTE_MODEL` | hw-profile (unknown devices) | `ANX_OBJ_BYTE_DATA[]` (stub .c files) |
| `push-profile` | `TASK_SIDE_EFFECT` | `EXECUTION_SERVICE` | hw-profile + stubs | HTTP 201 response |

Routing strategy for synthesis cells: `ANX_ROUTE_LOCAL_FIRST`, fallback `ANX_ROUTE_COST_FIRST`. Budget class: `ANX_BUDGET_BACKGROUND_ENRICHMENT` at boot; `ANX_BUDGET_INTERACTIVE_PRIVATE` for on-demand rescans.

Required capabilities for synthesis: `ANX_CAP_STRUCTURED_EXTRACTION`. For stub generation: `ANX_CAP_CODE_GENERATION`.

### 3.3 State Object Outputs

| Object | Type | Schema | Lifecycle | Tier |
|---|---|---|---|---|
| Raw PCI list | `ANX_OBJ_BYTE_DATA` | — | Ephemeral (dropped after synthesis) | `ANX_MEM_L1` |
| Raw ACPI tables | `ANX_OBJ_BYTE_DATA` | — | Ephemeral | `ANX_MEM_L1` |
| Hardware profile | `ANX_OBJ_STRUCTURED_DATA` | `anx:hw-profile/v1` | Sealed after creation | `ANX_MEM_L4` |
| Driver stub | `ANX_OBJ_BYTE_DATA` | `anx:driver-stub/v1` | Sealed after creation | `ANX_MEM_L3` |

Provenance: all output objects carry `ANX_PROV_DERIVED_FROM` events linking back to the raw probe inputs. The `probe_cell_id` is stored in the hw-profile object's metadata.

### 3.4 CLI Interface

```
hwd                          # Full discovery (boot mode, minimal output)
hwd --verbose                # Full discovery with step output
hwd rescan                   # Re-probe and diff against stored profile
hwd show                     # Print current hardware profile
hwd show --json              # Machine-readable
hwd stubs list               # List generated driver stubs
hwd stubs show <oid>         # Dump driver stub source
hwd push                     # Push profile to superrouter (manual)
hwd push --dry-run           # Show payload without sending
hwd status                   # Cell lifecycle state of last discovery run
hwd trace <cid>              # Full execution trace for a discovery cell
```

### 3.5 Driver Stub Schema (`anx:driver-stub/v1`)

Generated driver stubs are C source files following Anunix conventions:

```c
/* Anunix driver stub: AMD Radeon RX 7700S (PCI 0x1002:0x7480)
 * Generated by: claude-sonnet-4-6 via hwd synthesize-profile
 * Source profile: <oid-abbreviated>
 * Status: unvalidated — requires human review before integration
 */

#include <anx/driver.h>
#include <anx/pci.h>

#define ANX_DRV_VENDOR_ID  0x1002
#define ANX_DRV_DEVICE_ID  0x7480

static int
amdgpu_rx7700s_probe(struct anx_pci_device *dev)
{
        /* TODO: implement probe */
        return ANX_ENOTIMPL;
}

static struct anx_pci_driver amdgpu_rx7700s_driver = {
        .name      = "amdgpu-rx7700s",
        .vendor_id = ANX_DRV_VENDOR_ID,
        .device_id = ANX_DRV_DEVICE_ID,
        .probe     = amdgpu_rx7700s_probe,
};
```

All generated stubs set `return ANX_ENOTIMPL` in every function. They are not compiled into the kernel automatically; a human must review and promote them.

---

## 4. Agent-Native Utility Catalog

### 4.1 `celf` — Cell Inspector

Replaces `ps`, `strace`, `ltrace`, and `lsof` combined. Shows the live cell tree with intent, plan, engine assignment, and trace events.

**Unique capability**: `celf trace <cid>` shows not just what is running but what it is trying to do, how many steps into its plan, which model invocations occurred, and whether validation passed. This is impossible on POSIX — `ps` has no concept of intent or plan.

**API**: `anx_cell_store_lookup`, `anx_cell_trace`, cell lifecycle read-path. No engine routing needed.

```
celf                   # Live cell tree
celf -l                # Long listing: status, engine, intent, queue class
celf <cid>             # Inspect one cell
celf trace <cid>       # Full execution trace
celf watch             # Live-updating tree (refresh on state change)
celf kill <cid>        # Cancel cell (anx_cell_cancel)
celf --json            # Machine-readable
```

**Priority**: High. First utility to implement after `libanx`. Required for debugging all other utilities.

**Depends on**: Cell runtime functional.

---

### 4.2 `engctl` — Engine Registry Controller

Manages the Engine Registry: lists engines, status, capability bitmasks, quality scores, and resource leases. Loads/unloads model engines, runs benchmarks, displays routing feedback history per engine.

**Unique capability**: `engctl bench <eid>` spawns a probe cell, runs a standard workload against the engine, and records quality and latency scores that the routing plane uses. No POSIX analog for registering and benchmarking compute engines as first-class kernel objects.

**API**: `anx_engine_registry_init`, `anx_engine_find`, `anx_engine_transition`, `anx_route_get_feedback`, `anx_lease_lookup`.

```
engctl list            # All engines, status, caps, lease
engctl show <eid>      # Detailed engine info
engctl load <eid>      # Transition to AVAILABLE
engctl unload <eid>    # Drain and unload
engctl bench <eid>     # Run benchmark probe cell
engctl feedback <eid>  # Recent routing feedback records
engctl register        # Interactive registration
```

**Priority**: High. Required to validate routing plane on real hardware and configure model engines.

**Depends on**: Engine registry and lease subsystem functional.

---

### 4.3 `objls` / `objcat` / `objprov` — State Object Tools

Three tools for the object store:

- `objls`: list objects filtered by type, schema, creator cell, or validation state
- `objcat`: render object payload to stdout (type-aware for structured data)
- `objprov`: walk and render the full provenance log as a tree

**Unique capability**: `objprov` answers "where did this data come from, which model produced it, and was that invocation validated?" — impossible on POSIX where files have no provenance graph. Flag `--dot` outputs Graphviz DOT format.

**API**: `anx_objstore_lookup`, `anx_so_open`, `anx_so_read_payload`, `anx_prov_log_get`, `anx_memplane_lookup`.

```
objls                              # List all objects
objls --type structured            # Filter by object type
objls --schema anx:hw-profile/v1   # Filter by schema URI
objls --cell <cid>                 # Objects produced by a cell
objls --tier L3                    # Objects in a specific tier
objcat <oid>                       # Render payload
objcat --raw <oid>                 # Raw bytes
objprov <oid>                      # Full provenance tree
objprov --dot <oid>                # Graphviz DOT output
```

**Priority**: High. Required to inspect all `hwd` outputs and validate the state object layer.

---

### 4.4 `capctl` — Capability Object Manager

Full lifecycle management for Capability Objects. Creates, validates, installs, suspends, supersedes, and retires capabilities. The `capctl validate` subcommand spawns an `ANX_CELL_TASK_VALIDATION` cell that runs a test suite and transitions the capability to `ANX_CAP_VALIDATED` on pass.

**Unique capability**: Capabilities are not static packages — they have trust scores, invocation counters, and can be superseded. `capctl bench` records baseline performance by running the capability against a standard workload. No POSIX analog.

**API**: `anx_cap_create`, `anx_cap_transition`, `anx_cap_install`, `anx_cap_uninstall`, `anx_cap_record_invocation`.

```
capctl list                    # All capabilities, status, trust scores
capctl show <oid>              # Detailed info
capctl create <name>           # Create DRAFT capability
capctl validate <oid>          # Run validation suite
capctl install <oid>           # Install validated capability
capctl suspend <oid>           # Suspend
capctl supersede <old> <new>   # Supersede with newer capability
capctl retire <oid>            # Permanently remove from routing
capctl bench <oid>             # Run benchmark workload
```

**Priority**: Medium. Required once `hwd` generates driver capabilities.

---

### 4.5 `credctl` — Credential Store Manager

Creates, rotates, revokes, and lists credentials from the kernel credential store (RFC-0008). Credentials are kernel-enforced opaque objects — they never appear in traces, logs, or provenance records.

**Unique capability**: `credctl rotate --schedule 30d <name>` spawns a recurring `ANX_CELL_TASK_SIDE_EFFECT` cell for automated rotation. The secret never appears on the command line or in any observable log. Required before any model API call or superrouter push.

**API**: `anx_credential_create`, `anx_credential_rotate`, `anx_credential_revoke`, `anx_credential_list`.

```
credctl list                          # Names, types, metadata (no payloads)
credctl show <name>                   # Metadata for one credential
credctl create <name> --type api_key  # Prompt for secret, create
credctl rotate <name>                 # Prompt for new secret, rotate
credctl revoke <name>                 # Zero payload immediately
credctl rotate --schedule 30d <name>  # Schedule automatic rotation
```

**Priority**: High. Required by `hwd push` (superrouter API key) and all model calls.

---

### 4.6 `rstat` — Routing Plane Statistics

Renders routing plane diagnostics: decision distribution by stage, engine selection ratios, fallback rates, cost and latency distributions, budget violation events.

**Unique capability**: `rstat diff <t1> <t2>` compares routing behavior before and after an engine change using stored feedback records, proving whether adding a new local model improved quality. No POSIX analog for observing AI engine routing decisions.

**API**: `anx_route_get_feedback`, `anx_engine_find`, `anx_budget_exceeded`.

```
rstat                    # Summary: decisions by stage, fallback rate
rstat engines            # Per-engine: selection, success, latency
rstat history [n]        # Last n feedback records
rstat diff <t1> <t2>     # Compare routing behavior between timestamps
rstat budget             # Budget violation events
rstat --json             # Machine-readable
```

**Priority**: Medium. High value during hardware bringup to validate engine configuration.

---

### 4.7 `intent` — Cell Composer

Takes a natural language task description and produces a serialized `anx_cell` + `anx_cell_plan` that can be inspected, edited, and submitted. The plan is a first-class State Object (`anx:cell-plan/v1`) that can be versioned, diffed, validated, and replayed.

**Unique capability**: This is the agent-native replacement for shell script authoring. The resulting plan has explicit engine assignments, fallbacks, and validation steps. Plans are durable objects — they survive reboots and can be shared. No POSIX analog.

**API**: Spawns `ANX_CELL_TASK_PLAN_GENERATION` cell, routes via `ANX_ROUTE_CONFIDENCE_FIRST` to model engine with `ANX_CAP_STRUCTURED_EXTRACTION`. Output stored as `ANX_OBJ_STRUCTURED_DATA`.

```
intent "describe the task"      # Generate plan, print and save
intent show <pid>               # Show stored plan
intent run <pid>                # Submit plan for execution
intent run <pid> --dry-run      # Validate without executing
intent diff <pid1> <pid2>       # Diff two plans
intent list                     # Recently generated plans
intent edit <pid>               # Open plan in $EDITOR
```

**Priority**: Medium. Requires model engine available and cell plan runtime functional.

---

### 4.8 `mem` — Agent Memory Interface

Shell interface to the Agent Memory System (RFC-0009). Creates memory objects, queries by semantic similarity, walks the memory graph, and inspects decay state.

**Unique capability**: `mem search "hardware failed last week"` performs a vector similarity query over stored memories by meaning, not filename. `mem graph <oid>` traverses typed edges (`CAUSED_BY`, `CONTRADICTS`). No POSIX filesystem can answer "find the memory most semantically similar to this query."

**API**: `anx_memory_create`, `anx_memory_search`, `anx_memory_add_edge`, `anx_memory_episode_list`. Search cell routes to `ANX_ENGINE_LOCAL_MODEL` with `ANX_CAP_SEMANTIC_RETRIEVAL`.

```
mem list              # Memories in current episode
mem list --all        # All memory objects
mem show <oid>        # Show a memory object
mem add "text..."     # Create memory in current episode
mem search "query"    # Semantic similarity search
mem graph <oid>       # Walk memory graph
mem budget            # Memory budget usage
mem decay <oid>       # Decay state for an object
```

**Priority**: Medium. Depends on RFC-0009 Phase 1 (episodic store).

---

### 4.9 `decay` — Memory Decay Inspector

Exposes the Memory Control Plane's decay state. Lists objects sorted by decay score, shows tier placement, validation state, confidence percentages, and contradiction counts.

**Unique capability**: `decay top` is the agent-native analog of `top` for epistemic state — which memories are being challenged, which are stale, which are about to be forgotten. No POSIX analog for tracking whether file content is validated or contested.

**API**: `anx_memplane_lookup`, `anx_memplane_decay_sweep` (read path), `anx_memplane_forget`.

```
decay top               # Highest-decay-score objects
decay list              # All entries sorted by decay score
decay show <oid>        # Decay state for one object
decay contested         # ANX_MEMVAL_CONTESTED objects
decay force <oid> tombstone  # Manual forget with mode
decay sweep             # Trigger decay sweep (debug)
```

**Priority**: Low. Requires memplane populated with real data.

---

## 5. Superrouter Server

### 5.1 Purpose

`superrouter` is a Linux server (temporary host; will migrate to a dedicated machine when a domain is configured) that:

- Receives hardware profiles from `hwd push`
- Stores driver stubs indexed by PCI vendor/device ID and architecture
- Serves driver queries from any Anunix node
- Will host additional OS services as the project matures

The stack is intentionally minimal: a single Python file, standard library only, SQLite for storage, JSON over HTTP. No framework. The client on Anunix is `anx-fetch` using an API key stored via `credctl`.

### 5.2 Data Model

#### Hardware Profile (`anx:hw-profile/v1`)

```json
{
  "profile_id": "<uuid>",
  "node_id": "<anx-uuid>",
  "hostname": "framework-dev-01",
  "arch": "x86_64",
  "submitted_at": "<iso8601>",
  "kernel_version": "0.1.0",
  "probe_cell_id": "<anx-cid>",
  "profile_oid": "<anx-oid>",
  "profile_version": 3,
  "cpu": {
    "count": 16,
    "vendor": "AuthenticAMD",
    "model": "AMD Ryzen 9 7940HX",
    "features": ["avx2", "avx512f", "aes"]
  },
  "ram_bytes": 68719476736,
  "accelerators": [
    {
      "type": "gpu",
      "name": "AMD Radeon RX 7700S",
      "mem_bytes": 8589934592,
      "compute_units": 36
    }
  ],
  "pci_devices": [
    {
      "vendor_id": "0x1002",
      "device_id": "0x7480",
      "class": "0x03",
      "subclass": "0x00",
      "vendor_name": "Advanced Micro Devices",
      "device_name": "Radeon RX 7700S",
      "driver_status": "stub_generated"
    }
  ]
}
```

`probe_cell_id` and `profile_oid` enable round-trip provenance: a driver pulled from superrouter traces back to the profile submission that triggered its creation.

#### Driver Stub Record

```json
{
  "stub_id": "<uuid>",
  "vendor_id": "0x1002",
  "device_id": "0x7480",
  "class_code": "0x03",
  "arch": "x86_64",
  "profile_id": "<uuid>",
  "generated_by": "claude-sonnet-4-6",
  "generated_at": "<iso8601>",
  "stub_oid": "<anx-oid>",
  "stub_text": "/* driver stub source ... */",
  "validation_state": "unvalidated",
  "download_count": 0,
  "superseded_by": null
}
```

### 5.3 SQLite Schema

```sql
CREATE TABLE profiles (
    profile_id   TEXT PRIMARY KEY,
    node_id      TEXT NOT NULL,
    hostname     TEXT,
    arch         TEXT NOT NULL,
    submitted_at TEXT NOT NULL,
    payload      TEXT NOT NULL
);

CREATE TABLE driver_stubs (
    stub_id          TEXT PRIMARY KEY,
    vendor_id        TEXT NOT NULL,
    device_id        TEXT NOT NULL,
    class_code       TEXT NOT NULL,
    arch             TEXT NOT NULL,
    profile_id       TEXT NOT NULL REFERENCES profiles(profile_id),
    stub_text        TEXT NOT NULL,
    validation_state TEXT NOT NULL DEFAULT 'unvalidated',
    generated_at     TEXT NOT NULL,
    generated_by     TEXT NOT NULL,
    stub_oid         TEXT,
    download_count   INTEGER NOT NULL DEFAULT 0,
    superseded_by    TEXT REFERENCES driver_stubs(stub_id)
);

CREATE TABLE api_tokens (
    token_hash  TEXT PRIMARY KEY,
    node_id     TEXT NOT NULL,
    hostname    TEXT,
    created_at  TEXT NOT NULL,
    last_used   TEXT,
    revoked     INTEGER NOT NULL DEFAULT 0
);
```

### 5.4 HTTP API

All endpoints require `Authorization: Bearer <token>`. All responses are `application/json`.

| Method | Path | Description |
|---|---|---|
| `POST` | `/v1/profiles` | Submit hardware profile (upserts on `node_id`) |
| `GET` | `/v1/profiles/<id>` | Fetch one profile |
| `GET` | `/v1/profiles?arch=x86_64` | List profiles by arch |
| `POST` | `/v1/stubs` | Submit driver stub |
| `GET` | `/v1/stubs?vendor_id=0x1002&device_id=0x7480` | Query stubs by device |
| `GET` | `/v1/stubs/<id>` | Fetch one stub |
| `POST` | `/v1/stubs/<id>/download` | Record download (increments counter) |
| `GET` | `/v1/health` | Health check (no auth) |
| `POST` | `/v1/admin/tokens` | Provision API token (admin secret required) |

`POST /v1/profiles` response:

```json
{ "profile_id": "<uuid>", "profile_version": 3, "new_stubs_requested": true }
```

`new_stubs_requested: true` when any PCI device in the profile has no existing stub. The client uses this to decide whether to spawn a `generate-stubs` cell.

### 5.5 Implementation

Single file: `superrouter/server.py`. Standard library only: `http.server`, `sqlite3`, `json`, `hashlib`, `uuid`, `logging`.

Tokens are stored as SHA-256 hashes. Raw token appears once at provisioning time (`credctl create superrouter-api-key`); server never stores it in plain text.

Runs as a systemd service on port `8420`. Database at `/var/lib/anunix-superrouter/db.sqlite3`. Stubs also written as flat files to `/var/lib/anunix-superrouter/stubs/<stub_id>.c` for manual inspection.

When domain is configured: nginx TLS termination in front, server unchanged.

Future migration: swap Python server for a C server (POSIX sockets, SQLite C API) to run natively on Anunix.

---

## 6. Source Layout

```
userland/
  hwd/
    hwd.c               # CLI entry, cell orchestration
    probe.c             # probe-pci, probe-acpi, probe-devicetree cells
    synthesize.c        # model cells: synthesize-profile, generate-stubs
    push.c              # push-profile side-effect cell
    Makefile
  celf/
    celf.c
    Makefile
  objstore/
    objls.c
    objcat.c
    objprov.c
    Makefile
  engctl/
    engctl.c
    Makefile
  capctl/
    capctl.c
    Makefile
  credctl/
    credctl.c
    Makefile
  rstat/
    rstat.c
    Makefile
  intent/
    intent.c
    Makefile
  mem/
    mem.c
    Makefile
  decay/
    decay.c
    Makefile

superrouter/
  server.py
  schema.sql
  install.sh
```

---

## 7. Implementation Wave Plan

### Wave 1 — Unblock Hardware Bringup

Dependencies: credential store, http client, cell runtime functional.

1. `credctl` — install API keys before anything else
2. `anx-fetch` — HTTP client (see RFC-0010 Tier 3, also agent-native)
3. `superrouter` server — standalone, no kernel dependency; deploy on Linux host immediately
4. `celf` — needed for debugging everything else
5. `hwd` — requires pci, acpi, cell runtime, model engine, anx-fetch, credctl, superrouter

### Wave 2 — Make the System Observable

Dependencies: Wave 1 complete, state object store and engine registry functional.

1. `objls` / `objcat` / `objprov`
2. `engctl`
3. `rstat`
4. `capctl`

### Wave 3 — Agent Memory and Composition

Dependencies: RFC-0009 Phase 1, model engine functional.

1. `mem`
2. `intent`
3. `decay`

---

## 8. Open Questions

1. **`hwd` at boot**: should hardware discovery block boot (synchronous) or run as a background cell while the system continues booting? Recommend: probe phase is synchronous (fast, deterministic), synthesis and push are background cells.

2. **Driver stub review workflow**: generated stubs need human review before kernel integration. Should `capctl validate` be the gate, or a separate `hwd stubs promote <oid>` command that triggers a human-review cell? TBD in a future RFC.

3. **Superrouter fleet sharing**: should hardware profiles from multiple Anunix nodes inform driver stub generation for all nodes? I.e., if node A generates a stub for a GPU, should node B fetch it automatically? Recommend: opt-in via `hwd rescan --fetch-stubs`. Fleet sharing policy in a future RFC.
