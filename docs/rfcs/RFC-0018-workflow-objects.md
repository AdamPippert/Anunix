# RFC-0018: Workflow Objects — Graph-Structured Execution as First-Class Primitives

| Field      | Value                                                                        |
|------------|------------------------------------------------------------------------------|
| RFC        | 0018                                                                         |
| Title      | Workflow Objects — Graph-Structured Execution as First-Class Primitives      |
| Author     | Adam Pippert                                                                 |
| Status     | Draft                                                                        |
| Created    | 2026-04-21                                                                   |
| Updated    | 2026-04-21                                                                   |
| Depends On | RFC-0001, RFC-0002, RFC-0003, RFC-0005, RFC-0007, RFC-0008, RFC-0012, RFC-0016 |

---

## Executive Summary

AI workloads are not programs in the classical sense. A model inference call chains into a tool call, which branches on output, which fans out to parallel retrieval operations, which merge into a synthesis step awaiting human approval. This is a directed graph of typed operations, not a linear sequence of instructions. Classical programs hide this graph inside opaque process state: the user cannot see it, inspect it, modify it, or resume it after a failure.

RFC-0018 introduces the **Workflow Object** — a new `ANX_OBJ_WORKFLOW` State Object type — and the **Workflow Cell** — a new `ANX_CELL_WORKFLOW` Execution Cell subtype — that together make the execution graph a first-class kernel primitive. A Workflow Object is simultaneously the program definition (the node graph), the running process (the active execution), the log (provenance on every node transition), and the configuration (node parameters, policy, credentials). It is serializable, OID-addressable, versioned, and visually editable through the Interface Plane (RFC-0012).

The result is a system where AI workloads are auditable, resumable, composable, and interactively editable — not black boxes.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert
**Depends on:** RFC-0001, RFC-0002, RFC-0003, RFC-0005, RFC-0007, RFC-0008, RFC-0012, RFC-0016
**Blocks:** —

---

## 2. Problem Statement

### 2.1 AI Workloads Are Graphs

A generative AI workload has structure: a trigger fires, inputs are retrieved, a model is called, its output is routed to one of several tools depending on content, tool outputs are merged, a human reviews the result, and a final output is written to a State Object. This is a directed acyclic graph (or cyclic, for agents that loop). The graph is the program.

Classical OS primitives have no representation for this structure. A process is a sequential program counter; its branching, looping, and concurrency are invisible to the kernel. An agent running inside a cell may construct this graph internally, but the kernel cannot inspect, checkpoint, resume, or re-route it. When the cell crashes at the human-review step, the entire partial result is lost.

### 2.2 The Launcher/Process/Log/Config Split

In every classical OS, a user-facing application exists in four disconnected places: the launcher icon (desktop entry), the running process (PID), the log (syslog entries), and the configuration (dotfiles or registries). These four representations are kept in sync by convention, not by kernel enforcement. For AI workloads this split is unacceptable: the "program" may run for hours, accumulate gigabytes of intermediate state, require human interaction mid-run, and need to be auditable post-run by a separate agent.

A Workflow Object unifies all four: the object IS the program definition, the running execution, the provenance trail, and the policy/credential configuration.

### 2.3 Visibility and Editability

Users and operators need to see what an AI agent is actually doing. "The model is thinking" is not useful feedback for a 20-minute synthesis workflow. The execution graph, with live status per node, is the correct feedback mechanism. And if a node's parameters are wrong — the wrong model, the wrong retrieval strategy, the wrong approval threshold — the user needs to edit the graph and re-run without rebuilding the workflow from scratch.

This requires the graph to be a kernel object, not an application-internal data structure.

---

## 3. Goals

### 3.1 Primary Goals

1. **Workflow Object as State Object.** `ANX_OBJ_WORKFLOW` is a first-class State Object type: OID-addressable, sealing/versioning, provenance-tracked, accessible to agents through the standard object API.

2. **Workflow Cell as Execution Cell.** A running workflow execution is an `ANX_CELL_WORKFLOW`. It is admitted, traced, and policy-checked like every other cell. The cell and object share identity.

3. **Typed node graph.** The workflow is a directed graph of typed nodes (`enum anx_wf_node_kind`) with typed ports and edges. The kernel understands the graph structure semantically.

4. **Visual editing through RFC-0012.** The Interface Plane can render the node graph as an `ANX_CONTENT_CANVAS` surface. Right-clicking any surface with a `workflow_oid` binding opens the visual editor.

5. **AI-native node types.** Model inference, agent invocation, RAG retrieval, and human-in-the-loop approval are first-class node kinds, not simulated through generic computation.

6. **Composability.** Workflows reference sub-workflows via `ANX_WF_NODE_SUBFLOW`. Sub-workflow objects are ordinary Workflow Objects; the reference is by OID.

7. **Credential and capability gating.** Workflow execution and editing require named capabilities. Model calls inherit the workflow's credential scope.

8. **Column-tiled WM layout.** The Interface Plane gains `ANX_LAYOUT_TILED_COLUMNS` for Niri-style scrollable column layout; workflow editor surfaces tile alongside their parent surface.

### 3.2 Non-Goals

- **Workflow orchestration engine as a service.** Execution is driven by the kernel cell scheduler, not a userland daemon.
- **Full visual graph editor UI.** Phase 1 renders a static canvas layout; interactive drag-and-drop editing is Phase 2.
- **Long-running workflow persistence across reboots.** Persistent workflow store is Phase 3.
- **External workflow format import** (Airflow DAGs, GitHub Actions YAML). Deferred; RFC-0016 CEXL is the workflow authoring language.

---

## 4. Core Definitions

### 4.1 Workflow Object (`ANX_OBJ_WORKFLOW`)

A **Workflow Object** is a State Object of type `ANX_OBJ_WORKFLOW`. It contains the complete directed graph (nodes + edges), the execution policy, the credential binding, and the execution state of the most recent or current run. It exists and is addressable regardless of whether a workflow execution is in progress.

A Workflow Object has one of four **execution states**: `IDLE`, `RUNNING`, `PAUSED`, or `FAILED`. These govern which operations are permitted and whether a Workflow Cell is currently bound.

### 4.2 Workflow Cell (`ANX_CELL_WORKFLOW`)

A **Workflow Cell** is an Execution Cell of type `ANX_CELL_WORKFLOW`. It is created when a Workflow Object transitions from `IDLE` or `PAUSED` to `RUNNING`. It drives topological execution: each time a node's inputs become available, the cell spawns a child Execution Cell of the appropriate type to execute that node. The Workflow Cell is the coordination layer; the per-node cells do the actual work.

At most one Workflow Cell may be bound to a Workflow Object at any time.

### 4.3 Run Context

A **Run Context** (`struct anx_wf_run_context`) is the per-execution mutable state: which nodes are pending, running, completed, or failed; the output State Object OID produced by each completed node; and the run start time and current status. The run context is a child State Object of the Workflow Object and is replaced at the start of each execution.

### 4.4 Workflow Surface

A **Workflow Surface** is a Surface Object (RFC-0012) whose `workflow_oid` field is non-zero. The Interface Plane uses this binding to render the "Edit Workflow" context menu entry and to open the node graph canvas. Any surface created by a Workflow Cell automatically receives the workflow's OID in its `workflow_oid` field.

---

## 5. Object and Cell Type Additions

This RFC adds `ANX_OBJ_WORKFLOW` to the State Object type enum (RFC-0002 Section 5) and `ANX_CELL_WORKFLOW` to the cell type enum (RFC-0003):

```c
/* kernel/include/anx/state_object.h */
ANX_OBJ_WORKFLOW = 10,		/* Workflow Object (RFC-0018) */

/* kernel/include/anx/cell.h */
ANX_CELL_WORKFLOW = 9,		/* Workflow execution cell (RFC-0018) */
```

Workflow Objects participate in the full State Object lifecycle (NASCENT → ACTIVE → SEALED/ARCHIVED/DELETED). A SEALED Workflow Object is a versioned snapshot of the graph definition; it cannot be modified but can be cloned into a new mutable object.

---

## 6. Node Types

```c
enum anx_wf_node_kind {
    ANX_WF_NODE_TRIGGER      = 0,  /* entry point: manual/cron/webhook/event */
    ANX_WF_NODE_STATE_REF    = 1,  /* read or write a State Object by OID */
    ANX_WF_NODE_CELL_CALL    = 2,  /* invoke a Cell by type + intent string */
    ANX_WF_NODE_MODEL_CALL   = 3,  /* call model server (inference) */
    ANX_WF_NODE_AGENT_CALL   = 4,  /* spawn an agent Cell with a goal string */
    ANX_WF_NODE_RETRIEVAL    = 5,  /* search/retrieve from State Object store */
    ANX_WF_NODE_CONDITION    = 6,  /* conditional branch: if/switch on output */
    ANX_WF_NODE_FAN_OUT      = 7,  /* parallel split: copy output to N nodes */
    ANX_WF_NODE_FAN_IN       = 8,  /* barrier join: wait for N upstreams */
    ANX_WF_NODE_TRANSFORM    = 9,  /* inline map/filter/reduce on structured data */
    ANX_WF_NODE_HUMAN_REVIEW = 10, /* pause; surface approval UI to user */
    ANX_WF_NODE_SUBFLOW      = 11, /* nested workflow reference by OID */
    ANX_WF_NODE_OUTPUT       = 12, /* sink: write result or emit event */
};
```

Node semantics:

| Kind             | Spawns Cell? | Blocks On                        | Output Type            |
|------------------|-------------|-----------------------------------|------------------------|
| TRIGGER          | No          | External event or schedule        | Trigger payload        |
| STATE_REF (read) | No          | —                                 | State Object           |
| STATE_REF (write)| No          | —                                 | Written OID            |
| CELL_CALL        | Yes         | Cell completion                   | Cell output object     |
| MODEL_CALL       | Yes         | Inference completion              | ANX_OBJ_TENSOR or text |
| AGENT_CALL       | Yes         | Agent cell completion             | Agent output object    |
| RETRIEVAL        | Yes         | Query completion                  | Result list object     |
| CONDITION        | No          | —                                 | Selected edge index    |
| FAN_OUT          | No          | —                                 | N copies of input      |
| FAN_IN           | No          | All N upstream completions        | Merged object array    |
| TRANSFORM        | No          | —                                 | Transformed object     |
| HUMAN_REVIEW     | No          | ANX_EVENT_POINTER_BUTTON (approve/reject) | Boolean + note |
| SUBFLOW          | Yes         | Sub-workflow cell completion      | Sub-workflow output    |
| OUTPUT           | No          | —                                 | Confirmation OID       |

---

## 7. Data Model

### 7.1 Node (`struct anx_wf_node`)

Each node has:
- **id**: uint32, unique within the workflow; stable across edits
- **kind**: `enum anx_wf_node_kind`
- **label**: human-readable string for UI display
- **params**: union keyed on `kind`:
  - TRIGGER: `schedule` string (cron expression or `"manual"`), `event_tag` string for event triggers
  - STATE_REF: `target_oid`, `access` (read/write), `field_path` (optional, for structured objects)
  - CELL_CALL: `cell_type` enum, `intent` string (passed to cell at admission)
  - MODEL_CALL: `model_id` string, `prompt_template` string, `max_tokens` uint32, `temperature` float
  - AGENT_CALL: `goal` string, `max_steps` uint32, `credential_scope` string
  - RETRIEVAL: `index_oid` (State Object OID of retrieval index), `query_template` string, `top_k` uint32
  - CONDITION: `expr` string (CEXL expression evaluated against upstream output), `branch_count` uint32
  - FAN_OUT: `fan_count` uint32
  - FAN_IN: `fan_count` uint32 (how many upstreams to wait for)
  - TRANSFORM: `op` enum (map/filter/reduce), `fn` string (inline CEXL function body)
  - HUMAN_REVIEW: `prompt` string shown in approval surface, `timeout_seconds` uint32 (0 = no timeout)
  - SUBFLOW: `workflow_oid` of the nested Workflow Object
  - OUTPUT: `target_oid` (destination State Object OID, or zero to emit as event), `event_tag` string
- **canvas_x**, **canvas_y**: float, position in visual editor canvas coordinate space
- **ports**: array of `struct anx_wf_port` — each port has a `name`, `direction` (in/out), and `type_tag` (ANX_OBJ_* constant or zero for untyped)

### 7.2 Edge (`struct anx_wf_edge`)

Each edge connects one output port to one input port:
- **from_node**: uint32 node id
- **from_port**: uint8 port index on the source node
- **to_node**: uint32 node id
- **to_port**: uint8 port index on the destination node

Edges are directional. Type compatibility between `from_port.type_tag` and `to_port.type_tag` is checked at validation time. Zero on either side means untyped (always compatible).

### 7.3 Workflow Object (`struct anx_wf_object`)

Top-level structure:
- **oid**: `anx_oid_t` — standard State Object identifier
- **name**: string — human-readable name, unique within credential namespace
- **description**: string
- **nodes**: array of `struct anx_wf_node`, length `node_count`
- **edges**: array of `struct anx_wf_edge`, length `edge_count`
- **credential_oid**: `anx_oid_t` — Credential Object (RFC-0008) providing model API keys and agent scope; inherited by all MODEL_CALL and AGENT_CALL nodes
- **exec_state**: execution state enum (IDLE/RUNNING/PAUSED/FAILED)
- **running_cid**: `anx_oid_t` — Cell ID of the active Workflow Cell, zero when IDLE
- **last_run_ts**: uint64 timestamp of last run start
- **last_status**: int — 0 = success, negative = error code from failed node
- **policy**: embedded struct with fields:
  - `auto_retry`: bool — retry failed nodes once before marking workflow FAILED
  - `timeout_ms`: uint64 — wall-clock timeout for the entire workflow run (0 = no limit)
  - `max_parallel`: uint32 — maximum concurrently active node cells (default 4)

### 7.4 Run Context (`struct anx_wf_run_context`)

Per-execution state, stored as a child State Object of the Workflow Object:
- **run_id**: uint64 — monotonically increasing run counter
- **start_ts**: uint64 timestamp
- **node_states**: array indexed by node id; each entry is:
  - `status` enum: PENDING / READY / RUNNING / DONE / FAILED / SKIPPED
  - `cell_oid`: `anx_oid_t` — OID of the cell executing this node (zero if not running)
  - `output_oid`: `anx_oid_t` — OID of the node's output State Object (zero until DONE)
  - `error_code`: int — non-zero on FAILED
  - `start_ts`, `end_ts`: uint64

---

## 8. Execution Model

### 8.1 Startup

When `anx_wf_run()` is called on a Workflow Object in IDLE state:

1. Credential check: calling cell must hold `ANX_CAP_WORKFLOW_RUN` capability for this workflow.
2. Graph validation: topological sort of the node graph. Cycles are permitted only through SUBFLOW nodes (the sub-workflow has its own execution context). A cycle in the main graph is rejected with `ANX_EINVAL`.
3. A new Run Context child object is created; all nodes set to PENDING.
4. An `ANX_CELL_WORKFLOW` is created and bound to the Workflow Object.
5. The object transitions to RUNNING state.
6. TRIGGER nodes are immediately promoted to DONE with the trigger payload as their output. All nodes with all inputs now satisfied are promoted to READY.

### 8.2 Steady-State Scheduling

The Workflow Cell runs a scheduling loop:

1. Collect all READY nodes.
2. Count currently RUNNING nodes. If count >= `policy.max_parallel`, block until a running node completes.
3. For each READY node (up to the concurrency cap), spawn the appropriate Execution Cell and mark the node RUNNING.
4. When any child cell completes, read its output, write it to the run context, mark the node DONE, and re-evaluate which downstream nodes are now READY.
5. Repeat until no nodes remain PENDING or RUNNING.

### 8.3 Node-Specific Behavior

**FAN_OUT.** On execution, makes `fan_count` copies of the upstream output object (shallow copy, distinct OIDs). Each copy is routed to a distinct outgoing edge. Downstream FAN_IN node(s) track which upstream OIDs have arrived.

**FAN_IN.** Maintains a counter of received upstream completions. When the counter reaches `fan_count`, the node outputs an array object containing all upstream output OIDs in arrival order. The Workflow Cell does not spawn a child cell for FAN_IN; it resolves the barrier inline.

**CONDITION.** Evaluates `params.expr` as a CEXL expression (RFC-0016) with the upstream output bound to the symbol `input`. The expression must return an integer in `[0, branch_count)`. The Workflow Cell routes execution to the edge matching that index; all other outgoing edges are bypassed, and their downstream nodes are marked SKIPPED if they have no other active inputs.

**HUMAN_REVIEW.** The Workflow Cell creates a Surface Object (RFC-0012) with content type `ANX_CONTENT_BUTTON` containing the node's `prompt` string and two buttons: Approve and Reject. The Workflow Object's `workflow_oid` is set on this surface. The Workflow Cell then suspends this node's progress and listens for `ANX_EVENT_POINTER_BUTTON` events on the surface. On Approve, the node outputs `{approved: true, note: ""}`. On Reject, the node outputs `{approved: false, note: <user-supplied text>}`. If `timeout_seconds` > 0 and the timeout expires, the node outputs `{approved: false, note: "timeout"}`.

**SUBFLOW.** Calls `anx_wf_run()` on the referenced sub-workflow object. The call is synchronous from the Workflow Cell's perspective: the parent node is RUNNING until the sub-workflow's Workflow Cell completes. The sub-workflow runs with the same `policy.max_parallel` budget subtracted from the parent's concurrency count.

### 8.4 Failure and Retry

When a node's child cell exits with a non-zero status:

1. If `policy.auto_retry` is true and this is the first failure of this node in this run, the node is reset to READY and re-scheduled once.
2. On second failure, or if `auto_retry` is false, the node is marked FAILED.
3. All nodes that are exclusively downstream of a FAILED node (no other ready input path exists) are marked SKIPPED.
4. If no further nodes can make progress, the Workflow Cell writes the last error code to `last_status`, transitions the Workflow Object to FAILED, and exits.

### 8.5 Timeout

If `policy.timeout_ms` > 0, the Workflow Cell sets a kernel timer at run start. On expiry, all RUNNING child cells receive a cancellation signal, all nodes still PENDING or RUNNING are marked FAILED with error `ANX_ETIMEDOUT`, and the Workflow Object transitions to FAILED.

---

## 9. Node Graph (ASCII Diagram)

```
 ┌─────────────┐
 │   TRIGGER   │  (cron or manual)
 └──────┬──────┘
        │ trigger_payload
        ▼
 ┌─────────────┐
 │  RETRIEVAL  │  (RAG: query knowledge index)
 └──────┬──────┘
        │ retrieved_docs
        ▼
 ┌─────────────┐
 │ MODEL_CALL  │  (inference: summarize + recommend)
 └──────┬──────┘
        │ model_output
   ┌────┴─────┐
   ▼          ▼
 ┌──────┐  ┌──────┐   (FAN_OUT: parallel tool calls)
 │CELL_A│  │CELL_B│
 └──┬───┘  └──┬───┘
    └────┬────┘
         ▼
  ┌─────────────┐
  │   FAN_IN    │  (barrier: wait for both)
  └──────┬──────┘
         │ merged_results
         ▼
  ┌─────────────┐
  │HUMAN_REVIEW │  (approval surface in WM)
  └──────┬──────┘
         │ {approved: true}
         ▼
  ┌─────────────┐
  │   OUTPUT    │  (write to State Object)
  └─────────────┘
```

---

## 10. WM Integration (RFC-0012 Extension)

### 10.1 `workflow_oid` Field on Surface Objects

Surface Objects (RFC-0012 Section 4.1) gain one new field:

- **workflow_oid**: `anx_oid_t` — OID of the Workflow Object this surface belongs to. Zero means the surface is not associated with a workflow.

The compositor sets this field automatically on any surface created by an `ANX_CELL_WORKFLOW` child cell. Applications may also set it explicitly when creating surfaces to opt into workflow editing.

### 10.2 Context Menu Integration

When the compositor receives a right-click event on a surface with a non-zero `workflow_oid`:

1. The compositor queries the Workflow Object by OID to retrieve its name and execution state.
2. The context menu includes an entry: **"Edit Workflow: \<name\>"**.
3. Selecting this entry causes the compositor to call `anx_wf_open_editor()`, which maps a new surface of type `ANX_CONTENT_CANVAS` containing the node graph rendered in static layout (Phase 1: read-only canvas; Phase 2: interactive drag-and-drop).
4. The new canvas surface has its own `workflow_oid` set to the same OID, so it tiles correctly and can itself be right-clicked to re-open the editor (no-op: editor is already open).

If the Workflow Object is RUNNING, the canvas surface shows each node's current status from the Run Context (PENDING/RUNNING/DONE/FAILED are color-coded). The canvas is refreshed each time a node state transition occurs (the Workflow Cell sends a surface damage event to the compositor).

### 10.3 WM Integration Diagram

```
  Window Manager (RFC-0012 compositor)
  ┌──────────────────────────────────────────────────────┐
  │  Column layout                                       │
  │  ┌──────────────────┐  ┌──────────────────────────┐ │
  │  │   App Surface    │  │  Workflow Editor Canvas  │ │
  │  │  (workflow_oid=W)│  │  (workflow_oid=W)        │ │
  │  │                  │  │  ┌───┐  ┌───┐  ┌───┐    │ │
  │  │  [right-click]   │  │  │ T │→ │ M │→ │ O │    │ │
  │  │  → Edit Workflow │  │  └───┘  └───┘  └───┘    │ │
  │  └──────────────────┘  └──────────────────────────┘ │
  └──────────────────────────────────────────────────────┘
```

---

## 11. Column Tiling Layout (RFC-0012 Extension)

### 11.1 `ANX_LAYOUT_TILED_COLUMNS`

RFC-0012 Section 8 (Layout Modes) gains a new mode:

```c
ANX_LAYOUT_TILED_COLUMNS = 3,   /* Niri-style scrollable column tiling (RFC-0018) */
```

In this mode:
- The display is divided into variable-width columns.
- Each column contains one or more surfaces stacked vertically; each surface occupies one row in its column.
- Column width is determined by the widest surface in the column, subject to a minimum of 320px and a maximum of the display width.
- Columns scroll horizontally when the total column width exceeds the display width. The active column is always kept in view.
- New surfaces are appended to the active column unless the spawning cell requests a new column via `ANX_SURFACE_FLAG_NEW_COLUMN`.
- Workflow editor surfaces are always opened in a new column adjacent to their parent surface's column, via `anx_wf_open_editor()` setting `ANX_SURFACE_FLAG_NEW_COLUMN`.

### 11.2 Navigation

Keyboard shortcuts in `ANX_LAYOUT_TILED_COLUMNS` mode (default bindings, user-remappable):

| Shortcut        | Action                                          |
|-----------------|-------------------------------------------------|
| Mod+H           | Focus column to the left                        |
| Mod+L           | Focus column to the right                       |
| Mod+J           | Focus surface below within column               |
| Mod+K           | Focus surface above within column               |
| Mod+Shift+H     | Move active surface to column on the left       |
| Mod+Shift+L     | Move active surface to column on the right      |
| Mod+Shift+J     | Move surface down within column                 |
| Mod+Shift+K     | Move surface up within column                   |
| Mod+N           | Create new column at the right of active column |
| Mod+W           | Close active surface                            |

The compositor emits standard RFC-0012 focus-change events on all navigation actions; these are routable State Objects and are observable by agents.

---

## 12. Shell Interface

The `workflow` builtin exposes Workflow Object operations to Anunix shell sessions and agent cells running CEXL (RFC-0016):

```
workflow create <name> [--desc <text>] [--cred <cred-oid>]
workflow list
workflow add-node <name> <kind> [--label <text>] [--params <json>]
workflow add-edge <name> <from-node-id>:<port> <to-node-id>:<port>
workflow run <name>
workflow pause <name>
workflow resume <name>
workflow status <name>
workflow dump <name>          # emit full graph + run context as structured object
workflow destroy <name>
workflow editor <name>        # open visual editor surface (requires WM session)
```

`workflow add-node` returns the assigned node id on success. `workflow dump` emits the Workflow Object as a CEXL-serializable structured object that can be piped to `workflow create` to clone the graph. All commands check capabilities before execution.

---

## 13. Credential and Capability Model

### 13.1 Capabilities

Two capabilities gate Workflow Object access:

```c
/* kernel/include/anx/workflow.h */
#define ANX_CAP_WORKFLOW_RUN    (1u << 0)   /* execute the workflow */
#define ANX_CAP_WORKFLOW_EDIT   (1u << 1)   /* add/remove/modify nodes and edges */
```

A cell must hold a Capability Object (RFC-0007) with `ANX_CAP_WORKFLOW_RUN` set to call `anx_wf_run()`. It must hold `ANX_CAP_WORKFLOW_EDIT` to call `anx_wf_node_add()`, `anx_wf_node_remove()`, `anx_wf_edge_add()`, `anx_wf_edge_remove()`, or any mutation to node params.

Capability Objects for workflows are scoped to a specific Workflow Object OID. A cell holding `ANX_CAP_WORKFLOW_RUN` for workflow W cannot run workflow X.

### 13.2 Credential Inheritance

Each Workflow Object has an optional `credential_oid` referencing a Credential Object (RFC-0008). All MODEL_CALL and AGENT_CALL nodes that do not specify their own credential inherit this object. The Workflow Cell presents this credential to child cells at admission time, subject to RFC-0008 delegation constraints.

If `credential_oid` is zero, MODEL_CALL and AGENT_CALL nodes that require a credential will fail at the credential check step (not at graph validation time, to allow offline graph construction).

### 13.3 Agent Scope Containment

Agent cells spawned by `ANX_WF_NODE_AGENT_CALL` nodes receive a Capability Object that is the intersection of:
- The workflow's credential scope
- The node's `credential_scope` parameter (if set; further restricts the inherited scope)
- The system-wide agent admission policy

Agents cannot escalate beyond the workflow's own capability set.

---

## 14. Kernel API Summary

```c
/* Creation and deletion */
int anx_wf_create(const char *name, const char *description,
                  const anx_oid_t *credential_oid,
                  const struct anx_wf_policy *policy,
                  anx_oid_t *wf_oid_out);
int anx_wf_destroy(const anx_oid_t *wf_oid);

/* Graph construction */
int anx_wf_node_add(const anx_oid_t *wf_oid,
                    const struct anx_wf_node *node,
                    uint32_t *node_id_out);
int anx_wf_node_remove(const anx_oid_t *wf_oid, uint32_t node_id);
int anx_wf_node_update(const anx_oid_t *wf_oid,
                       const struct anx_wf_node *node);
int anx_wf_edge_add(const anx_oid_t *wf_oid,
                    const struct anx_wf_edge *edge);
int anx_wf_edge_remove(const anx_oid_t *wf_oid,
                       uint32_t from_node, uint8_t from_port,
                       uint32_t to_node, uint8_t to_port);

/* Execution */
int anx_wf_run(const anx_oid_t *wf_oid);
int anx_wf_pause(const anx_oid_t *wf_oid);
int anx_wf_resume(const anx_oid_t *wf_oid);
int anx_wf_cancel(const anx_oid_t *wf_oid);

/* Status */
int anx_wf_status(const anx_oid_t *wf_oid,
                  struct anx_wf_run_context **ctx_out);
int anx_wf_list(anx_oid_t *results, uint32_t max,
                uint32_t *count_out);

/* Object access */
int anx_wf_dump(const anx_oid_t *wf_oid,
                struct anx_wf_object **wf_out);

/* WM integration */
int anx_wf_open_editor(const anx_oid_t *wf_oid,
                       anx_oid_t *surface_oid_out);
```

---

## 15. Implementation Plan

**Phase 1 — Data model, CRUD, execution, shell, static canvas (this RFC):**
- Add `ANX_OBJ_WORKFLOW` to state object type enum
- Add `ANX_CELL_WORKFLOW` to cell type enum
- Implement Workflow Object CRUD in `kernel/core/workflow/wf_object.c`
- Implement graph validation (topological sort, port type check) in `kernel/core/workflow/wf_graph.c`
- Implement Workflow Cell scheduler in `kernel/core/workflow/wf_cell.c`
- Implement node execution dispatch (TRIGGER, STATE_REF, CELL_CALL, MODEL_CALL, CONDITION, FAN_OUT, FAN_IN, OUTPUT) in `kernel/core/workflow/wf_exec.c`
- Implement HUMAN_REVIEW surface creation + event wait in `kernel/core/workflow/wf_review.c`
- Add `workflow_oid` field to `struct anx_surface_object` (RFC-0012 extension)
- Add `ANX_LAYOUT_TILED_COLUMNS` layout mode to compositor
- Implement static canvas renderer for node graph in `kernel/core/workflow/wf_canvas.c`
- Add `workflow` shell builtin in `kernel/core/tools/workflow.c`
- Tests: `tests/test_wf_object.c`, `tests/test_wf_graph.c`, `tests/test_wf_exec.c`

**Phase 2 — Live status, interactive editor, HUMAN_REVIEW, sub-flows:**
- Live run context updates push damage events to canvas surface (nodes pulse while RUNNING)
- HUMAN_REVIEW: full approve/reject UI with optional text note
- SUBFLOW execution: nested Workflow Cell with shared concurrency budget
- AGENT_CALL: full agent cell spawn with credential inheritance and scope containment
- Interactive canvas: drag nodes, add/remove edges via pointer events
- Tests: `tests/test_wf_subflow.c`, `tests/test_wf_review.c`

**Phase 3 — Versioning, persistence, diff/merge:**
- Seal a Workflow Object to create a named version (immutable `ANX_OBJ_WORKFLOW`)
- Clone a sealed version into a new mutable object (`anx_wf_clone()`)
- Diff two versions: emit a structured object describing added/removed/changed nodes and edges
- Merge: apply a diff to a target version, with conflict detection
- Persistent workflow store: Workflow Objects survive reboots via RFC-0004 memory tier flush
- Tests: `tests/test_wf_version.c`

---

## 16. Directory Structure

```
kernel/core/workflow/
    wf_object.c         — ANX_OBJ_WORKFLOW CRUD, lifecycle state machine
    wf_graph.c          — graph validation, topological sort, port type checking
    wf_cell.c           — ANX_CELL_WORKFLOW scheduler and coordination loop
    wf_exec.c           — per-node-kind execution dispatch
    wf_review.c         — HUMAN_REVIEW: surface creation and event wait
    wf_canvas.c         — static canvas renderer for visual editor
    wf_credential.c     — credential inheritance and agent scope containment
kernel/include/anx/
    workflow.h          — public Workflow API and type definitions
    wf_node.h           — node and edge struct definitions, enum anx_wf_node_kind
    wf_policy.h         — policy struct, capability defines
kernel/core/tools/
    workflow.c          — shell 'workflow' builtin
tests/
    test_wf_object.c
    test_wf_graph.c
    test_wf_exec.c
    test_wf_subflow.c
    test_wf_review.c
    test_wf_version.c
```

---

## Appendix A: Example — Agent Provisions a Nightly Synthesis Workflow

```
; CEXL cell that creates a nightly document synthesis workflow
(cell workflow-provision
  :credentials ["wf/nightly-synthesis/run"
                "wf/nightly-synthesis/edit"]
  :capabilities [(wf-cap nightly-synthesis :flags (run edit))]

  (let [wf (workflow create "nightly-synthesis"
              :desc "Nightly RAG synthesis and summary"
              :cred model-api-cred)]

    ; Nodes
    (let [t  (workflow add-node wf :trigger   :label "Nightly 02:00"
                :params {:schedule "0 2 * * *"})
          r  (workflow add-node wf :retrieval :label "Fetch new docs"
                :params {:index-oid knowledge-index :query-template "new since {last_run}" :top-k 20})
          m  (workflow add-node wf :model-call :label "Synthesize"
                :params {:model-id "anx:model/default" :prompt-template "Summarize: {input}"
                         :max-tokens 1024})
          h  (workflow add-node wf :human-review :label "Review"
                :params {:prompt "Approve nightly summary?" :timeout-seconds 3600})
          o  (workflow add-node wf :output :label "Write summary"
                :params {:target-oid summary-state-obj})]

      ; Edges
      (workflow add-edge wf t:out0  r:in0)
      (workflow add-edge wf r:out0  m:in0)
      (workflow add-edge wf m:out0  h:in0)
      (workflow add-edge wf h:out0  o:in0))))
```

---

## Appendix B: Interaction with RFC-0016 (CEXL)

CONDITION and TRANSFORM nodes evaluate CEXL expressions. The expression context exposes:
- `input`: the upstream node's output State Object, deserialized to a CEXL value
- `run`: the current Run Context (node states, run id, start timestamp)
- `wf`: the Workflow Object's name and OID (read-only)

CEXL expressions in nodes are sandboxed: no cell spawn, no object mutation, no network access. They may only compute and return a value. Violations cause the node to fail with `ANX_EPERM`.

TRANSFORM nodes additionally expose `map`, `filter`, and `reduce` as built-in higher-order operations on list-typed inputs. The function body is a CEXL lambda evaluated per element.
