# RFC-0005: Routing Plane and Unified Scheduler

| Field      | Value                                      |
|------------|--------------------------------------------|
| RFC        | 0005                                       |
| Title      | Routing Plane and Unified Scheduler        |
| Author     | Adam Pippert                               |
| Status     | Draft                                      |
| Created    | 2026-04-13                                 |
| Updated    | 2026-04-13                                 |
| Depends On | RFC-0001, RFC-0002, RFC-0003, RFC-0004     |

---

## Executive Summary

This RFC defines the **Routing Plane and Unified Scheduler**, the subsystem responsible for deciding **how** work should be executed and **where** it should run in an AI-native operating system.

In a classical operating system, scheduling is primarily concerned with:

- CPU time
- memory allocation
- I/O waiting
- process priority

That is necessary but no longer sufficient.

In an AI-native system, every non-trivial execution request may require a decision across multiple heterogeneous execution surfaces:

- deterministic tools
- local models
- remote models
- retrieval services
- validators
- graph traversals
- network-resident execution resources

The system therefore needs two tightly connected but distinct mechanisms:

1. a **Routing Plane** that decides the appropriate execution path for a task
2. a **Unified Scheduler** that binds the selected route to available resources and budgets

This RFC specifies:

- engine and capability models
- route planning and selection
- routing policies and constraints
- scheduler contracts and queue classes
- local-first and network-first behavior
- cost, latency, confidence, and locality tradeoffs
- degraded/offline behavior
- feedback loops and route learning
- APIs and CLI
- reference implementation guidance

The design goal is to preserve operational clarity while enabling the system to route work at the right level of abstraction instead of assuming that all tasks should run as flat local processes.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert / public collaborators
**Depends on:** RFC-0001, RFC-0002, RFC-0003, RFC-0004
**Blocks:** RFC-0006, RFC-0007, RFC-0008

---

## 2. Problem Statement

The existing scheduling model in mainstream operating systems assumes that work is fundamentally local and mostly homogeneous. Even when remote systems are involved, the operating system largely treats them as:

- sockets
- filesystems
- devices
- userland concerns

That is no longer adequate.

In an AI-native environment, system execution may require choices such as:

- exact tool vs local model
- small local model vs larger local model
- local model vs remote model
- lexical retrieval vs semantic retrieval vs graph traversal
- single-pass execution vs recursive decomposition
- wait for network vs run degraded local fallback

These choices are not merely performance optimizations. They affect:

- correctness
- trust
- cost
- privacy
- latency
- reproducibility
- user experience

Without a first-class routing plane and unified scheduler, these decisions become scattered across applications, causing:

- route inconsistency
- policy drift
- cost overruns
- unpredictable latency
- hidden remote dependencies
- weak observability
- poor degraded behavior

The system needs a single architectural layer that makes routing and scheduling explicit.

---

## 3. Goals

### 3.1 Primary Goals

1. **Capability-aware routing**
   - Route tasks based on engine capabilities, not vendor names or hardcoded assumptions.

2. **Unified scheduling**
   - Allocate resources across local compute, accelerators, memory tiers, and network-backed execution surfaces.

3. **Policy-first planning**
   - Enforce privacy, locality, export, and trust constraints before route scoring.

4. **Multi-objective optimization**
   - Balance cost, latency, confidence, locality, and energy.

5. **Network-aware behavior**
   - Treat remote execution as native while preserving graceful degradation.

6. **Decomposition-aware planning**
   - Route not only whole tasks but also task fragments and child cells.

7. **Traceability**
   - Record why a route was chosen and how scheduling decisions were made.

8. **Feedback-driven improvement**
   - Learn from route outcomes over time without making the system opaque.

### 3.2 Non-Goals

1. Hardwiring all routing to one model provider.
2. Building a black-box reinforcement learner as the first implementation.
3. Eliminating deterministic tools in favor of models.
4. Replacing kernel CPU scheduling directly in the first prototype.
5. Guaranteeing globally optimal routes for all tasks.

---

## 4. Core Definitions

### 4.1 Routing Plane

The **Routing Plane** evaluates execution intent, constraints, available engines, memory locality, and policy to choose one or more candidate execution plans.

### 4.2 Unified Scheduler

The **Unified Scheduler** binds routed plans to resources such as:

- CPU
- GPU / NPU
- memory tiers
- queue classes
- network budgets
- remote inference slots

### 4.3 Engine

An **Engine** is any execution surface capable of completing part of a task, including:

- deterministic binaries
- scripts
- local models
- remote models
- retrieval services
- validators
- graph traversers
- remote execution backends

### 4.4 Capability Registry

The **Capability Registry** is the machine-readable inventory of known engines, their properties, and their eligibility rules.

### 4.5 Route Plan

A **Route Plan** is the selected execution path for a cell or subtask, including engine assignments, fallbacks, and budget bindings.

### 4.6 Scheduler Binding

A **Scheduler Binding** is the concrete resource allocation associated with a route plan.

### 4.7 Budget Profile

A **Budget Profile** is the declared or inferred limit for cost, latency, energy, or remote usage.

### 4.8 Locality

**Locality** refers to how close data, compute, and memory are to one another in execution terms, including:

- process-local
- machine-local
- LAN/edge-local
- remote/cloud

---

## 5. Design Principles

### 5.1 Route by Capability, Not Branding
Routing should be based on what an engine can reliably do.

### 5.2 Feasibility First, Scoring Second
Ineligible engines should be filtered before optimization begins.

### 5.3 Policy Beats Optimization
A cheaper or faster engine must not be selected if policy forbids it.

### 5.4 Locality Matters
Moving compute to data or data to compute should be an explicit tradeoff.

### 5.5 Routing and Scheduling Are Distinct but Coupled
Routing decides what should happen; scheduling decides how resources are allocated to make it happen.

### 5.6 Degraded Behavior Must Be Designed
Offline or network-impaired operation must be a first-class routing outcome.

### 5.7 Learning Must Remain Inspectable
Feedback loops should improve route quality without turning the system into an opaque black box.

---

## 6. Architectural Overview

The end-to-end decision flow is:

```text
Execution Cell
  -> policy filter
  -> capability filter
  -> decomposition decision
  -> candidate route generation
  -> route scoring
  -> scheduler binding
  -> execution
  -> outcome feedback
```

The Routing Plane and Unified Scheduler sit between the Execution Cell Runtime and the available engines.

### 6.1 Control Boundaries

- **Execution Cell Runtime** owns intent, lineage, validation, and commit semantics
- **Routing Plane** owns candidate plan generation and selection
- **Unified Scheduler** owns resource binding and queueing
- **Memory Control Plane** influences locality and retrieval strategy
- **Policy Layer** constrains every decision

---

## 7. Engine Classes

### 7.1 Canonical Engine Classes

Initial engine classes are:

- `deterministic_tool`
- `local_model`
- `remote_model`
- `retrieval_service`
- `graph_service`
- `validation_service`
- `execution_service`
- `device_service`

### 7.2 Engine Examples

#### deterministic_tool
- parser, compiler, regex extractor, symbolic checker, OCR pipeline

#### local_model
- small local instruct model, medium local reasoning model, local embedding model, local reranker

#### remote_model
- high-capability external reasoning model, remote embedding endpoint, remote multimodal model

#### retrieval_service
- lexical search, semantic search, late-interaction retrieval, hybrid rerank pipeline

#### graph_service
- neighborhood traversal, contradiction sweep, taxonomy-aware expansion

#### validation_service
- schema checker, cross-source verifier, model-based critic, policy validator

### 7.3 Engine Identity

Each engine must have a stable registry identity such as:

```text
eng_local_qwen3_8b
eng_remote_reasoner_x
eng_tool_tantivy_search
eng_graph_local_adj
```

---

## 8. Capability Registry

### 8.1 Purpose

The Capability Registry enables the routing plane to reason about what engines can and cannot do.

### 8.2 Registry Schema

```json
{
  "engine_id": "eng_local_qwen3_8b",
  "engine_class": "local_model",
  "status": "available",
  "capabilities": [
    "summarization",
    "classification",
    "structured_extraction"
  ],
  "constraints": {
    "supports_private_data": true,
    "requires_network": false,
    "max_context_tokens": 65536,
    "supports_streaming": true
  },
  "cost_model": {
    "kind": "local_estimate",
    "cpu_weight": 0.2,
    "gpu_weight": 0.8
  },
  "quality_profile": {
    "strengths": [
      "cheap",
      "local",
      "good_for_extraction"
    ],
    "weaknesses": [
      "weaker_long_chain_reasoning"
    ]
  }
}
```

### 8.3 Required Registry Fields

- `engine_id`
- `engine_class`
- `status`
- `capabilities`
- `constraints`
- `cost_model`
- `quality_profile`
- `policy_tags`
- `locality_class`

### 8.4 Capability Tags

Initial capability tags may include:

- `summarization`
- `question_answering`
- `structured_extraction`
- `long_context_reasoning`
- `semantic_retrieval`
- `lexical_retrieval`
- `graph_traversal`
- `schema_validation`
- `contradiction_detection`
- `tool_execution`
- `multimodal_input`

### 8.5 Registry Rules

1. Capabilities must be explicit, not implied.
2. Engines may advertise strengths and weaknesses.
3. Status must be dynamic:
   - `available`
   - `degraded`
   - `offline`
   - `maintenance`
4. Route planning must not assume availability without checking the registry and runtime signals.

---

## 9. Routing Inputs

### 9.1 Route Inputs

Routing decisions may use:

- cell intent
- input object types
- input policies
- validation requirements
- budget profile
- memory location and tier
- current engine availability
- network quality
- historical outcomes
- user or system preferences

### 9.2 Locality Inputs

Locality-sensitive routing should consider:

- where the source objects live
- where the needed representations live
- whether remote transport would violate policy
- whether it is cheaper to move compute or data
- whether the task must remain offline-capable

### 9.3 Trust Inputs

Routing may bias toward engines or plans with:

- stronger validation support
- higher historical success on the task type
- better reproducibility
- lower contradiction rate

---

## 10. Feasibility Filtering

### 10.1 Purpose

Before scoring candidate routes, the routing plane must eliminate all routes that cannot satisfy hard requirements.

### 10.2 Feasibility Constraints

Examples:

- engine lacks required capability
- privacy policy forbids remote inference
- model cannot handle required input modality
- network unavailable but route requires remote access
- context length insufficient
- cost cap cannot be met
- validation requirements cannot be satisfied
- data residency restrictions block route

### 10.3 Feasibility Rules

1. Policy constraints must be applied first.
2. Feasibility filtering should be deterministic where possible.
3. The system must explain why a candidate route was excluded.

---

## 11. Route Strategies

### 11.1 Initial Strategies

Supported initial strategies:

- `local_first`
- `cost_first`
- `latency_first`
- `confidence_first`
- `privacy_first`
- `adaptive`
- `policy_locked`

### 11.2 Strategy Semantics

#### local_first
Prefer local engines and local memory surfaces unless quality constraints require escalation.

#### cost_first
Minimize expected spend subject to success and policy constraints.

#### latency_first
Prefer the quickest path that clears minimum quality and policy thresholds.

#### confidence_first
Prefer routes historically associated with stronger validation outcomes or better task quality.

#### privacy_first
Maximize local and policy-restricted processing, even if cost or latency rises.

#### adaptive
Use a weighted multi-objective score informed by current conditions and historical performance.

### 11.3 Default Strategy

For private or mixed-sensitivity workloads, the recommended initial default is:

```text
local_first + validation_gated escalation
```

---

## 12. Candidate Route Generation

### 12.1 Purpose

A single task may be executable through multiple valid plans.

### 12.2 Route Forms

#### Direct Route
One engine handles the task end to end.

#### Staged Route
A deterministic or retrieval stage precedes a model stage.

#### Decomposed Route
The task is split into subtasks with different engines.

#### Recursive Route
A parent route delegates hard subproblems to child cells.

### 12.3 Example Candidate Routes

For summarizing a meeting:

1. `local_model only`
2. `lexical retrieval -> local_model`
3. `semantic retrieval -> local_model -> validator`
4. `local_model -> remote_model fallback -> validator`
5. `decompose into summary + action extraction + merge`

### 12.4 Candidate Generation Rules

1. Candidate generation must be bounded.
2. It should use intent and type information, not just string matching.
3. Candidate routes must record fallback paths when permitted.

---

## 13. Route Scoring

### 13.1 Purpose

After feasibility filtering, candidate routes should be ranked.

### 13.2 Scoring Dimensions

A route score may consider:

- expected latency
- expected cost
- expected confidence
- locality fit
- policy robustness
- energy use
- retrieval quality fit
- fallback richness
- reproducibility
- historical success

### 13.3 Scoring Model

A simple initial route score can be expressed as:

```text
RouteScore =
  w_confidence * confidence_estimate
- w_latency    * latency_estimate
- w_cost       * cost_estimate
+ w_locality   * locality_score
+ w_policy     * policy_margin
+ w_history    * historical_success_score
```

### 13.4 Scoring Rules

1. Policy must remain a hard gate, not merely a soft penalty.
2. Score computation should be logged for traceability.
3. Initial weights should be configuration-driven, not hidden.
4. Historical outcomes should influence score but not erase hard constraints.

---

## 14. Decomposition-Aware Routing

### 14.1 Principle

Route selection should not assume that the entire task is best handled by a single engine.

### 14.2 Decomposition Triggers

Decomposition may be considered when:

- task type is composite
- latency or cost would improve through specialization
- validation requirements suggest staged processing
- one engine cannot satisfy all sub-capabilities
- memory retrieval can significantly reduce problem size

### 14.3 Decomposition Example

```text
answer_complex_question
  -> retrieve_relevant_memory
  -> solve_subquestion_a
  -> solve_subquestion_b
  -> merge_results
  -> validate_consistency
```

### 14.4 Decomposition Rules

1. Decomposition must preserve lineage back to the parent cell.
2. Budget allocation should be explicit across child routes.
3. Excessive decomposition must be capped.

---

## 15. Unified Scheduler

### 15.1 Purpose

The Unified Scheduler allocates resources to the selected route.

### 15.2 Scheduler Scope

The scheduler should reason over:

- CPU shares
- GPU / NPU fractions
- memory tier bandwidth
- queue class
- local I/O capacity
- network budget
- remote slot availability
- execution priority

### 15.3 Queue Classes

Initial queue classes:

- `interactive`
- `background`
- `latency_sensitive`
- `batch`
- `validation`
- `replication`

### 15.4 Scheduler Binding Schema

```json
{
  "route_plan_ref": "plan_01JR...",
  "queue_class": "interactive",
  "priority": "high",
  "resource_contract": {
    "cpu_shares": 200,
    "gpu_fraction": 0.25,
    "memory_tier_budget": {
      "L1_mb": 512,
      "L3_queries": 8
    },
    "network_budget_class": "moderate"
  }
}
```

### 15.5 Scheduler Rules

1. A valid route plan is required before binding.
2. Interactive routes should preempt background work within policy bounds.
3. Validation and safety-critical checks should not starve indefinitely.
4. The scheduler should honor explicit budget caps from the cell.

---

## 16. Budget Profiles

### 16.1 Purpose

Budget profiles encode acceptable tradeoffs for a task or class of tasks.

### 16.2 Budget Dimensions

- latency
- cost
- energy
- network usage
- remote dependence
- validation strictness

### 16.3 Example Budget Profiles

#### Interactive Private
- low latency
- prefer local
- avoid remote
- moderate quality floor

#### Background Enrichment
- lower priority
- can use spare resources
- higher tolerance for latency
- aggressive cost control

#### Critical Decision Support
- strong validation
- confidence prioritized
- remote allowed if policy permits
- higher cost ceiling

### 16.4 Budget Rules

1. Budgets may be declared by the caller or inferred by policy.
2. Budget profiles should be versioned and inspectable.
3. Route selection should record which budget profile was applied.

---

## 17. Network-First and Degraded Behavior

### 17.1 Principle

The system should treat the network as a normal execution substrate without becoming fragile when network quality drops.

### 17.2 Network Modes

- `offline_only`
- `local_only`
- `local_first`
- `edge_preferred`
- `remote_allowed`
- `remote_required`

### 17.3 Degraded Routing Behavior

When network conditions worsen, the routing plane may:

- switch to local retrieval only
- replace remote models with local models
- narrow context to fit local limits
- defer non-essential enrichments
- mark outputs as degraded or lower confidence
- enter waiting state if correctness requires remote access

### 17.4 Degraded Behavior Rules

1. Silent quality collapse is discouraged.
2. The system should annotate degraded decisions in trace.
3. Critical paths should prefer known fallback routes over ad hoc substitutions.

---

## 18. Feedback and Route Learning

### 18.1 Purpose

Routing quality should improve over time based on observed outcomes.

### 18.2 Outcome Signals

Potential feedback signals include:

- latency achieved
- cost realized
- validation pass rate
- contradiction rate
- user correction rate
- fallback frequency
- retry count
- resource pressure

### 18.3 Route Feedback Record

```json
{
  "route_plan_ref": "plan_01JR...",
  "task_family": "meeting_summarization",
  "engine_sequence": [
    "eng_tool_local_search",
    "eng_local_qwen3_8b",
    "eng_validator_schema"
  ],
  "observed_latency_ms": 2310,
  "observed_cost_usd": 0.002,
  "validation_outcome": "pass_with_warnings",
  "user_correction": false
}
```

### 18.4 Learning Rules

1. Initial routing should use rules plus logged feedback, not opaque end-to-end learned policy.
2. Learned adjustments must remain explainable.
3. Historical signals should decay over time when environments or models change.

---

## 19. Route Trace and Observability

### 19.1 Trace Requirements

Every routed execution should record:

- route strategy
- candidate routes considered
- excluded candidates and reasons
- selected route
- fallback path
- scheduler binding
- outcome metrics
- replan events

### 19.2 Minimum Metrics

- route planning latency
- selected engine class
- local vs remote ratio
- fallback usage
- validation success by route family
- cost per route family
- degradation frequency
- queue wait time

### 19.3 Debug Questions the System Must Answer

- Why did this task use a remote model?
- Why did this task not use the local model?
- Why did validation fail after route selection?
- Why did the route replan during execution?
- Why was this job queued instead of executed immediately?

---

## 20. Security and Policy

### 20.1 Baseline Requirements

1. Routing must obey object execution policy.
2. Remote model usage requires explicit permission.
3. Data export must be policy-checked before route commitment.
4. Scheduler bindings must not grant resources or capabilities forbidden by policy.

### 20.2 Policy Tags

Example policy tags for engines:

- `private_safe`
- `remote_export`
- `high_cost`
- `regulated_data_forbidden`
- `offline_capable`

### 20.3 Policy Resolution

Effective route policy should be derived from the intersection of:

- cell execution policy
- input object policies
- user or system governance
- engine policy tags
- network availability and residency constraints

---

## 21. APIs

### 21.1 Get Capabilities

```http
GET /routing/capabilities
```

### 21.2 Plan Route

```http
POST /routing/plan
```

Request:

```json
{
  "cell_ref": "cell_01JR...",
  "strategy": "local_first",
  "budget_profile": "interactive_private"
}
```

### 21.3 Score Candidates

```http
POST /routing/score
```

### 21.4 Bind Schedule

```http
POST /scheduler/bind
```

### 21.5 Inspect Route

```http
GET /routing/plans/{id}
```

### 21.6 Replan Route

```http
POST /routing/plans/{id}/replan
```

### 21.7 Submit Feedback

```http
POST /routing/feedback
```

---

## 22. CLI Surface

Suggested initial CLI:

```bash
route plan --cell cell_01JR... --strategy local_first
route inspect plan_01JR...
route replan plan_01JR...
route capabilities
sched bind plan_01JR... --queue interactive
sched status
route feedback --plan plan_01JR... --validation pass
```

Advanced example:

```bash
route plan \
  --cell cell_01JR... \
  --strategy adaptive \
  --budget critical_decision_support \
  --prefer-local \
  --allow-remote-fallback
```

---

## 23. POSIX Compatibility

### 23.1 Compatibility Thesis

The Routing Plane and Unified Scheduler generalize process scheduling without removing classic process semantics.

### 23.2 Mapping

| Classical Concept | Routing / Scheduler Equivalent |
|---|---|
| process selection | engine selection |
| process priority | queue class + priority |
| CPU scheduler decision | scheduler binding |
| fork/exec chain | decomposed route plan |
| remote call in app code | first-class routed execution step |

### 23.3 Compatibility Mode

In the initial prototype, normal processes remain runnable, but route decisions can wrap or augment them through Execution Cells.

---

## 24. Reference Prototype Architecture

### 24.1 Initial Components

1. **Capability Registry Service**
   - engine inventory and status

2. **Route Planner**
   - feasibility filtering and candidate generation

3. **Route Scorer**
   - strategy and budget-aware ranking

4. **Scheduler Service**
   - queueing and resource binding

5. **Execution Adapters**
   - local tool, local model, remote model, retrieval, graph, validation

6. **Feedback Service**
   - route outcomes and historical metrics

### 24.2 Recommended Initial Stack

- Python for planner and scheduler orchestration
- PostgreSQL for registry, plans, bindings, and metrics
- Redis or similar lightweight queue for job dispatch
- HTMX-based routing dashboard if desired
- local model runner adapters first
- remote adapters second
- optional Rust components later for high-throughput scheduling paths

---

## 25. Failure Modes

### 25.1 Route Oscillation
The system repeatedly shifts between routes without stability.

**Mitigation:** hysteresis, cooldown windows, bounded replanning.

### 25.2 Hidden Remote Spend
Remote routes are selected too often and cost balloons.

**Mitigation:** hard budget profiles, route cost accounting, remote usage caps.

### 25.3 Quality Collapse Under Degradation
Fallback routes preserve availability but not usefulness.

**Mitigation:** minimum quality floors, degraded-output annotations, explicit waiting when needed.

### 25.4 Capability Drift
Registry says an engine can do something it can no longer do well.

**Mitigation:** health checks, outcome feedback decay, capability verification.

### 25.5 Scheduler Starvation
Background or validation tasks never run.

**Mitigation:** queue fairness, reserved capacity for validation and maintenance classes.

### 25.6 Policy Bypass
Routes are scored before policy filtering or adapters violate route constraints.

**Mitigation:** feasibility-first architecture and centralized policy evaluation.

---

## 26. Implementation Plan

### Phase 1: Static Rules and Local Engines
- capability registry
- route feasibility filtering
- local deterministic and local model routes
- simple queue classes

### Phase 2: Cost, Latency, and Validation Scoring
- budget profiles
- weighted route scoring
- validation outcome tracking
- route trace

### Phase 3: Decomposition-Aware Routing
- child-cell route planning
- bounded recursive routing
- scheduler contracts for multi-step plans

### Phase 4: Network-First Routing
- remote engines
- degraded/offline behavior
- network quality signals
- remote slot budgeting

### Phase 5: Feedback-Driven Optimization
- route outcome learning
- capability drift detection
- adaptive scoring updates
- richer dashboard and observability

---

## 27. Open Questions

1. What is the right default weighting between confidence, latency, and cost?
2. How should route learning incorporate user corrections without overfitting?
3. Should the scheduler directly observe memory tier pressure, or only indirect signals?
4. When should the routing plane prefer decomposition over a stronger monolithic model?
5. How should route reproducibility be preserved when engine versions change?
6. What is the best contract between the scheduler and remote inference providers?
7. Should graph traversal be modeled as retrieval, execution, or its own engine family?

---

## 28. Decision Summary

This RFC makes the following decisions:

1. Routing and scheduling are first-class subsystems.
2. Engines are selected by capability and constraints, not brand names.
3. Feasibility filtering happens before route scoring.
4. Policy remains a hard gate throughout route planning and scheduling.
5. The unified scheduler binds routes across local compute, accelerators, memory, and network budgets.
6. Network-first operation is supported, but degraded local behavior must be explicit.
7. Route choice and scheduler decisions must be traceable.
8. Initial implementation should use rules plus feedback, not opaque end-to-end learned routing.

---

## 29. Conclusion

The Routing Plane and Unified Scheduler are the parts of the architecture that convert abstract intent into concrete execution.

They provide the system with a principled way to decide:

- which engine should do the work
- where it should run
- how much should be spent
- how much latency is acceptable
- when to remain local
- when to escalate remotely
- when to decompose
- when to wait
- when to degrade gracefully

Without this layer, model routing remains application glue. With it, routing becomes an operating capability.
