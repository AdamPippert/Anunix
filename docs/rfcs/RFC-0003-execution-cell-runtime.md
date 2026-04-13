# RFC-0003: Execution Cell Runtime

| Field      | Value                                      |
|------------|--------------------------------------------|
| RFC        | 0003                                       |
| Title      | Execution Cell Runtime                     |
| Author     | Adam Pippert                               |
| Status     | Draft                                      |
| Created    | 2026-04-13                                 |
| Updated    | 2026-04-13                                 |
| Depends On | RFC-0001, RFC-0002                         |

---

## Executive Summary

This RFC defines the **Execution Cell Runtime**, the canonical execution abstraction for the AI-native operating system established in RFC-0001 and grounded on the State Object Model in RFC-0002.

The Execution Cell replaces the assumption that the primary unit of system work is a flat process running against passive files. Instead, an Execution Cell is a **policy-bound, provenance-producing, routable unit of work** that can invoke:

- deterministic code
- local models
- remote models
- retrieval systems
- validators
- recursive sub-cells
- network-resident execution resources

The cell is not just a process wrapper. It is the system's core unit for:

- intent declaration
- task decomposition
- routing and scheduling
- execution
- validation
- commit into durable state and memory

This RFC specifies:

- the Execution Cell schema
- lifecycle and state transitions
- decomposition model
- routing hooks
- validation and commit semantics
- retry, rollback, and compensation behavior
- policy and security boundaries
- runtime APIs and CLI
- compatibility mapping to POSIX process execution

The design goal is to preserve the composability of UNIX while making execution fit a world where computation is increasingly **distributed, probabilistic, memory-aware, and model-mediated**.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert / public collaborators
**Depends on:** RFC-0001, RFC-0002
**Blocks:** RFC-0004, RFC-0005, RFC-0006

---

## 2. Problem Statement

Classical operating systems assume that work is performed by processes which:

- consume input
- execute code locally
- produce output
- exit with a status code

That abstraction is too weak for AI-native systems.

Modern workloads frequently require:

- multi-step decomposition
- dynamic routing across local and remote engines
- probabilistic generation
- retrieval-augmented execution
- validation before promotion
- explicit provenance capture
- policy checks before model invocation
- recursive workflows
- network-aware fallback

A single "process" abstraction cannot express these concerns cleanly without pushing them into application-specific orchestration code. The result is fragmented runtime behavior, inconsistent trust handling, poor observability, and weak composability.

The system needs a new first-class execution abstraction.

---

## 3. Goals

### 3.1 Primary Goals

1. **Intent-first execution**
   - Work must be declared in terms of what is being attempted, not only which binary to run.

2. **Composable decomposition**
   - Complex tasks must be splittable into sub-cells with explicit lineage.

3. **Routable execution**
   - The runtime must support execution across deterministic tools, local models, remote models, retrieval engines, and hybrid pipelines.

4. **Validation before promotion**
   - Outputs must be eligible for validation before they are committed into trusted memory or durable system state.

5. **Policy-aware execution**
   - Cells must obey access, execution, export, replication, and trust policies inherited from input State Objects and runtime context.

6. **Provenance by default**
   - Every cell execution must generate traceable provenance.

7. **Network-first compatibility**
   - The runtime must treat remote compute as a normal part of execution, not a special-case bolt-on.

8. **Backward compatibility**
   - Conventional programs and scripts must remain usable inside cells.

### 3.2 Non-Goals

1. Replacing all POSIX processes immediately.
2. Moving model orchestration into kernel space.
3. Requiring every cell to use decomposition or recursion.
4. Forcing every cell output to be persisted as a State Object.
5. Mandating one specific scheduler implementation.

---

## 4. Core Definitions

### 4.1 Execution Cell

An **Execution Cell** is the canonical unit of work in the system. It declares:

- intent
- inputs
- constraints
- routing policy
- validation policy
- commit policy
- execution scope

A cell may directly execute work or may expand into a graph of child cells.

### 4.2 Cell Runtime

The **Cell Runtime** is the service layer responsible for:

- admission control
- policy evaluation
- decomposition
- routing
- scheduling coordination
- execution
- validation dispatch
- state commit
- trace and provenance emission

### 4.3 Cell Plan

A **Cell Plan** is the executable plan produced after decomposition and routing. It specifies:

- substeps
- engine assignments
- fallback paths
- expected outputs
- validation requirements
- commit behavior

### 4.4 Cell Trace

A **Cell Trace** is the append-only execution record generated by a cell. It includes:

- timing
- routing decisions
- tool/model invocations
- input/output references
- validation results
- commit records
- error events

### 4.5 Child Cell

A **Child Cell** is a derived execution unit created by a parent cell during decomposition.

### 4.6 Commit

A **Commit** is the act of materializing cell outputs into durable State Objects, memory structures, or side effects after policy and validation conditions are satisfied.

### 4.7 Compensation

A **Compensation** is an explicit corrective action for partially completed side effects when a cell fails after external mutation.

---

## 5. Design Principles

### 5.1 Execution Must Be Explicit
The runtime should not hide decomposition, routing, or validation decisions.

### 5.2 Intent and Mechanism Must Be Separable
The cell should declare the task; the runtime should decide the best eligible execution path.

### 5.3 Side Effects Must Be Controlled
No irreversible side effect should occur without a defined commit or compensation rule.

### 5.4 Probabilistic Outputs Must Be Labeled
Model-produced outputs must carry confidence and validation state where applicable.

### 5.5 Cells Must Be Inspectable
A human or system agent must be able to inspect why a cell ran the way it did.

### 5.6 Decomposition Must Be Bounded
Recursive expansion is allowed, but uncontrolled fan-out is forbidden.

### 5.7 Network Is Native
Remote execution and remote memory access must be treated as regular execution surfaces.

---

## 6. Execution Cell Schema

### 6.1 Canonical Logical Schema

```json
{
  "id": "cell_01JRXYZ...",
  "cell_type": "task.execution",
  "schema_version": "1.0",
  "revision": 1,
  "created_at": "2026-04-13T18:00:00Z",
  "updated_at": "2026-04-13T18:00:00Z",
  "status": "created",
  "intent": {},
  "inputs": [],
  "constraints": {},
  "routing_policy": {},
  "validation_policy": {},
  "commit_policy": {},
  "execution_policy": {},
  "runtime": {},
  "parent_cell_ref": null,
  "child_cell_refs": [],
  "plan_ref": null,
  "trace_ref": null,
  "output_refs": [],
  "error": null,
  "ext": {}
}
```

### 6.2 Field Definitions

| Field | Required | Description |
|---|---:|---|
| `id` | Yes | Stable cell identifier |
| `cell_type` | Yes | Cell classification |
| `schema_version` | Yes | Envelope schema version |
| `revision` | Yes | Revision token |
| `created_at` | Yes | Creation time |
| `updated_at` | Yes | Last mutation time |
| `status` | Yes | Lifecycle status |
| `intent` | Yes | Declared objective and expected result class |
| `inputs` | Yes | Input references and binding information |
| `constraints` | Yes | Hard requirements such as latency, cost, privacy, locality |
| `routing_policy` | Yes | Preferred routing strategy and allowed engines |
| `validation_policy` | Yes | Validation requirements before commit/promotion |
| `commit_policy` | Yes | Persistence, side-effect, and memory promotion rules |
| `execution_policy` | Yes | Security and execution restrictions |
| `runtime` | Yes | Runtime state, resource bindings, and metrics |
| `parent_cell_ref` | No | Parent cell lineage reference |
| `child_cell_refs` | Yes | Child cells created during decomposition |
| `plan_ref` | No | Reference to execution plan |
| `trace_ref` | No | Reference to execution trace |
| `output_refs` | Yes | Output State Objects or side-effect receipts |
| `error` | No | Terminal or current error record |
| `ext` | No | Reserved extension map |

### 6.3 Example Cell

```json
{
  "id": "cell_01JRCELL001",
  "cell_type": "task.execution",
  "schema_version": "1.0",
  "revision": 1,
  "created_at": "2026-04-13T18:00:00Z",
  "updated_at": "2026-04-13T18:00:00Z",
  "status": "planned",
  "intent": {
    "name": "summarize_customer_meeting",
    "objective": "Produce a validated summary and action item list from transcript",
    "requested_outputs": [
      "memory.summary",
      "task.result"
    ]
  },
  "inputs": [
    {
      "name": "transcript",
      "state_object_ref": "so_01TRANSCRIPT123",
      "required": true
    }
  ],
  "constraints": {
    "max_latency_ms": 12000,
    "max_cost_usd": 0.04,
    "privacy_scope": "private",
    "allow_remote_execution": false
  },
  "routing_policy": {
    "strategy": "local_first",
    "allowed_engines": [
      "local_model",
      "deterministic_tool"
    ]
  },
  "validation_policy": {
    "required": true,
    "minimum_validation_state": "provisional"
  },
  "commit_policy": {
    "persist_outputs": true,
    "promote_to_memory": true
  },
  "execution_policy": {
    "allow_network_access": false,
    "allow_recursive_cells": true
  },
  "runtime": {
    "attempt_count": 0
  },
  "child_cell_refs": [],
  "output_refs": []
}
```

---

## 7. Cell Types

### 7.1 Initial Cell Type Families

#### Direct Execution Cells
- `task.execution`
- `task.batch_execution`
- `task.stream_execution`

#### Planning and Decomposition Cells
- `task.plan_generation`
- `task.decomposition`

#### Retrieval and Memory Cells
- `task.retrieval`
- `task.memory_update`
- `task.graph_update`

#### Validation Cells
- `task.validation`
- `task.consistency_check`
- `task.policy_check`

#### Action Cells
- `task.side_effect`
- `task.external_call`

#### Control Cells
- `task.router`
- `task.scheduler_binding`
- `task.compensation`

### 7.2 Type Registry

As with State Objects, cell types must be registered in a versioned registry containing:

- type name
- required fields
- allowed lifecycle transitions
- expected input classes
- expected output classes
- default validation behavior
- allowed engine classes

---

## 8. Intent Model

### 8.1 Purpose

Intent captures the "what" of the cell rather than just the mechanism.

Example:

```json
"intent": {
  "name": "extract_action_items",
  "objective": "Identify and structure action items from a validated meeting transcript",
  "requested_outputs": [
    "task.result"
  ],
  "success_condition": {
    "kind": "schema_match",
    "schema": "action_item_list_v1"
  }
}
```

### 8.2 Intent Rules

1. Intent must be machine-readable.
2. Intent must not over-specify engine choice unless required by policy.
3. Intent should include requested outputs and success conditions.
4. Intent may include quality targets and preferred strategies.

### 8.3 Intent Components

Recommended fields:

- `name`
- `objective`
- `requested_outputs`
- `success_condition`
- `quality_targets`
- `human_visibility`
- `priority`

---

## 9. Inputs and Bindings

### 9.1 Input Sources

A cell may accept inputs from:

- State Objects
- literal inline parameters
- stream bindings
- previous cell outputs
- remote references
- device interfaces
- environment bindings

### 9.2 Input Schema

```json
"inputs": [
  {
    "name": "source_document",
    "state_object_ref": "so_01ABC...",
    "representation_preference": "raw_text",
    "required": true,
    "access_mode": "read"
  },
  {
    "name": "temperature",
    "value": 0.2,
    "required": false
  }
]
```

### 9.3 Input Rules

1. All object-backed inputs must reference stable IDs.
2. Preferred representations may be declared but are not guaranteed.
3. Input access modes must be explicit:
   - `read`
   - `read_write`
   - `append`
4. Sensitive inputs inherit policy constraints into the cell.

---

## 10. Constraints

### 10.1 Purpose

Constraints define hard and soft execution boundaries.

Example:

```json
"constraints": {
  "max_latency_ms": 5000,
  "max_cost_usd": 0.01,
  "privacy_scope": "private",
  "energy_profile": "balanced",
  "locality": "prefer_local",
  "minimum_confidence": 0.75
}
```

### 10.2 Constraint Classes

#### Resource Constraints
- latency
- cost
- memory
- accelerator use
- network budget

#### Policy Constraints
- privacy scope
- export rules
- data residency
- model class allowlist

#### Quality Constraints
- minimum confidence
- required validation level
- reproducibility requirements

#### Topology Constraints
- local-only
- edge-preferred
- remote-allowed
- offline-capable

---

## 11. Routing Policy

### 11.1 Purpose

Routing policy constrains how the routing engine may plan execution.

Example:

```json
"routing_policy": {
  "strategy": "local_first",
  "allowed_engines": [
    "deterministic_tool",
    "local_model",
    "retrieval_service"
  ],
  "disallowed_engines": [
    "remote_model"
  ],
  "decomposition_mode": "adaptive",
  "max_child_cells": 8,
  "fallback_order": [
    "deterministic_tool",
    "local_model"
  ]
}
```

### 11.2 Routing Strategy Values

Initial strategies:

- `direct`
- `local_first`
- `cost_first`
- `latency_first`
- `confidence_first`
- `adaptive`
- `policy_locked`

### 11.3 Decomposition Modes

- `none`
- `static`
- `adaptive`
- `recursive`

### 11.4 Routing Rules

1. Routing policy constrains but does not fully encode the runtime plan.
2. Routing cannot violate execution policy or input policy inheritance.
3. Max child cell count must be enforced.
4. Recursive decomposition must have a depth bound.

---

## 12. Validation Policy

### 12.1 Purpose

Validation policy defines what must happen before a cell is considered successful and before outputs may be promoted or acted upon.

Example:

```json
"validation_policy": {
  "required": true,
  "validators": [
    "schema_check",
    "source_crosscheck",
    "consistency_pass"
  ],
  "minimum_validation_state": "provisional",
  "block_commit_on_failure": true,
  "allow_partial_commit": false
}
```

### 12.2 Validation Modes

- `none`
- `schema_only`
- `light`
- `strict`
- `domain_specific`

### 12.3 Validation Rules

1. External side-effect cells should default to strict validation where feasible.
2. Probabilistic outputs should not be auto-promoted to trusted memory without validation.
3. Validation results must be written to trace and output State Objects.

---

## 13. Commit Policy

### 13.1 Purpose

Commit policy determines what becomes durable and under what conditions.

Example:

```json
"commit_policy": {
  "persist_outputs": true,
  "promote_to_memory": true,
  "promote_to_graph": false,
  "allow_side_effects": false,
  "write_trace": true,
  "cleanup_ephemeral_artifacts": true
}
```

### 13.2 Commit Classes

#### Durable Object Commit
Creates or updates State Objects.

#### Memory Promotion
Adds outputs into taxonomy, graph, or retrieval indexes.

#### External Side Effect
Applies changes outside the runtime boundary.

#### Trace Commit
Persists execution trace and provenance artifacts.

### 13.3 Commit Rules

1. Commit should be delayed until validation requirements are satisfied.
2. External side effects must be explicitly declared.
3. Partial commit must be opt-in, not default.
4. Trace persistence should default to enabled.

---

## 14. Execution Policy

### 14.1 Purpose

Execution policy governs security and admissibility of runtime behaviors.

Example:

```json
"execution_policy": {
  "allow_network_access": true,
  "allow_remote_models": false,
  "allow_recursive_cells": true,
  "allow_external_side_effects": false,
  "sandbox_profile": "strict",
  "max_recursion_depth": 3
}
```

### 14.2 Execution Policy Areas

- network access
- remote model use
- filesystem access
- device access
- recursion
- side effects
- exportability
- sandbox profile
- credential scope

---

## 15. Runtime State

### 15.1 Runtime Section

The runtime section captures mutable live execution state.

Example:

```json
"runtime": {
  "attempt_count": 1,
  "current_engine": "local_model:qwen3-8b",
  "scheduler_binding": {
    "queue": "interactive",
    "cpu_shares": 200,
    "gpu_fraction": 0.25
  },
  "metrics": {
    "start_at": "2026-04-13T18:00:02Z",
    "planned_at": "2026-04-13T18:00:01Z"
  }
}
```

### 15.2 Runtime Rules

1. Runtime state is mutable and operational.
2. Durable semantic results should not live only in runtime.
3. Runtime state must be separated from trace for clarity.
4. Runtime state may be ephemeral after completion if trace is persisted.

---

## 16. Lifecycle

### 16.1 Cell Status Values

Initial allowed statuses:

- `created`
- `admitted`
- `planning`
- `planned`
- `queued`
- `running`
- `waiting`
- `validating`
- `committing`
- `completed`
- `failed`
- `cancelled`
- `compensating`
- `compensated`

### 16.2 Lifecycle Flow

Typical path:

```text
created -> admitted -> planning -> planned -> queued -> running
                                                -> waiting
running -> validating -> committing -> completed
running -> failed
committing -> compensating -> compensated
```

### 16.3 Lifecycle Semantics

#### created
Cell envelope exists, not yet admitted.

#### admitted
Policy checks passed; eligible for planning.

#### planning
Decomposition and routing are active.

#### planned
Plan generated and bound.

#### queued
Waiting for resources or dependencies.

#### running
Execution in progress.

#### waiting
Blocked on child cells, streams, remote responses, or retry backoff.

#### validating
Output validation in progress.

#### committing
Durable writes and side effects are in progress.

#### completed
Execution and commit successful.

#### failed
Terminal failure with no remaining retry path.

#### compensating
Runtime is attempting corrective action after partial side effects.

#### compensated
Compensation completed.

---

## 17. Decomposition Model

### 17.1 Purpose

Decomposition enables a cell to turn one intent into smaller execution units.

Example decomposition:

```text
summarize_and_extract_actions
  -> retrieve_context
  -> summarize
  -> extract_actions
  -> validate_schema
  -> commit_outputs
```

### 17.2 Decomposition Rules

1. Decomposition must produce explicit child cell lineage.
2. The parent cell remains accountable for final success conditions.
3. Child cells inherit constraints and policies unless overridden by stricter rules.
4. Recursive decomposition must stop at bounded depth and bounded fan-out.

### 17.3 Decomposition Triggers

A cell may decompose when:

- task complexity exceeds direct execution profile
- routing policy prefers specialization
- validation requires independent passes
- memory retrieval is required
- uncertainty exceeds threshold
- side effects require staged commit

### 17.4 Parent and Child Responsibilities

#### Parent Cell
- defines top-level success
- allocates budget
- aggregates outputs
- decides final commit

#### Child Cells
- perform bounded subwork
- emit local outputs and traces
- return status and artifacts to parent

---

## 18. Planning and Routing Hooks

### 18.1 Planning Pipeline

The runtime planning pipeline should follow:

```text
admission
  -> policy evaluation
  -> input inspection
  -> decomposition decision
  -> engine routing
  -> fallback selection
  -> scheduler binding
```

### 18.2 Routing Inputs

Routing may consider:

- task intent
- input object types
- input policy restrictions
- current resource availability
- model capability graph
- latency and cost budgets
- historical performance
- locality and network conditions

### 18.3 Plan Structure

```json
{
  "id": "plan_01JR...",
  "cell_ref": "cell_01JR...",
  "steps": [
    {
      "step_id": "step_retrieve",
      "kind": "child_cell",
      "cell_type": "task.retrieval",
      "assigned_engine": "retrieval_service:local_semantic",
      "fallback_engines": []
    },
    {
      "step_id": "step_summarize",
      "kind": "child_cell",
      "cell_type": "task.execution",
      "assigned_engine": "local_model:qwen3-8b",
      "fallback_engines": [
        "local_model:small_fast",
        "remote_model:trusted_high_quality"
      ]
    }
  ]
}
```

### 18.4 Plan Rules

1. Plans must be materialized as inspectable artifacts.
2. Fallback paths must be explicit where allowed.
3. Plans may be revised during execution only within policy bounds.
4. Significant replanning events must be recorded in trace.

---

## 19. Execution Semantics

### 19.1 Direct Execution

A direct cell may run without decomposition when:

- one eligible engine satisfies policy and constraints
- output expectations are simple
- validation is straightforward
- no recursive orchestration is needed

### 19.2 Multi-Step Execution

A multi-step cell may:

- spawn child cells
- wait on streamed outputs
- merge subresults
- validate merged outputs
- commit aggregate results

### 19.3 Streaming Execution

For token or event streams, a cell may expose partial outputs before completion. These outputs must be marked as:

- provisional
- non-committed
- subject to correction

### 19.4 Side-Effect Execution

Cells that mutate external systems must:

1. declare side effects in advance
2. record receipts or acknowledgements
3. define compensation or rollback strategy

---

## 20. Validation Semantics

### 20.1 Validation Pipeline

The default validation flow is:

```text
output produced
  -> schema or type validation
  -> policy validation
  -> content validation
  -> trust scoring
  -> promotion eligibility
```

### 20.2 Validator Classes

- schema validator
- deterministic checker
- cross-source checker
- model-based critic
- policy validator
- consistency validator
- contradiction detector

### 20.3 Validation Outcomes

- `pass`
- `pass_with_warnings`
- `retryable_fail`
- `terminal_fail`
- `escalate`

### 20.4 Validation Rules

1. Validation may produce child validation cells.
2. Validation failure may trigger retry or fallback.
3. Validation results must be linked to output State Objects and cell trace.
4. Promotion into durable memory should depend on validation state.

---

## 21. Commit Semantics

### 21.1 Commit Phases

The runtime should conceptually separate commit into:

1. **prepare**
   - reserve object IDs
   - stage payloads
   - verify policies

2. **apply**
   - write State Objects
   - update memory/indexes
   - emit relations

3. **finalize**
   - mark commit complete
   - attach trace refs
   - release ephemeral staging state

### 21.2 Durable Output Rules

A completed cell should emit durable outputs when:

- output is requested by intent
- validation policy permits commit
- commit policy requires persistence

Outputs should normally be created as new State Objects rather than in-place mutation of unrelated objects.

### 21.3 External Side Effects

External actions should follow a staged model:

```text
validate intent
  -> prepare action payload
  -> perform side effect
  -> record receipt
  -> finalize commit
```

### 21.4 Partial Commit

Partial commit is allowed only when explicitly declared. Example use cases:

- streaming logs
- long-running batch checkpoints
- append-only traces

---

## 22. Failure Handling

### 22.1 Failure Classes

#### Admission Failures
- policy violation
- missing credentials
- forbidden engine class

#### Planning Failures
- no valid route
- decomposition overflow
- invalid input type

#### Execution Failures
- tool crash
- model timeout
- network failure
- invalid intermediate output

#### Validation Failures
- schema mismatch
- contradiction
- insufficient confidence

#### Commit Failures
- storage error
- policy block
- side-effect receipt missing

### 22.2 Retry Policy

Retry behavior should be policy-driven and bounded.

Suggested fields:

```json
"retry_policy": {
  "max_attempts": 3,
  "backoff_mode": "exponential",
  "retry_on": [
    "timeout",
    "network_failure",
    "retryable_validation_fail"
  ]
}
```

Retry policy may live under `execution_policy` or a future dedicated field.

### 22.3 Failure Rules

1. Retrying identical failing plans without adjustment is discouraged.
2. Retry should prefer revised routing or fallback paths where possible.
3. Terminal failure must preserve trace and staged outputs for inspection unless forbidden by policy.

---

## 23. Rollback and Compensation

### 23.1 Rollback

Rollback applies when the runtime can safely discard staged but uncommitted internal state.

Examples:
- staged object envelopes
- temp blobs
- queued child cells not yet executed

Rollback is preferred for internal operations.

### 23.2 Compensation

Compensation applies when irreversible or external side effects already occurred.

Examples:
- remote API mutation
- message dispatch
- device control action
- graph update after downstream failure

Compensation must be explicit, best-effort, and traceable.

### 23.3 Compensation Cell

Compensation should itself be represented as a cell:

- `task.compensation`

This keeps corrective action observable and composable.

### 23.4 Rollback and Compensation Rules

1. Not all side effects are truly reversible.
2. The system must distinguish:
   - staged internal rollback
   - committed internal reversal
   - external compensation
3. Compensation failure must itself be traceable and may require escalation.

---

## 24. Provenance and Trace

### 24.1 Trace Requirements

Every cell must emit a trace containing:

- cell ID
- parent cell ID
- plan ID
- engine selections
- timing
- retries
- validator results
- output references
- failure events
- side-effect receipts
- compensation records

### 24.2 Trace as State Object

The cell trace should be materialized as one or more State Objects, typically:

- `trace.execution`
- `trace.validation`

### 24.3 Provenance Rules

Outputs created by a cell must include provenance pointing back to:

- the creating cell
- source object refs
- transformation or engine identity
- relevant plan or trace refs

---

## 25. Scheduler Integration

### 25.1 Scheduler Responsibilities

The scheduler is responsible for resource allocation, while the cell runtime is responsible for semantic execution orchestration.

The two should interact through binding hints and resource contracts.

### 25.2 Scheduler Binding Fields

Example:

```json
"runtime": {
  "scheduler_binding": {
    "queue": "interactive",
    "priority": "high",
    "cpu_shares": 300,
    "gpu_fraction": 0.4,
    "network_budget_class": "moderate"
  }
}
```

### 25.3 Scheduling Dimensions

- CPU
- GPU / NPU
- memory tier access
- network path quality
- device access
- remote inference slot availability

---

## 26. Network-First Behavior

### 26.1 Principle

The runtime must treat remote compute and remote memory as normal execution surfaces.

### 26.2 Network Execution Modes

- `offline_only`
- `local_first`
- `edge_preferred`
- `remote_allowed`
- `remote_required`

### 26.3 Degraded Operation

When remote resources are unavailable, cells should:

1. use local fallback when policy allows
2. lower quality within declared bounds
3. enter waiting state when correctness requires remote access
4. fail explicitly when no valid path exists

Silent substitution is discouraged.

---

## 27. Security Model

### 27.1 Baseline Requirements

1. Cells must execute within a sandbox profile.
2. Input object policies must be enforced before engine access.
3. Remote execution requires explicit policy permission.
4. Sensitive outputs must inherit restrictive policy defaults.

### 27.2 Execution Boundaries

Cells should run with scoped credentials and explicit capabilities for:

- filesystem access
- network access
- model endpoint access
- device access
- state store mutation
- memory promotion

### 27.3 Policy Inheritance

A child cell inherits the stricter of:

- parent execution policy
- input object execution policies
- system-level governance rules

---

## 28. POSIX Compatibility

### 28.1 Compatibility Thesis

The Execution Cell does not eliminate processes; it generalizes them.

### 28.2 Mapping

| POSIX Concept | Execution Cell Runtime Equivalent |
|---|---|
| Process | Direct execution cell |
| Fork/exec | Child cell creation with bound engine |
| Exit code | Terminal status + error object |
| stdout/stderr | Stream output or trace artifact |
| Environment variables | Runtime bindings |
| Pipe | Stream binding between cells |

### 28.3 Compatibility Modes

#### Mode A: Process-Wrapped Cells
A conventional process runs inside a cell envelope with trace, policy, and commit surfaces.

#### Mode B: Native Cells
Cell is primary abstraction and may execute without a traditional process boundary.

The initial prototype should prioritize Mode A.

---

## 29. APIs

### 29.1 Create Cell

```http
POST /cells
```

Request:

```json
{
  "cell_type": "task.execution",
  "intent": {
    "name": "summarize_document",
    "objective": "Generate a summary"
  },
  "inputs": [
    {
      "name": "document",
      "state_object_ref": "so_01ABC..."
    }
  ],
  "constraints": {
    "max_latency_ms": 3000
  }
}
```

Response:

```json
{
  "id": "cell_01JR...",
  "status": "created"
}
```

### 29.2 Admit and Run Cell

```http
POST /cells/{id}/run
```

### 29.3 Get Cell

```http
GET /cells/{id}
```

### 29.4 Get Plan

```http
GET /cells/{id}/plan
```

### 29.5 Get Trace

```http
GET /cells/{id}/trace
```

### 29.6 Cancel Cell

```http
POST /cells/{id}/cancel
```

### 29.7 Retry Cell

```http
POST /cells/{id}/retry
```

Optional request body may override retry constraints or fallback behavior within policy bounds.

### 29.8 Derive Child Cell

```http
POST /cells/{id}/children
```

### 29.9 Validate Outputs

```http
POST /cells/{id}/validate
```

### 29.10 Commit Outputs

```http
POST /cells/{id}/commit
```

---

## 30. CLI Surface

Suggested initial CLI:

```bash
cell create --type task.execution --intent summarize_document --input so_01ABC...
cell run cell_01JR...
cell status cell_01JR...
cell plan cell_01JR...
cell trace cell_01JR...
cell retry cell_01JR...
cell cancel cell_01JR...
cell outputs cell_01JR...
```

Advanced example:

```bash
cell create \
  --type task.execution \
  --intent extract_action_items \
  --input so_01TRANSCRIPT \
  --constraint max_latency_ms=8000 \
  --routing local_first \
  --validate strict
```

---

## 31. Reference Prototype Architecture

### 31.1 Initial Userland Components

1. **Cell API Service**
   - cell CRUD and orchestration

2. **Planner / Router**
   - decomposition and engine assignment

3. **Executor Adapters**
   - process adapter
   - local model adapter
   - remote model adapter
   - retrieval adapter

4. **Validator Service**
   - schema and trust checks

5. **Commit Service**
   - output persistence and memory integration

6. **Trace Service**
   - append-only event recording

### 31.2 Recommended Initial Stack

- Python for orchestration and API
- PostgreSQL for cell envelopes and plans
- blob storage on local filesystem or S3-compatible layer
- message queue for async execution
- HTMX-based control plane UI if a lightweight dashboard is useful
- local model runner and deterministic tool adapters as first targets

This remains consistent with a low-cost, open-source-first strategy.

---

## 32. Observability

### 32.1 Minimum Metrics

Per cell:

- queue time
- planning time
- execution time
- validation time
- commit time
- total latency
- retry count
- route selected
- fallback frequency
- output validation state
- cost estimate

### 32.2 Debug Surfaces

The system should support:

- plan inspection
- trace replay
- route comparison
- validation failure inspection
- provenance graph inspection

---

## 33. Failure Modes

### 33.1 Decomposition Explosion
Too many child cells create overhead and loss of tractability.

**Mitigation:** depth and fan-out caps, budget-aware planner.

### 33.2 Hidden Routing Drift
Routing changes silently over time and harms reproducibility.

**Mitigation:** plan materialization and route versioning.

### 33.3 Commit Without Validation
Probabilistic results enter trusted memory prematurely.

**Mitigation:** validation gates in commit service.

### 33.4 Side-Effect Inconsistency
External actions succeed while internal commit fails.

**Mitigation:** staged commit and compensation cells.

### 33.5 Policy Bypass
Adapters invoke engines without inherited restrictions.

**Mitigation:** centralized policy evaluation and adapter capability checks.

### 33.6 Trace Gaps
Critical decisions are not recorded.

**Mitigation:** append-only trace requirements at each lifecycle transition.

---

## 34. Implementation Plan

### Phase 1: Direct Cells Over Existing Processes
- wrap normal processes in cell envelopes
- emit trace and output State Objects
- support local deterministic tools only

### Phase 2: Local Model and Retrieval Adapters
- add local model adapter
- add retrieval cell type
- add strict validation and commit service

### Phase 3: Child Cells and Adaptive Decomposition
- parent/child cell lineage
- planning service
- route and fallback policies

### Phase 4: Remote and Network-First Execution
- remote model adapters
- network-aware routing
- degraded/offline behavior

### Phase 5: Side-Effect Safety
- compensation cells
- staged side-effect commit
- stronger security and credential scoping

---

## 35. Open Questions

1. Should every child step be a child cell, or should some remain intra-cell steps for efficiency?
2. What is the right default retry policy for model outputs that fail validation?
3. Which validation failures should trigger re-routing versus escalation?
4. How much of the plan should be fixed before execution versus adapted during execution?
5. Should streaming outputs be represented as separate stream objects or attached to trace by default?
6. How should cost accounting be normalized across local and remote engines?
7. When should the runtime persist intermediate artifacts versus leaving them ephemeral?

---

## 36. Decision Summary

This RFC makes the following decisions:

1. The Execution Cell is the canonical unit of work.
2. Cells declare intent, inputs, constraints, routing policy, validation policy, commit policy, and execution policy.
3. Cells may decompose into bounded child cells.
4. Routing and planning are explicit, inspectable runtime stages.
5. Validation is a first-class stage before durable commit and memory promotion.
6. Side effects require declared commit behavior and compensation planning.
7. Every cell emits trace and provenance by default.
8. Initial compatibility will wrap conventional processes inside cell envelopes.

---

## 37. Conclusion

The Execution Cell Runtime is the missing operational layer between the State Object Model and the higher-level memory and routing architecture.

It gives the system a principled way to execute work that is:

- composable
- observable
- policy-aware
- validation-gated
- network-native
- compatible with both deterministic code and model-mediated computation

This is the right execution abstraction for a system where:

- data is structured state
- memory is active
- routing is first-class
- trust must be explicit
- remote compute is normal
