# RFC-0007: Capability Objects and Runtime Installation

| Field      | Value                                                     |
|------------|-----------------------------------------------------------|
| RFC        | 0007                                                      |
| Title      | Capability Objects and Runtime Installation                |
| Author     | Adam Pippert                                              |
| Status     | Draft                                                     |
| Created    | 2026-04-13                                                |
| Updated    | 2026-04-13                                                |
| Depends On | RFC-0001, RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0006|

---

## Executive Summary

The Anunix architecture can now track what exists (State Objects, RFC-0002), what runs (Execution Cells, RFC-0003), what persists (Memory Control Plane, RFC-0004), how work is routed (Routing Plane and Unified Scheduler, RFC-0005), and how the system extends across network boundaries (Network Plane, RFC-0006). But it cannot express what the machine has **learned to do durably**. When a complex multi-step task succeeds — a task that required specific decomposition, routing choices, validation sequences, and fallback strategies — the operational knowledge that made that success possible evaporates. The next time a similar task arrives, the system starts from scratch: re-decomposing, re-routing, re-discovering the same failure modes, re-learning the same workarounds. This is the computational equivalent of institutional amnesia.

RFC-0007 introduces the **Capability Object**, a new State Object type encoding operational competence the system has acquired, validated, and can install into the routing plane as a first-class engine. A Capability Object is not a prompt template, not a saved workflow, and not a configuration file. It is a kernel-level representation of a validated procedure — a composition of engines, routing constraints, validation requirements, and fallback strategies that has been proven to work and promoted through a trust lifecycle. Once installed, a Capability Object participates in routing decisions as a native engine class: the scheduler can route tasks to an installed capability just as it routes to a deterministic tool or a model endpoint.

This RFC specifies the Capability Object schema, the trust lifecycle from formation through retirement, the installation mechanism that registers capabilities as routable engines, the composition and dependency model, the versioning and supersession semantics, the network distribution protocol, and the observability requirements. The design goal is to close the gap between "the system can do X once" and "the system reliably knows how to do X" — making operational learning a durable, composable, policy-governed property of the system rather than an ephemeral artifact of one execution.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert / public collaborators
**Depends on:** RFC-0001, RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0006
**Blocks:** RFC-0008

---

## 2. Problem Statement

### 2.1 The Knowledge Evaporation Problem

Consider a task that requires the system to: retrieve relevant documents from a knowledge base, extract structured data from those documents using a local model, cross-validate the extraction against a graph of known entities, resolve contradictions using a remote reasoning model, format the validated result according to a domain-specific schema, and commit the output with appropriate provenance. Suppose this task succeeds after the routing plane selects the right engines, the scheduler allocates appropriate resources, the validation service catches and corrects two extraction errors, and the fallback path handles a transient network failure during the cross-validation step.

That success represents genuine operational knowledge: which engines work for which substeps, what validation sequence catches real errors, how to handle the specific failure mode that occurred, and what decomposition structure produces reliable results. But none of that knowledge is captured in a form the system can reuse. The Execution Cell completes, its trace is recorded, and the operational insight is buried in a log that no routing decision will ever consult. The next time a similar task arrives, the system must rediscover everything from scratch.

This is not a hypothetical concern. It is the central failure mode of every system that treats execution as stateless. Traditional operating systems avoid the problem because their tasks are simple enough that rediscovery is cheap — running `grep` does not require learned orchestration. But in an AI-native system where tasks routinely involve multi-engine composition, probabilistic generation, retrieval-augmented processing, and validation pipelines, the cost of rediscovery is substantial. The system that cannot remember what it has learned to do is permanently inexperienced.

### 2.2 What Existing RFCs Cannot Express

Each preceding RFC contributes essential infrastructure, but none of them closes the knowledge evaporation gap.

**RFC-0002 (State Object Model)** defines six canonical object types: `byte_data`, `structured_data`, `embedding`, `graph_node`, `model_output`, and `execution_trace`. These types cover the essential categories of data at rest and data in transit. None of them represents an operational procedure. An `execution_trace` records what happened during one execution, but it is a historical record, not an installable plan. A `structured_data` object could store a procedure description, but the kernel would have no way to distinguish it from any other JSON document — it could not route to it, enforce its contracts, manage its lifecycle, or reason about its dependencies. The type system was deliberately designed to be small and closed (Section 5.7 requires an RFC amendment to add a type), which means the absence of a procedure type is a real architectural gap, not an oversight that metadata can patch.

**RFC-0003 (Execution Cell Runtime)** defines the execution abstraction — cells that decompose, route, validate, and commit. Cells are excellent at executing work, but they are fundamentally ephemeral. A cell runs, produces outputs, and terminates. The decomposition strategy, routing decisions, and validation sequences that made the cell successful are encoded in the cell's trace, but the trace is a forensic artifact, not a reusable blueprint. There is no mechanism to say "this decomposition pattern worked — install it as a reusable capability." The Cell Runtime knows how to execute plans; it does not know how to learn from them.

**RFC-0004 (Memory Control Plane)** provides durable, tiered, retrievable memory with trust states and contradiction tracking. Memory can store facts, embeddings, graph relations, and observations. It can remember that "the Tantivy search engine is effective for this corpus" or "model X produces unreliable extractions for tables with merged cells." But memory stores declarative knowledge — observations about the world. It cannot store procedural knowledge in a form that the routing plane can act on. A memory entry saying "use engine A for step 1, then engine B for step 2, with validation C between them" is just text to the memory system. It has no contract, no dependency resolution, no installation mechanism, and no trust lifecycle.

**RFC-0005 (Routing Plane and Unified Scheduler)** routes tasks to engines based on capability matching, policy constraints, and multi-objective scoring. The Capability Registry (Section 8) catalogs what engines can do. But the registry only knows about atomic engine capabilities — a single engine's declared abilities. It defines eight engine classes (`deterministic_tool`, `local_model`, `remote_model`, `retrieval_service`, `graph_service`, `validation_service`, `execution_service`, `device_service`), each representing a single execution surface. There is no concept of a **composite capability** — a learned combination of multiple engines, routing constraints, and validation requirements that together accomplish something none of them can accomplish alone. The routing plane can pick the best single engine for a task, but it cannot install a proven multi-step procedure as a routable unit.

**RFC-0006 (Network Plane and Federated Execution)** enables capability advertisement across network boundaries. Peer nodes can declare which engines and services they offer, and the routing plane can incorporate remote engines into its decisions. But capability advertisements (Section 11) describe what remote nodes have — engines with declared properties. They do not describe what remote nodes have learned to do. A node that has developed and validated a sophisticated multi-step procedure for a specific class of tasks cannot share that operational knowledge with peers in a form that the receiving node's routing plane can install and use.

### 2.3 Why Metadata Is Not Enough

One might argue that the existing object types can accommodate procedures through metadata conventions. A `structured_data` object could carry a procedure description in its payload, with metadata tags marking it as a capability. This approach fails for four reasons that align with RFC-0002 Section 5.7's criteria for justifying a new kernel-level type.

**Routing priority insertion.** An installed capability must participate in route planning as a first-class engine. When the routing plane evaluates candidate execution paths for a task, it must consider installed capabilities alongside atomic engines. This requires the kernel to understand what a capability object *is* — its input contract, output contract, engine dependencies, and confidence bounds. A `structured_data` object with a "capability" tag in its metadata is invisible to the routing plane unless the routing plane is taught to scan all structured data objects looking for capability-shaped metadata, which is precisely the kind of convention-based workaround that Section 5.7 exists to prevent.

**Contract enforcement at admission.** When a capability is invoked, its input contract must be validated before execution begins. The kernel must verify that the provided inputs match the capability's declared schema, that all required engines are available, and that policy constraints are satisfied. This is admission control — a kernel responsibility. Delegating it to userland metadata inspection produces the same enforcement gap that RFC-0002 Section 2.3 identified for provenance and lifecycle: conventions can be silently circumvented.

**Supersession and dependency graph management.** Capabilities evolve. A capability that was validated at version 1 may be superseded by version 2 with a different engine composition. The system must track which capabilities depend on which engines, which capabilities supersede which prior versions, and what happens when a dependency becomes unavailable. This is a graph management problem that requires kernel participation — the same argument that justified kernel-managed provenance in RFC-0002 rather than external lineage tracking.

**Trust lifecycle tracking.** A capability moves through trust states: `candidate`, `validated`, `installed`, `deprecated`, `retired`. These transitions must be policy-governed and auditable. The kernel must refuse to route to a capability that has not been validated. It must refuse to install a capability whose dependencies are unavailable. It must propagate deprecation when an upstream dependency is deprecated. These are enforcement requirements, not annotation requirements, and they demand a kernel-level type.

---

## 3. Goals

### 3.1 Primary Goals

1. **Operational knowledge as state**
   - The system must be able to represent validated operational procedures — specific compositions of engines, routing strategies, validation sequences, and fallback paths — as first-class State Objects with full provenance, policy, and lifecycle management.

2. **Installable into routing**
   - A validated Capability Object must be installable into the routing plane as a native engine class, so that the Unified Scheduler can route tasks to an installed capability just as it routes to a deterministic tool or a model endpoint. This must not require special-case logic in the routing plane; the installed capability must participate through the same interfaces as any other engine.

3. **Lifecycle-managed**
   - Capability Objects must have a defined lifecycle from initial formation through validation, installation, deprecation, and retirement. Each lifecycle transition must be policy-governed, auditable, and reversible where appropriate. The system must not allow unvalidated capabilities to participate in routing, and it must not allow deprecated capabilities to be installed on new nodes.

4. **Trust-gated promotion**
   - The transition from "candidate capability" to "installed capability" must pass through explicit validation gates. These gates may include execution against test inputs, comparison with known-good outputs, policy review, and human approval. The system must support both automated and human-in-the-loop validation, and must record the validation evidence as part of the capability's provenance.

5. **Composable**
   - Capabilities must be composable: a higher-level capability may depend on lower-level capabilities as components. The composition model must support dependency declaration, version pinning, and graceful degradation when a component capability is unavailable. Circular dependencies must be detected and rejected at installation time.

6. **Versionable and supersedable**
   - Capabilities must support explicit versioning. A new version of a capability may supersede an older version, inheriting its routing registrations while providing updated procedure logic. The system must track the supersession chain and must allow rollback to a previous version if the new version fails validation.

7. **Network-distributable**
   - Capability Objects must be distributable across the Network Plane (RFC-0006) using the existing replication and capability advertisement infrastructure. A node that has developed and validated a capability must be able to share it with peer nodes, subject to trust zone policies and attestation requirements.

8. **Observable**
   - Every aspect of capability lifecycle — formation, validation, installation, invocation, refinement, deprecation, retirement — must produce traceable events. Operators must be able to answer: what capabilities are installed, how often is each invoked, what is each capability's success rate, which capabilities depend on which engines, and which capabilities are candidates for promotion or retirement.

### 3.2 Non-Goals

1. **Replacing all engine classes with capabilities.**
   Capabilities are compositions built on top of atomic engines. The eight engine classes defined in RFC-0005 remain the foundation. A `deterministic_tool` does not need to be wrapped in a capability to be useful. Capabilities exist to capture *learned compositions*, not to replace atomic execution surfaces.

2. **Autonomous self-modification without validation gates.**
   The system must not autonomously install or modify capabilities without passing through defined validation gates. Self-improvement is a goal; uncontrolled self-modification is a hazard. Every capability promotion must be auditable and, where policy requires, human-approved.

3. **Guaranteeing universal portability.**
   A capability developed on one node may depend on engines, memory contents, or network resources that are not available on another node. The system must detect and report dependency mismatches, but it is not required to make every capability portable to every environment. Portability is a property of specific capabilities, not a universal guarantee.

4. **Building a general-purpose workflow engine.**
   Capability Objects encode validated procedures, not arbitrary workflow definitions. The system is not intended to compete with general-purpose orchestration tools. Capabilities are narrower in scope (they encode what *worked*) and richer in trust semantics (they carry validation evidence and lifecycle state) than a typical workflow definition.

5. **Requiring every task to use an installed capability.**
   Ad-hoc execution remains fully supported. A task that does not match any installed capability is routed normally through the routing plane to atomic engines. Capabilities are an optimization and a knowledge-preservation mechanism, not a mandatory execution path.

---

## 4. Core Definitions

### 4.1 Capability Object

A **Capability Object** is a State Object (RFC-0002) of a new canonical type `capability` that encodes a validated operational procedure. It declares:

- the task class it addresses (what kinds of inputs it accepts and what outputs it produces)
- the engine composition it requires (which engines, in what arrangement)
- the routing constraints it imposes (locality, trust zone, budget bounds)
- the validation sequence it prescribes (what checks must pass before output is committed)
- the fallback strategy it defines (what to do when a component engine is unavailable)
- the trust state it currently holds (candidate, validated, installed, deprecated, retired)
- the provenance of its formation and validation (which executions produced it, what evidence supports it)

A Capability Object is not executable code. It is a declarative specification that the Cell Runtime interprets when the routing plane selects it for a task. The Cell Runtime reads the capability's procedure specification and generates an Execution Cell plan accordingly.

### 4.2 Capability Bundle

A **Capability Bundle** is a set of related Capability Objects that together address a coherent domain. For example, a "document analysis" bundle might include capabilities for extraction, validation, summarization, and cross-referencing. Bundles are organizational units, not execution units — they group capabilities for discovery, distribution, and lifecycle management. A bundle is itself a State Object of type `structured_data` with a well-known schema that references its constituent capability OIDs.

### 4.3 Capability Installation

**Capability Installation** is the process by which a validated Capability Object is registered with the routing plane as a routable engine. Installation creates an entry in the Capability Registry (RFC-0005 Section 8) with engine class `installed_capability`. After installation, the routing plane may select the capability as a candidate execution path for matching tasks. Installation is reversible: an installed capability can be uninstalled, which removes it from the routing plane without deleting the underlying State Object.

### 4.4 Capability Refinement

**Capability Refinement** is the process of improving an existing capability based on execution feedback. When an installed capability is invoked and the outcome reveals a suboptimal routing choice, a missing validation step, or a better fallback strategy, the system may produce a refined version of the capability. The refined version is a new Capability Object with a provenance link to the original. It must pass through the same validation gates before it can be installed.

### 4.5 Capability Supersession

**Capability Supersession** occurs when a new version of a capability replaces an older version in the routing plane. The new version explicitly declares which capability it supersedes. Upon successful installation of the superseding capability, the superseded version is transitioned to `deprecated` status. The supersession chain is recorded in provenance, enabling rollback if the new version proves inferior.

### 4.6 Capability Retirement

**Capability Retirement** is the terminal lifecycle state. A retired capability is removed from the routing plane, excluded from distribution, and marked as historically significant but no longer operational. Retirement may occur because the capability has been superseded, because its engine dependencies are permanently unavailable, or because policy requires its removal. The Capability Object itself is not deleted — its provenance and validation history remain accessible for audit — but it is ineligible for installation or distribution.

### 4.7 Capability Attestation

A **Capability Attestation** is a signed statement from a validation authority (which may be local or remote) asserting that a capability has passed a specific set of validation criteria. Attestations are attached to the Capability Object's provenance record. They are required for installation (a capability cannot be installed without at least one valid attestation) and for network distribution (a receiving node may require attestations from authorities it trusts before accepting a remote capability).

### 4.8 Installed Capability Engine

An **Installed Capability Engine** is the routing-plane representation of an installed capability. It appears in the Capability Registry as an engine of class `installed_capability`, with declared capabilities, constraints, and status derived from the underlying Capability Object. The Installed Capability Engine is the interface through which the routing plane interacts with capabilities — it does not contain the procedure logic itself, but references the Capability Object that does.

---

## 5. Design Principles

### 5.1 Capabilities Are Earned, Not Declared

A capability is not created by writing a procedure specification and declaring it operational. A capability is formed from evidence: successful executions, validated outputs, observed failure modes and their resolutions. The system must distinguish between a hypothetical procedure ("this sequence of engines might work") and a validated capability ("this sequence of engines has been tested against representative inputs and produced correct, validated outputs"). Only validated capabilities may be installed into the routing plane. This principle prevents the routing plane from being polluted with untested procedures and ensures that the word "capability" means something the system has demonstrated, not merely asserted.

### 5.2 Validation Is Continuous

Validation is not a one-time gate that a capability passes and then ignores. An installed capability must continue to demonstrate its effectiveness through ongoing execution. The system must track per-capability success rates, latency distributions, and validation failure rates. When an installed capability's performance degrades below defined thresholds, it must be flagged for review, and may be automatically demoted from `installed` to `deprecated` status. Continuous validation ensures that capabilities remain trustworthy as the underlying engines, data, and environment evolve. A capability that was valid six months ago against a different corpus with different models is not necessarily valid today.

### 5.3 Supersession Must Be Explicit

When a new version of a capability replaces an old one, the replacement must be declared, not inferred. The new capability must explicitly name the capability it supersedes, and the system must verify that the new version addresses the same task class before allowing the supersession. Implicit supersession — where a new capability silently shadows an old one because it happens to match the same routing criteria — is forbidden. This principle ensures that the supersession graph is auditable and that rollback is always possible. It also prevents the accumulation of redundant capabilities that compete for the same routing slots without clear precedence.

### 5.4 Capabilities Are State Objects

A Capability Object inherits all properties of State Objects as defined in RFC-0002: globally unique identity, content-addressable hashing, immutable provenance, policy governance, lifecycle management, and metadata. This is not a design convenience — it is a requirement. Capabilities must be addressable by OID, traceable through provenance chains, subject to access policy, governed by retention rules, and manageable through the same kernel interfaces as every other piece of state in the system. The decision to make capabilities a kernel-level type rather than a metadata convention follows directly from RFC-0002 Section 5.7: the kernel must understand what a capability is in order to enforce its contracts, manage its lifecycle, and integrate it into routing decisions.

### 5.5 Routing Integration Must Be Clean

An installed capability must participate in the routing plane through the same interfaces as any other engine class. The routing plane must not contain special-case logic for capabilities. Instead, the installation process creates an engine registry entry of class `installed_capability` with declared capabilities, constraints, and status fields that the routing plane's existing feasibility filter, scoring function, and scheduling logic can evaluate without modification. This principle ensures that the routing plane remains a single, coherent decision layer rather than a system with two parallel routing paths — one for atomic engines and one for capabilities. The cost of clean integration is borne at installation time, not at every routing decision.

### 5.6 Dependencies Must Be Resolved

A capability that depends on specific engines, memory contents, or other capabilities must declare those dependencies explicitly. The installation process must verify that all declared dependencies are available and satisfy version constraints before allowing installation. If a dependency becomes unavailable after installation (an engine goes offline, a subordinate capability is retired), the system must propagate the impact: the dependent capability must be flagged, and the routing plane must stop routing to it until the dependency is restored or an alternative is provided. Lazy dependency resolution — installing a capability and hoping its dependencies will be available at invocation time — is not acceptable. The system must know at installation time whether a capability can function.

### 5.7 Network Distribution Requires Attestation

A capability that is distributed to a peer node via the Network Plane (RFC-0006) must carry sufficient attestation for the receiving node to evaluate its trustworthiness. The receiving node must not be required to trust the originating node's validation unconditionally. Instead, the capability must carry signed attestations that the receiving node can verify against its own trust policies. A node may require attestations from specific authorities, may require a minimum number of independent attestations, or may require that the capability be re-validated locally before installation. This principle prevents the network from becoming a vector for unvalidated procedure injection and ensures that each node retains authority over what capabilities it installs.

---

## 6. Capability Object Schema

This section invokes the Type Extension Process defined in RFC-0002 Section 5.7 to add `capability` as the seventh canonical State Object type. The justification follows the three requirements of that process: (1) an RFC amendment specifying the payload structure and kernel behavior, (2) a demonstration that the new type enables kernel-level optimizations not achievable via existing types with metadata, and (3) backward-compatible storage format changes.

### 6.1 New State Object Type: `capability`

A `capability` State Object encodes an operational competence: a validated, reusable, installable procedure that the system has learned or been taught to perform. It is the seventh canonical type alongside `byte_data`, `structured_data`, `embedding`, `graph_node`, `model_output`, and `execution_trace`.

**Why not `structured_data` with metadata?** A capability stored as `structured_data` with appropriate metadata would be opaque to the kernel. The kernel could not:

- **Insert it into routing priority.** The routing plane (RFC-0005) selects execution paths based on engine class. A `structured_data` object has no engine class — it is data, not an execution path. The kernel must understand `capability` as a type to register it as an `installed_capability` engine and include it in feasibility filtering and route scoring.
- **Enforce input/output contracts at admission time.** When a cell arrives and the routing plane considers an installed capability, the kernel must verify that the cell's inputs satisfy the capability's input contract before routing. This requires parsing the capability's contract structure — not possible if the kernel treats the payload as opaque structured data.
- **Manage the supersession graph.** Capabilities replace each other over time. The kernel must maintain supersession edges, prevent supersession cycles, and cascade staleness to dependent capabilities. These are graph-integrity operations that require the kernel to understand the `supersedes_ref` and `superseded_by_ref` fields as first-class relationships, not opaque metadata.
- **Track the trust lifecycle.** Capability trust (confidence, validation history, freshness) is distinct from generic memory trust (RFC-0004 Section 16.2). The kernel must update trust fields based on production outcomes and trigger re-validation when freshness degrades — a lifecycle that is specific to the `capability` type and cannot be generalized from `structured_data` handling.

These four requirements satisfy the bar set by RFC-0002 Section 5.7: the kernel must understand the type to provide optimizations and enforcement that cannot be achieved through metadata alone.

### 6.2 Payload Structure

```
CapabilityPayload {
    capability_name:     string          // human-readable name (e.g., "meeting_summarization")
    capability_version:  string          // semver string (e.g., "1.2.0")
    status:              CapabilityStatus // lifecycle state (Section 7)
    procedure_ref:       oid             // structured_data: the procedure definition (Section 8)
    validation_suite_ref: oid            // structured_data: validation criteria (Section 9)
    input_contract:      InputContract   // what the capability accepts (Section 6.4)
    output_contract:     OutputContract  // what the capability produces (Section 6.4)
    dependencies:        Dependency[]    // other capabilities or resources required (Section 6.5)
    provenance_summary:  ProvenanceSummary // how this capability was learned (Section 6.7)
    trust:               CapabilityTrust // validation and confidence history (Section 6.6)
    supersedes_ref:      oid?            // capability this one replaces (null if original)
    superseded_by_ref:   oid?            // capability that replaced this one (null if current)
    tags:                string[]        // routing-relevant capability tags
}
```

The payload is separated from the procedure and validation suite deliberately. The capability object is the *envelope* — it declares what the capability does, what it requires, and how much to trust it. The procedure and validation suite are separate State Objects referenced by OID. This separation enables:

- Independent versioning of procedure and validation.
- Loading the capability envelope (for routing decisions) without loading the full procedure (for execution).
- Sharing validation suites across related capabilities.
- Replacing a procedure without changing the capability's identity.

### 6.3 CapabilityStatus

The `CapabilityStatus` enum defines seven lifecycle states. The full state machine is specified in Section 7; this section defines the values:

| Status        | Description                                                                   |
|---------------|-------------------------------------------------------------------------------|
| `draft`       | Capability envelope exists; procedure may be incomplete. Not routable.        |
| `validating`  | Validation suite is being executed. Not routable.                             |
| `validated`   | Passed validation; eligible for installation. Not yet routable.               |
| `installed`   | Registered with the routing plane. Routable.                                  |
| `suspended`   | Temporarily removed from routing due to failure, policy, or dependency issue. |
| `superseded`  | Replaced by a newer version. Not routable. Preserved for provenance.          |
| `retired`     | Permanently removed from active use. Not routable. May be archived.           |

The status field is managed exclusively by the kernel in response to lifecycle operations (Section 7). Applications cannot set the status directly — they invoke lifecycle cells (Section 11) which trigger kernel-managed transitions.

### 6.4 InputContract and OutputContract

Contracts define the typed interface between the capability and the cells that invoke it. The routing plane uses contracts for feasibility filtering (Section 12.5): a capability is only considered for a task if the task's inputs can satisfy the capability's input contract.

```
InputContract {
    slots:              InputSlot[]         // named input requirements
    constraints:        ContractConstraint[] // cross-slot constraints
}

InputSlot {
    name:               string              // slot name (e.g., "source_document")
    required:           bool                // must be provided at invocation
    accepted_types:     object_type[]       // which State Object types are accepted
    schema_uri:         string?             // required schema conformance, if any
    min_trust:          string?             // minimum memory trust state (e.g., "validated")
    description:        string              // human-readable purpose
}

ContractConstraint {
    kind:               string              // e.g., "mutual_exclusion", "co_required"
    slot_names:         string[]            // slots this constraint applies to
    description:        string
}
```

```
OutputContract {
    slots:              OutputSlot[]        // named output declarations
    guarantees:         ContractGuarantee[] // quality and schema guarantees
}

OutputSlot {
    name:               string              // output name (e.g., "summary")
    object_type:        object_type         // State Object type produced
    schema_uri:         string?             // output schema
    description:        string
}

ContractGuarantee {
    property:           string              // e.g., "schema_valid", "confidence_above"
    value:              Value               // threshold or assertion value
    enforcement:        enum { HARD, SOFT } // hard = fail if violated; soft = warn
}
```

Contracts are declarative — they state requirements and guarantees without specifying how they are achieved. The procedure (Section 8) specifies the how; the contract specifies the what.

### 6.5 Dependency

A capability may depend on other capabilities, engine classes, memory surfaces, or a minimum system version. Dependencies are checked at installation time (Section 11.1.1) and monitored during the installed lifecycle.

```
Dependency {
    kind:               enum { CAPABILITY, ENGINE_CLASS, MEMORY_SURFACE, SYSTEM_VERSION }
    reference:          string              // cap OID or tag, engine class name, tier name, or semver
    required:           bool                // hard dependency (blocks installation) vs soft (warns)
    min_version:        string?             // minimum version, if applicable
    description:        string              // why this dependency exists
}
```

- **CAPABILITY** dependencies reference another capability by OID (exact match) or by tag (any capability providing that tag satisfies the dependency). Tag-based dependencies enable substitution: if the "entity_extraction" capability is retired and replaced by "entity_extraction_v2" with the same tags, dependents are satisfied without update.
- **ENGINE_CLASS** dependencies require a specific engine class to be available in the routing plane. For example, a capability whose procedure includes a `local_model` step depends on a `local_model` engine being registered.
- **MEMORY_SURFACE** dependencies require a memory tier or retrieval surface. A capability that performs semantic search depends on the L3 semantic retrieval tier being operational.
- **SYSTEM_VERSION** dependencies require a minimum Anunix version, ensuring that the kernel supports the features the capability needs.

### 6.6 CapabilityTrust

The trust record tracks the capability's validation history and production performance. It is updated by the kernel after every validation run and after every production execution.

```
CapabilityTrust {
    confidence:             float32         // [0.0, 1.0] aggregate confidence score
    last_validated_at:      timestamp?      // when validation suite last ran successfully
    validation_pass_count:  uint32          // total successful validation runs
    validation_fail_count:  uint32          // total failed validation runs
    benchmark_refs:         oid[]           // references to benchmark report State Objects
    freshness_class:        enum { FRESH, AGING, STALE, UNKNOWN }
    consecutive_successes:  uint32          // consecutive successful production executions
    consecutive_failures:   uint32          // consecutive failed production executions
    last_execution_at:      timestamp?      // when the capability was last used in production
    mean_latency_ms:        float64?        // rolling average production latency
    mean_cost_usd:          float64?        // rolling average production cost
}
```

**Confidence** is computed as a weighted combination of validation pass rate and production success rate:

```
confidence = 
    w_val * (validation_pass_count / (validation_pass_count + validation_fail_count))
  + w_prod * (consecutive_successes / (consecutive_successes + consecutive_failures + 1))
  + w_fresh * freshness_factor(last_validated_at)
```

The weights `w_val`, `w_prod`, `w_fresh` are system-configurable (defaults: 0.4, 0.4, 0.2).

**Freshness class** is determined by the age of the last validation relative to configurable thresholds:

| Class     | Condition                                     |
|-----------|-----------------------------------------------|
| `FRESH`   | Last validated within the freshness window (default: 7 days) |
| `AGING`   | Last validated within 2x the freshness window |
| `STALE`   | Last validated beyond 2x the freshness window, or a dependency has changed since validation |
| `UNKNOWN` | Never validated                               |

A capability with freshness class `STALE` remains installed but receives a reduced `capability_validation_score` in route scoring (Section 12.4), making the routing plane prefer fresher alternatives. If no fresher alternative exists, the stale capability is still used — but a re-validation is queued.

### 6.7 ProvenanceSummary

The provenance summary records how the capability was created. This is a summary — the full provenance record is in the State Object's governance section (RFC-0002 Section 4.6.1). The summary provides quick access to the most relevant provenance information without traversing the full provenance graph.

```
ProvenanceSummary {
    source_trace_refs:      oid[]           // execution traces this capability was derived from
    learning_method:        enum { AUTHORED, EXTRACTED, REFINED, IMPORTED }
    original_task_context:  string?         // description of the task class
    author_cell:            oid?            // cell that created this capability
    parent_capability_ref:  oid?            // if REFINED: the capability this was refined from
    import_source_node:     oid?            // if IMPORTED: the node it was imported from
}
```

- **AUTHORED**: A human or system explicitly defined the procedure and validation suite.
- **EXTRACTED**: The `capability.extract` cell (Section 11.1.7) analyzed execution traces and generated a draft capability.
- **REFINED**: The `capability.refine` cell (Section 11.1.2) updated an existing capability based on new evidence.
- **IMPORTED**: The capability was received from a remote node via the Network Plane (Section 17).

### 6.8 Kernel Behavior for `capability` Type

The kernel provides the following type-specific behaviors for `capability` objects that are not available for other State Object types:

**Routing plane integration.** When a capability transitions to `installed`, the kernel automatically registers it as an `installed_capability` engine in the RFC-0005 Capability Registry. When it transitions to `suspended`, `superseded`, or `retired`, the kernel deregisters it. This registration is a kernel-internal operation — no application intervention is needed.

**Contract enforcement at admission.** When the routing plane considers an installed capability for a task, the kernel evaluates the capability's `InputContract` against the cell's declared inputs. This evaluation happens during feasibility filtering (Section 12.5), before route scoring. The kernel can reject a capability match without loading or executing the procedure.

**Supersession graph integrity.** The kernel maintains the `supersedes_ref` and `superseded_by_ref` fields as a directed acyclic graph. It enforces:
- No cycles: if A supersedes B, B cannot supersede A (directly or transitively).
- Bidirectional consistency: if A.supersedes_ref = B, then B.superseded_by_ref = A.
- Cascade notifications: when A supersedes B, the kernel marks B as `superseded` and notifies any cells watching B.

**Trust lifecycle management.** After every production execution of an installed capability, the kernel updates the trust record: incrementing success or failure counts, updating mean latency and cost, and recomputing confidence. If consecutive failures exceed a configurable threshold (default: 3), the kernel transitions the capability to `suspended` and emits a `capability.auto_suspended` event. This is a kernel-level circuit breaker — it does not require application intervention.

**Placement optimization.** The kernel co-locates the capability object, its procedure, and its validation suite in the same storage region when possible. This reduces I/O latency when the executor adapter loads the procedure at execution time. The Memory Control Plane (RFC-0004) uses the capability's `installed` status as a placement hint to keep these artifacts in hot storage (L1/L2).

### 6.9 Example Capability Object

```
{
    // Identity (RFC-0002 Section 4.2)
    "oid": "cap_01961f3a-7c00-7000-8000-000000000042",
    "content_hash": "sha256:a1b2c3d4...",
    "version": 3,

    // Type & Schema (RFC-0002 Section 4.3)
    "object_type": "capability",
    "schema_uri": "anunix:schema/capability/v1",
    "schema_version": "1.0.0",

    // Payload
    "payload": {
        "capability_name": "meeting_summarization",
        "capability_version": "1.2.0",
        "status": "installed",
        "procedure_ref": "so_01961f3a-7c00-7000-8000-000000000100",
        "validation_suite_ref": "so_01961f3a-7c00-7000-8000-000000000101",
        "input_contract": {
            "slots": [
                {
                    "name": "transcript",
                    "required": true,
                    "accepted_types": ["structured_data", "byte_data"],
                    "schema_uri": "anunix:schema/transcript/v1",
                    "min_trust": null,
                    "description": "Meeting transcript to summarize"
                },
                {
                    "name": "context_docs",
                    "required": false,
                    "accepted_types": ["structured_data"],
                    "schema_uri": null,
                    "min_trust": null,
                    "description": "Optional background documents for context"
                }
            ],
            "constraints": []
        },
        "output_contract": {
            "slots": [
                {
                    "name": "summary",
                    "object_type": "structured_data",
                    "schema_uri": "anunix:schema/meeting_summary/v1",
                    "description": "Structured meeting summary"
                },
                {
                    "name": "action_items",
                    "object_type": "structured_data",
                    "schema_uri": "anunix:schema/action_items/v1",
                    "description": "Extracted action items with assignees"
                }
            ],
            "guarantees": [
                {
                    "property": "schema_valid",
                    "value": true,
                    "enforcement": "HARD"
                },
                {
                    "property": "action_items_extracted",
                    "value": true,
                    "enforcement": "SOFT"
                }
            ]
        },
        "dependencies": [
            {
                "kind": "ENGINE_CLASS",
                "reference": "local_model",
                "required": true,
                "min_version": null,
                "description": "Requires a local model for summarization and extraction"
            },
            {
                "kind": "ENGINE_CLASS",
                "reference": "retrieval_service",
                "required": false,
                "min_version": null,
                "description": "Retrieval service for context augmentation (optional)"
            }
        ],
        "provenance_summary": {
            "source_trace_refs": [
                "trace_01961f3a-7c00-7000-8000-000000000200",
                "trace_01961f3a-7c00-7000-8000-000000000201",
                "trace_01961f3a-7c00-7000-8000-000000000202"
            ],
            "learning_method": "EXTRACTED",
            "original_task_context": "Summarize weekly engineering team meetings with action item extraction",
            "author_cell": "cell_01961f3a-7c00-7000-8000-000000000300",
            "parent_capability_ref": "cap_01961f3a-7c00-7000-8000-000000000041",
            "import_source_node": null
        },
        "trust": {
            "confidence": 0.91,
            "last_validated_at": "2026-04-13T18:00:00Z",
            "validation_pass_count": 12,
            "validation_fail_count": 1,
            "benchmark_refs": ["so_01961f3a-7c00-7000-8000-000000000400"],
            "freshness_class": "FRESH",
            "consecutive_successes": 27,
            "consecutive_failures": 0,
            "last_execution_at": "2026-04-13T21:30:00Z",
            "mean_latency_ms": 4200.0,
            "mean_cost_usd": 0.0045
        },
        "supersedes_ref": "cap_01961f3a-7c00-7000-8000-000000000041",
        "superseded_by_ref": null,
        "tags": ["meeting_summarization", "action_item_extraction", "structured_output"]
    }
}
```

This example shows a `meeting_summarization` capability at version 1.2.0 that has been extracted from three execution traces, validated 12 times, and has 27 consecutive successful production executions. It supersedes an earlier version (cap_...0041) and requires a `local_model` engine. The output contract guarantees schema-valid structured output with a soft guarantee on action item extraction.

---

## 7. Capability Lifecycle

A Capability Object moves through a defined sequence of states from creation to retirement. The kernel enforces the lifecycle — applications invoke lifecycle cells (Section 11) which trigger kernel-managed transitions. No application can set a capability's status directly.

### 7.1 Lifecycle States

```
                    ┌─────────┐
                    │  DRAFT  │
                    └────┬────┘
                         │ validate()
                         ▼
                    ┌─────────────┐
              ┌─────│ VALIDATING  │
              │     └──────┬──────┘
              │ fail       │ pass
              ▼            ▼
         ┌────────┐  ┌─────────────┐
         │ DRAFT  │  │  VALIDATED  │──────────────┐
         │(reset) │  └──────┬──────┘              │
         └────────┘         │ install()            │ supersede()
                            ▼                      │ (without installing)
                    ┌─────────────┐                │
              ┌────→│  INSTALLED  │←──┐            │
              │     └──┬──┬──┬───┘   │            │
              │        │  │  │       │            │
              │  fail  │  │  │ revalidate()       │
              │  ┌─────┘  │  │  pass │            │
              │  ▼        │  └───────┘            │
              │ ┌──────────┐                      │
              │ │SUSPENDED │                      │
              │ └──────┬───┘                      │
              │        │ revalidate() pass        │
              └────────┘                          │
                                                  │
                    ┌─────────────┐               │
                    │ SUPERSEDED  │←──────────────┘
                    └─────────────┘    (from installed
                                        or validated)
                    ┌─────────────┐
                    │   RETIRED   │←── (from installed,
                    └─────────────┘     suspended, or
                                        superseded)
```

### 7.2 State Transition Rules

| From          | To            | Trigger                   | Preconditions                                                     | Effects                                                            |
|---------------|---------------|---------------------------|-------------------------------------------------------------------|--------------------------------------------------------------------|
| `draft`       | `validating`  | `capability.benchmark`    | Procedure and validation suite refs are valid                     | Validation suite execution begins                                  |
| `validating`  | `validated`   | Validation passes         | All required validation modes pass acceptance criteria             | Trust record updated; `capability.validated` event emitted          |
| `validating`  | `draft`       | Validation fails          | Any required validation mode fails                                 | Trust record updated; failure anti-knowledge created (RFC-0004 16.4)|
| `validated`   | `installed`   | `capability.install`      | All dependencies satisfied; no conflicting installed capability    | Registered in routing plane; executor adapter bound; event emitted  |
| `validated`   | `superseded`  | `capability.supersede`    | A newer capability declares `supersedes_ref` pointing to this one | Superseded_by_ref set; provenance recorded; event emitted           |
| `installed`   | `suspended`   | Validation failure or auto-suspend | Consecutive failures exceed threshold, or explicit invalidation | Deregistered from routing; dependents marked stale; event emitted  |
| `installed`   | `superseded`  | `capability.supersede`    | Successor is at least `validated`                                 | Deregistered from routing; superseded_by_ref set; event emitted    |
| `installed`   | `validating`  | `capability.benchmark`    | Re-validation requested                                           | Remains in routing during validation unless validation fails        |
| `installed`   | `retired`     | `capability.retire`       | No installed dependents without a successor                       | Deregistered from routing; demoted to L4; event emitted            |
| `suspended`   | `installed`   | Re-validation passes      | Validation suite passes; dependencies still satisfied             | Re-registered in routing; trust record updated; event emitted      |
| `suspended`   | `retired`     | `capability.retire`       | Explicit retirement                                               | Demoted to L4/archive; event emitted                               |
| `superseded`  | `retired`     | `capability.retire`       | Explicit retirement                                               | Demoted to archive; event emitted                                  |

All transitions are atomic from the perspective of the routing plane: a capability is never in an intermediate state where it is partially registered or partially deregistered.

### 7.3 State Semantics

**Draft.** The capability envelope exists but is not operational. The procedure may be incomplete or the validation suite may not yet exist. The capability is visible in queries but is not considered by the routing plane. Permitted operations: edit payload fields, update procedure or validation suite references, trigger validation. The capability is stored in L2 (durable local). No routing plane registration exists.

**Validating.** The validation suite is executing against the capability's procedure. If the capability was previously `installed`, it remains in the routing plane during re-validation — only a validation failure will remove it. If the capability was `draft`, it is not routable. The trust record's `validation_pass_count` or `validation_fail_count` is incremented when validation completes.

**Validated.** The capability has passed its validation suite and is eligible for installation. It is not yet routable — installation is a separate, deliberate step. This separation allows review: a human or policy gate can inspect the validated capability before approving installation. The capability is stored in L2 and L3 (semantic discovery index). The trust record reflects the validation outcome.

**Installed.** The capability is registered with the routing plane as an `installed_capability` engine. The routing plane considers it during feasibility filtering and route scoring for matching tasks. The executor adapter is bound and ready to interpret the procedure. The capability is stored in L1 (hot routing lookup), L2, L3, and L4. Production execution outcomes update the trust record. The kernel monitors consecutive failures for auto-suspension.

**Suspended.** The capability has been temporarily removed from routing due to validation failure, excessive production failures (auto-suspension), a policy change, or a dependency issue. It is not routable but is not replaced — it can be restored by re-validation. Dependents are notified of the suspension and marked stale. The capability remains in L2 and L4 but is removed from L1 and L3.

**Superseded.** A newer capability version has replaced this one. The `superseded_by_ref` field points to the successor. The capability is not routable and cannot be re-installed (installation would require creating a new version). It is preserved for provenance: queries can trace the evolution from the superseded version to the current version. Stored in L4 (long-term history).

**Retired.** The capability is permanently removed from active use. It is not routable, cannot be re-installed, and cannot be superseded (it is already out of service). It is preserved for provenance and audit purposes. Stored in L4 or archived. Retirement is the terminal state — no transitions out of `retired` are permitted.

---

## 8. Procedure Model

The procedure is the operational heart of a capability: it defines *what the capability does* as a sequence of declarative steps. Procedures are not imperative code — they are templates that the executor adapter interprets to create child cells. This separation is deliberate: the procedure declares intent and structure; the routing plane and cell runtime handle execution.

### 8.1 Procedure Definition

A procedure is a `structured_data` State Object referenced by the capability's `procedure_ref` field. It is separate from the capability envelope so that:

- Procedures can be versioned independently of the capability's trust and metadata.
- The executor adapter loads the procedure only at execution time, not at routing time (the capability envelope is sufficient for routing decisions).
- Multiple capability versions can reference the same procedure if only the validation or metadata changed.

### 8.2 Procedure Schema

```
Procedure {
    procedure_id:       string          // human-readable identifier
    version:            string          // matches parent capability version
    description:        string          // what this procedure does
    parameters:         Parameter[]     // invocation-time input bindings (Section 8.3)
    steps:              ProcedureStep[] // ordered execution steps
    error_strategy:     ErrorStrategy   // global error handling policy
}

ProcedureStep {
    step_name:          string          // unique within the procedure
    cell_type:          string          // RFC-0003 cell type (e.g., "task.execution")
    engine_preference:  EnginePreference // which engine class to prefer
    input_bindings:     Binding[]       // where this step gets its inputs
    output_name:        string          // name for this step's output in the binding namespace
    validation:         StepValidation? // optional per-step validation
    fallback:           FallbackPolicy? // what to do if this step fails
    timeout:            duration?       // maximum execution time for this step
    condition:          string?         // predicate for conditional execution (Section 8.3)
}

EnginePreference {
    preferred_class:    string          // first-choice engine class
    fallback_classes:   string[]        // ordered alternatives
    constraints:        map<string, Value> // e.g., {"min_context_window": 32000}
}

Binding {
    source:             enum { CAPABILITY_INPUT, STEP_OUTPUT, CONSTANT }
    source_name:        string          // parameter name or step output name
    target_field:       string          // input field on the target cell
}

StepValidation {
    validation_type:    string          // e.g., "schema_check", "confidence_threshold"
    threshold:          Value?          // threshold value, if applicable
    on_failure:         enum { FAIL_STEP, RETRY, FALLBACK, SKIP }
}

FallbackPolicy {
    max_retries:        uint32          // maximum retry attempts
    retry_delay:        duration?       // delay between retries
    fallback_step:      string?         // alternative step to execute on exhaustion
}

ErrorStrategy {
    default_on_failure: enum { ABORT, SKIP_AND_CONTINUE, COMPENSATE }
    compensation_step:  string?         // step to run for cleanup on abort
}

Parameter {
    name:               string          // parameter name (used in bindings)
    param_type:         object_type     // expected State Object type
    required:           bool            // must be provided at invocation
    default_ref:        oid?            // default value if not provided
    description:        string          // human-readable purpose
}
```

### 8.3 Parameterization

Procedures declare parameters that are bound at invocation time. When the routing plane selects an installed capability for a cell, the executor adapter resolves parameters from the cell's input bindings:

1. The adapter reads the procedure's `parameters` list.
2. For each parameter, it looks for a matching input in the cell's declared inputs (matched by name or by type).
3. Required parameters that cannot be resolved cause the execution to fail with `ERR_MISSING_PARAMETER`.
4. Optional parameters without a match use `default_ref` if provided, or are omitted from the binding namespace.

**Conditional execution.** Steps may include a `condition` field — a predicate expression evaluated against the binding namespace. If the condition evaluates to false, the step is skipped and its output is set to null in the namespace. This enables capabilities that adapt their behavior based on input properties (e.g., "run the context retrieval step only if context_docs were not provided as input").

### 8.4 Example Procedure

The following procedure defines a five-step meeting summarization capability:

```
{
    "procedure_id": "meeting_summarization_proc",
    "version": "1.2.0",
    "description": "Summarize a meeting transcript with action item extraction",
    "parameters": [
        {
            "name": "transcript",
            "param_type": "structured_data",
            "required": true,
            "default_ref": null,
            "description": "The meeting transcript to summarize"
        },
        {
            "name": "context_docs",
            "param_type": "structured_data",
            "required": false,
            "default_ref": null,
            "description": "Optional background documents"
        }
    ],
    "steps": [
        {
            "step_name": "retrieve_context",
            "cell_type": "task.retrieval",
            "engine_preference": {
                "preferred_class": "retrieval_service",
                "fallback_classes": [],
                "constraints": {}
            },
            "input_bindings": [
                {"source": "CAPABILITY_INPUT", "source_name": "transcript", "target_field": "query_source"},
                {"source": "CAPABILITY_INPUT", "source_name": "context_docs", "target_field": "seed_documents"}
            ],
            "output_name": "context",
            "validation": null,
            "fallback": {"max_retries": 1, "retry_delay": null, "fallback_step": null},
            "timeout": "30s",
            "condition": null
        },
        {
            "step_name": "extract_action_items",
            "cell_type": "task.execution",
            "engine_preference": {
                "preferred_class": "local_model",
                "fallback_classes": ["remote_model"],
                "constraints": {"min_context_window": 16000}
            },
            "input_bindings": [
                {"source": "CAPABILITY_INPUT", "source_name": "transcript", "target_field": "input"},
                {"source": "STEP_OUTPUT", "source_name": "context", "target_field": "context"}
            ],
            "output_name": "action_items",
            "validation": {
                "validation_type": "schema_check",
                "threshold": null,
                "on_failure": "RETRY"
            },
            "fallback": {"max_retries": 2, "retry_delay": "1s", "fallback_step": null},
            "timeout": "60s",
            "condition": null
        },
        {
            "step_name": "generate_summary",
            "cell_type": "task.execution",
            "engine_preference": {
                "preferred_class": "local_model",
                "fallback_classes": ["remote_model"],
                "constraints": {"min_context_window": 32000}
            },
            "input_bindings": [
                {"source": "CAPABILITY_INPUT", "source_name": "transcript", "target_field": "input"},
                {"source": "STEP_OUTPUT", "source_name": "context", "target_field": "context"},
                {"source": "STEP_OUTPUT", "source_name": "action_items", "target_field": "action_items"}
            ],
            "output_name": "summary",
            "validation": {
                "validation_type": "schema_check",
                "threshold": null,
                "on_failure": "RETRY"
            },
            "fallback": {"max_retries": 2, "retry_delay": "1s", "fallback_step": null},
            "timeout": "90s",
            "condition": null
        },
        {
            "step_name": "validate_schema",
            "cell_type": "task.validation",
            "engine_preference": {
                "preferred_class": "validation_service",
                "fallback_classes": [],
                "constraints": {}
            },
            "input_bindings": [
                {"source": "STEP_OUTPUT", "source_name": "summary", "target_field": "object_to_validate"},
                {"source": "STEP_OUTPUT", "source_name": "action_items", "target_field": "secondary_object"}
            ],
            "output_name": "validation_result",
            "validation": null,
            "fallback": null,
            "timeout": "15s",
            "condition": null
        },
        {
            "step_name": "cross_check_transcript",
            "cell_type": "task.validation",
            "engine_preference": {
                "preferred_class": "local_model",
                "fallback_classes": ["remote_model"],
                "constraints": {}
            },
            "input_bindings": [
                {"source": "STEP_OUTPUT", "source_name": "summary", "target_field": "generated_output"},
                {"source": "CAPABILITY_INPUT", "source_name": "transcript", "target_field": "source_material"}
            ],
            "output_name": "cross_check_result",
            "validation": {
                "validation_type": "confidence_threshold",
                "threshold": 0.85,
                "on_failure": "FAIL_STEP"
            },
            "fallback": null,
            "timeout": "60s",
            "condition": null
        }
    ],
    "error_strategy": {
        "default_on_failure": "ABORT",
        "compensation_step": null
    }
}
```

This procedure retrieves relevant context, extracts action items, generates a summary using the transcript and action items as input, validates both outputs against their schemas, and cross-checks the summary against the original transcript. Each step declares its engine preference (favoring `local_model` with `remote_model` as fallback) and its validation behavior.

### 8.5 Procedure Rules

1. **Procedures must be declarative.** A procedure defines what steps to run, in what order, with what inputs. It does not contain executable code, shell commands, or model prompts. The engine class selected by the routing plane determines how each step is executed.

2. **Reference engine classes, not engine IDs.** Steps specify `engine_preference` by class name (e.g., `local_model`), not by specific engine identifier (e.g., `eng_01JR_claude_sonnet`). This ensures portability across nodes and over time as engines are added or removed. An exception is made when a cell's execution policy explicitly requires a specific engine (policy-locked routing per RFC-0005 Section 11.7).

3. **At least one validation step.** Every procedure must include at least one step with a non-null `validation` field or a dedicated `task.validation` step. A procedure with no validation cannot be part of a capability — without validation, there is no way to verify that the capability produces correct output.

4. **Changes require a new capability version.** Modifying a procedure creates a new `structured_data` State Object with a new OID. The capability must be updated to reference the new procedure via `capability.refine` (Section 11.1.2), which produces a new capability version. In-place mutation of an installed capability's procedure is not permitted.

5. **Steps must be acyclic.** No step may bind to the output of a step that follows it in the sequence. The binding graph must be a directed acyclic graph (DAG). The kernel validates this at capability creation time and rejects cyclic procedures.

---

## 9. Validation Suite

The validation suite defines how a capability proves it works. It is the test harness for operational competence — without it, a capability is an unverified claim. The validation suite is the gatekeeper between `draft` and `validated`, and between `validated` and `installed`.

### 9.1 Purpose

Every installed capability has passed a validation suite. This is the foundational guarantee of the capability model: the system does not install unverified procedures. The validation suite serves three roles:

1. **Gate for installation.** A capability cannot be installed without passing validation. This prevents untested procedures from entering the routing plane.
2. **Ongoing health check.** Installed capabilities are periodically re-validated (Section 7.3). Performance degradation is detected by running the same suite against the current procedure.
3. **Regression detection.** When a capability is refined (new version), the validation suite ensures the new version is at least as good as the old one.

### 9.2 ValidationSuite Schema

```
ValidationSuite {
    suite_id:               string          // human-readable identifier
    version:                string          // suite version
    target_capability:      oid             // the capability this suite validates
    modes:                  ValidationMode[] // validation modes to run (Section 9.3)
    acceptance_criteria:    AcceptanceCriteria
}

ValidationMode {
    mode:                   enum { SCHEMA_CHECK, GOLDEN_SET, CROSS_VALIDATION,
                                   BENCHMARK, REGRESSION }
    config:                 map<string, Value> // mode-specific configuration
    required:               bool            // must pass for overall validation to pass
    weight:                 float32         // contribution to overall score (0.0-1.0)
}

AcceptanceCriteria {
    min_overall_score:      float32         // minimum weighted score to pass (default: 0.8)
    required_modes_must_pass: bool          // all required modes must individually pass (default: true)
    max_allowed_failures:   uint32          // maximum individual case failures (default: 0 for golden_set)
}
```

### 9.3 Validation Modes

#### 9.3.1 Schema Check (`SCHEMA_CHECK`)

Verifies that the capability's outputs conform to the declared output contract. The validator runs the procedure against a set of representative inputs and checks that every output matches its declared `object_type` and `schema_uri`.

```
SchemaCheckConfig {
    test_inputs:        oid[]           // State Objects to use as test inputs
    strict:             bool            // if true, reject outputs with extra fields
}
```

Schema check is mandatory for all capabilities. It is the minimum validation — without it, there is no guarantee that the capability produces well-formed output.

#### 9.3.2 Golden Set (`GOLDEN_SET`)

Tests the capability against known input/output pairs where the expected output quality is established. Each golden case provides an input, a reference output, and a quality metric.

```
GoldenSetConfig {
    cases:              GoldenCase[]
    scoring_method:     enum { EXACT_MATCH, SEMANTIC_SIMILARITY, CUSTOM_METRIC }
    min_case_score:     float32         // minimum score per case (default: 0.7)
    min_aggregate_score: float32        // minimum average across cases (default: 0.8)
}

GoldenCase {
    case_id:            string
    input_refs:         oid[]           // input State Objects
    reference_output_ref: oid           // expected output (for comparison)
    metric:             string          // quality metric name
    min_score:          float32?        // per-case minimum (overrides config default)
}
```

Golden set validation is required before installation. It is the primary quality gate — a capability that produces schema-valid but semantically incorrect output will fail golden set validation.

#### 9.3.3 Cross-Validation (`CROSS_VALIDATION`)

The capability's outputs are evaluated by an independent engine (typically a different model or a human review queue) to detect systematic errors that golden set testing might miss.

```
CrossValidationConfig {
    validator_engine_class: string      // engine class for the independent validator
    evaluation_criteria:    string[]    // what to evaluate (e.g., ["accuracy", "completeness"])
    min_approval_rate:      float32     // minimum fraction of outputs approved (default: 0.9)
    sample_size:            uint32      // number of cases to evaluate (default: 10)
}
```

Cross-validation is optional but recommended for capabilities that produce natural language or creative outputs where schema checking is insufficient.

#### 9.3.4 Benchmark (`BENCHMARK`)

Measures the capability's performance against latency, cost, and quality thresholds. This mode ensures the capability is not only correct but also practical.

```
BenchmarkConfig {
    test_inputs:            oid[]       // inputs for benchmarking
    max_latency_ms:         uint64?     // maximum acceptable latency
    max_cost_usd:           float64?    // maximum acceptable cost per execution
    min_quality_score:      float32?    // minimum quality (from golden set or cross-validation)
    iterations:             uint32      // number of runs for statistical significance (default: 5)
}
```

#### 9.3.5 Regression (`REGRESSION`)

Compares the capability's outputs against a previous version to ensure the new version is not worse. This mode is relevant only for refined capabilities that supersede a previous version.

```
RegressionConfig {
    baseline_capability_ref: oid        // the previous version to compare against
    test_inputs:             oid[]      // inputs for comparison
    regression_threshold:    float32    // maximum allowed quality degradation (default: 0.05)
    metrics:                 string[]   // metrics to compare
}
```

### 9.4 ValidationReport Schema

A validation report is a `structured_data` State Object produced by running a validation suite. Reports are immutable records of validation outcomes — they are the evidence trail for capability trust.

```
ValidationReport {
    report_id:              string
    capability_ref:         oid         // the capability that was validated
    suite_ref:              oid         // the validation suite that was run
    capability_version:     string      // version of the capability at validation time
    timestamp:              datetime    // when validation was performed
    overall_result:         enum { PASS, FAIL, PARTIAL }
    overall_score:          float32     // weighted aggregate score
    mode_results:           ModeResult[]
    execution_cost:         ResourceUsage // total resources consumed by validation
    validator_cell:         oid         // the cell that ran the validation
    duration_ms:            uint64      // total validation wall-clock time
}

ModeResult {
    mode:                   string      // validation mode name
    result:                 enum { PASS, FAIL, SKIP }
    score:                  float32?    // mode-specific score
    details:                string      // human-readable summary
    case_results:           CaseResult[]? // per-case results (for golden_set, regression)
}

CaseResult {
    case_id:                string
    result:                 enum { PASS, FAIL }
    score:                  float32?
    expected:               string?     // expected output summary
    actual:                 string?     // actual output summary
    details:                string?     // diagnostic information
}
```

### 9.5 Validation Rules

1. **Schema check is mandatory.** Every validation suite must include at least one `SCHEMA_CHECK` mode marked as `required`. A capability that cannot demonstrate schema-valid output is not ready for validation.

2. **Golden set is required for installation.** The `capability.install` cell (Section 11.1.1) requires that the most recent passing validation report include a `GOLDEN_SET` mode with `result: PASS`. Schema correctness alone is insufficient for installation — the capability must demonstrate quality on known examples.

3. **Reports are persisted as State Objects.** Every validation report is stored as a `structured_data` State Object with full provenance (the `CREATED` event records the validator cell, the validation suite, and the capability version). Reports are never deleted — they form the audit trail for capability trust decisions.

4. **Failed validation produces anti-knowledge.** When validation fails, the kernel creates an anti-knowledge record per RFC-0004 Section 16.4. This record documents what the capability got wrong and prevents the memory system from treating the capability's failed outputs as valid knowledge.

5. **Validation must be reproducible.** Running the same validation suite against the same capability version with the same inputs must produce consistent results. Non-deterministic modes (e.g., cross-validation with a stochastic model) must include a sufficient sample size (configurable, default: 10) to achieve statistical significance. The acceptance criteria should account for variance.

---

## 10. Capability Bundle

A Capability Bundle groups the capability object, its procedure definition, validation suite, most recent passing validation reports, benchmark history, and dependency manifest into a coherent, distributable unit. Bundles are the atomic unit of capability transfer between nodes via RFC-0006.

### 10.1 Purpose

Capabilities are composed of multiple State Objects: the capability envelope, the procedure, the validation suite, and multiple reports. Distributing these individually would be fragile — a receiving node might get the capability without its validation suite, or with an outdated procedure. The bundle ensures that a complete, self-consistent set of artifacts travels together.

### 10.2 Bundle Schema

```
CapabilityBundle {
    bundle_id:              string          // unique bundle identifier
    bundle_version:         string          // matches the capability version
    capability_ref:         oid             // the capability object
    procedure_ref:          oid             // the procedure definition
    validation_suite_ref:   oid             // the validation criteria
    validation_report_refs: oid[]           // most recent passing reports
    benchmark_refs:         oid[]           // benchmark history
    dependency_manifest:    Dependency[]    // resolved dependencies
    created_at:             datetime
    created_by:             oid             // cell or node that created the bundle
    content_hash:           sha256          // hash of all bundled artifact content hashes
    signature:              bytes?          // node signature for attestation (Section 17.3)
}
```

The `content_hash` is computed over the content hashes of all referenced artifacts, providing a single integrity check for the entire bundle. If any artifact is modified, the bundle hash changes.

### 10.3 Bundle as Distribution Unit

When publishing capabilities to remote nodes (Section 17), the bundle is the atomic transfer unit:

1. The publishing node creates the bundle, including all referenced artifacts.
2. The bundle is signed with the publishing node's key.
3. The receiving node verifies the signature and content hash.
4. The receiving node extracts the artifacts and stores them as local State Objects.
5. The receiving node runs the validation suite locally before considering installation.

Bundles are immutable. Updating a capability produces a new bundle — the old bundle remains valid as a historical record.

### 10.4 Bundle Rules

1. A bundle must contain a complete, self-consistent set of artifacts. All OID references within the bundle must resolve to artifacts included in the bundle or to artifacts that are universally available (e.g., system schemas).
2. Bundle integrity is verifiable via the `content_hash`. A receiving node must verify the hash before processing.
3. Bundles may be signed for attestation. The signature covers the `content_hash` and the bundle metadata, providing a cryptographic guarantee that the bundle was created by the claimed node.
4. A bundle must reference a capability in at least `validated` status. Bundles of `draft` capabilities are not permitted — distributing unvalidated capabilities would undermine the trust model.
5. Bundles are immutable once created. Modifying any bundled artifact invalidates the content hash and signature.

---

## 11. New Execution Cell Types

This section amends RFC-0003 Section 7.1 to add a new cell type family for capability lifecycle management. These cells are system operations — they manage the capability lifecycle, not application workloads.

### 11.1 Cell Type Family: `capability.*`

Seven new cell types, each responsible for a specific lifecycle operation:

#### 11.1.1 `capability.install`

**Purpose:** Validate a capability and register it with the routing plane as an available execution path.

**Inputs:** `capability_ref` (oid), `force_revalidate` (bool, default: false).

**Process:**
1. Verify the capability is in `validated` status. If `force_revalidate` is true, run the validation suite first.
2. Verify a recent validation report exists with `GOLDEN_SET` mode passing.
3. Resolve all dependencies: for each `Dependency` in the capability's payload, verify the referenced resource is available and satisfies version constraints.
4. Check for conflicts: if another installed capability provides the same tags with higher trust, warn but proceed (both can coexist).
5. Register the capability as an `installed_capability` engine in the RFC-0005 Capability Registry.
6. Create the executor adapter binding (Section 12.6).
7. Transition the capability to `installed` status.
8. Update memory tier placement: promote to L1 (hot routing).
9. Emit `capability.installed` event.

**Outputs:** `installation_report` (structured_data) with dependency resolution details, registry entry, and any warnings.

**Failure:** If dependency resolution fails, the capability remains `validated` with an error report. If registration fails, the transition is rolled back.

#### 11.1.2 `capability.refine`

**Purpose:** Create a new version of a capability with an updated procedure or validation suite.

**Inputs:** `capability_ref` (oid), `updated_procedure_ref` (oid, optional), `updated_validation_ref` (oid, optional), `refinement_reason` (string).

**Process:**
1. Create a new capability State Object with the version incremented.
2. Copy unchanged fields from the parent capability.
3. Apply updated procedure and/or validation references.
4. Set status to `draft`.
5. Set `supersedes_ref` to the parent capability (provisional — not yet superseded).
6. Set `provenance_summary.learning_method` to `REFINED` and `parent_capability_ref` to the parent.
7. Emit `capability.refined` event.

**Outputs:** `new_capability_ref` (oid) — the new draft capability.

**Note:** The refined capability enters `draft` status and must go through the full validation → installation lifecycle. Refinement does not auto-install.

#### 11.1.3 `capability.invalidate`

**Purpose:** Suspend an installed capability due to validation failure or policy violation.

**Inputs:** `capability_ref` (oid), `reason` (string), `validation_report_ref` (oid, optional).

**Process:**
1. Transition the capability to `suspended` status.
2. Deregister from the routing plane registry.
3. Remove from L1 (hot routing) memory tier.
4. Record the reason and optional report reference in provenance.
5. Check dependents: mark any installed capabilities that depend on this one as `stale` in their freshness class.
6. Emit `capability.invalidated` event.

#### 11.1.4 `capability.publish`

**Purpose:** Distribute a capability bundle to peer nodes via the Network Plane.

**Inputs:** `capability_ref` (oid), `target_nodes` (oid[], optional — default: all trusted peers), `trust_zone_filter` (string, optional — minimum trust zone for targets).

**Process:**
1. Verify the capability is in `installed` or `validated` status.
2. Verify the capability's policy allows export (some capabilities may be local-only).
3. Create or retrieve the capability bundle (Section 10).
4. Sign the bundle with the local node's key.
5. Distribute via RFC-0006 replication mechanisms, filtered by target nodes and trust zone.
6. Emit `capability.published` event.

**Outputs:** `distribution_report` (structured_data) — which nodes received the bundle, any failures.

#### 11.1.5 `capability.benchmark`

**Purpose:** Run a capability's validation suite and produce a validation/benchmark report.

**Inputs:** `capability_ref` (oid), `benchmark_config` (structured_data, optional — override parameters).

**Process:**
1. Load the capability's validation suite.
2. If `benchmark_config` is provided, merge overrides into the suite configuration.
3. Execute all validation modes.
4. Create a `ValidationReport` State Object with full results.
5. Update the capability's `CapabilityTrust` fields (pass/fail counts, confidence, freshness).
6. If the capability is `installed` and validation fails, trigger the auto-suspension logic (transition to `suspended`).
7. If validation fails, create an anti-knowledge record (RFC-0004 Section 16.4).
8. Emit `capability.benchmarked` event.

**Outputs:** `validation_report_ref` (oid).

#### 11.1.6 `capability.retire`

**Purpose:** Permanently remove a capability from active use, preserving it for provenance.

**Inputs:** `capability_ref` (oid), `reason` (string), `successor_ref` (oid, optional).

**Process:**
1. If the capability is `installed`, deregister from the routing plane registry.
2. Check dependents: if any installed capability depends on this one, require either a `successor_ref` or `force: true`.
3. If `successor_ref` is provided, update dependents to reference the successor.
4. Transition to `retired` status.
5. Demote memory tier placement to L4 (long-term history) or archive.
6. Emit `capability.retired` event.

#### 11.1.7 `capability.extract`

**Purpose:** Analyze execution traces to suggest a candidate capability.

**Inputs:** `trace_refs` (oid[], minimum 3), `task_description` (string).

**Process:**
1. Load the referenced execution traces.
2. Validate that the traces represent similar tasks (same cell types, similar input types, overlapping engine classes).
3. Identify common decomposition patterns across traces.
4. Generalize engine-specific assignments to engine class preferences.
5. Extract input/output contracts from actual trace inputs and outputs.
6. Generate a draft procedure definition from the common pattern.
7. Generate a draft validation suite from the validation that was actually performed in the traces.
8. Create a draft capability State Object.
9. Emit `capability.extracted` event.

**Outputs:** `draft_capability_ref` (oid), `extraction_report` (structured_data) with analysis details, confidence, and generalization notes.

**Note:** Extracted capabilities enter `draft` status and require human review. The extraction process is suggestive — it identifies patterns but cannot guarantee the generalized procedure will work on new inputs without validation.

### 11.2 Cell Type Rules

1. `capability.install` must verify all dependencies before transitioning to `installed`.
2. `capability.refine` must produce a new capability version, never mutate an existing one.
3. `capability.invalidate` must cascade staleness to all dependents.
4. `capability.publish` must check the target node's trust zone against the capability's export policy.
5. `capability.extract` requires a minimum of 3 traces from tasks with similar structure.
6. All capability cells emit provenance events linked to the capability's provenance record.
7. All capability cells require the `capability_admin` privilege in the calling cell's capability token (RFC-0002 Section 9.1).

---

## 12. Installed Capability as Engine Class

This section amends RFC-0005 to integrate installed capabilities into the routing plane as a native engine class. The routing plane must treat installed capabilities as first-class execution paths — not special cases, not workarounds, but a standard engine class with well-defined feasibility, scoring, and execution semantics.

### 12.1 New Engine Class: `installed_capability`

The ninth engine class, added to the set defined in RFC-0005 Section 7.1. An `installed_capability` engine represents a validated, installed Capability Object that the routing plane can select as an execution path for matching tasks.

Unlike other engine classes which represent atomic execution units (a single model, a single tool, a single service), `installed_capability` is a **composite engine**: it interprets a multi-step procedure and creates child cells for each step. This is analogous to the difference between a function and a script — other engines are functions; installed capabilities are scripts composed of function calls.

**Properties:**

| Property      | Value                                                                     |
|---------------|---------------------------------------------------------------------------|
| Determinism   | Variable — depends on the procedure's engine preferences                  |
| Latency       | Higher than atomic engines; lower than re-deriving a plan from scratch    |
| Cost          | Predictable — estimable from procedure steps and engine class cost models |
| Quality       | Validated — bounded by the validation suite's acceptance criteria         |
| Composability | High — capabilities can depend on other capabilities                      |

### 12.2 Routing Priority Amendment

The default routing preference order defined in RFC-0005 Section 11.3 is amended to insert `installed_capability` between `deterministic_tool` and `local_model`:

| Priority | Engine Class            | Rationale                                                              |
|----------|-------------------------|------------------------------------------------------------------------|
| 1        | `deterministic_tool`    | Exact, fast, zero ambiguity                                           |
| 2        | `installed_capability`  | Validated composite procedure — more reliable than raw model inference |
| 3        | `local_model`           | Local inference, no network dependency                                |
| 4        | `remote_model`          | Remote inference with network cost                                    |
| 5        | `retrieval_service`     | Retrieval-augmented path                                              |
| 6        | `graph_service`         | Graph traversal path                                                  |
| 7        | `validation_service`    | Validation-only path                                                  |
| 8        | `execution_service`     | Generic execution path                                                |
| 9        | `device_service`        | Hardware-specific path                                                |

**Rationale for position 2:** An installed capability represents operational knowledge that has been validated against a test suite. It is more predictable than raw model inference (the procedure is fixed, the validation criteria are known, the cost is estimable) but less rigid than a deterministic tool (it may use models internally, and its outputs vary with model behavior). This position ensures the system prefers reusing validated procedures before falling back to ad-hoc model inference — the same principle as preferring a tested function over writing inline code each time.

### 12.3 Capability Registry Entry

When a capability is installed, it is registered in the RFC-0005 Capability Registry (Section 8) with the following entry structure:

```
CapabilityRegistryEntry {
    engine_id:              string          // "cap_<oid>" format
    engine_class:           "installed_capability"
    status:                 "available" | "degraded" | "unavailable"
    capabilities:           string[]        // the capability's tags
    constraints: {
        supports_private_data:  bool
        requires_network:       bool
        max_input_size:         uint64?
        estimated_steps:        uint32      // number of procedure steps
    }
    cost_model: {
        kind:                   "composite_estimate"
        estimated_cost_usd:     float64     // per-execution estimate
        cost_per_step:          float64[]   // per-step cost estimates
    }
    quality_profile: {
        strengths:              string[]    // e.g., ["validated_procedure", "consistent_schema"]
        weaknesses:             string[]    // e.g., ["fixed_decomposition", "model_dependent"]
    }
    // Capability-specific fields:
    capability_ref:         oid             // the capability State Object
    procedure_ref:          oid             // the procedure definition
    trust: {
        validation_state:       string      // current capability status
        confidence:             float32     // from CapabilityTrust
        benchmark_count:        uint32      // number of benchmarks run
        last_validated_at:      timestamp
        consecutive_successes:  uint32
    }
}
```

The `trust` sub-object is unique to `installed_capability` entries. Other engine classes do not have trust metadata — they are assumed to be operational when registered. Installed capabilities carry trust because their quality depends on validation, not just availability.

### 12.4 Route Scoring Amendment

The RouteScore formula defined in RFC-0005 Section 13.3 is extended with a capability trust term:

```
RouteScore = 
    w_confidence * confidence_estimate
  - w_latency * latency_estimate
  - w_cost * cost_estimate
  + w_locality * locality_score
  + w_policy * policy_margin
  + w_history * historical_success_score
  + w_capability_trust * capability_validation_score    // NEW
```

Where `capability_validation_score` is:

```
capability_validation_score =
    trust.confidence
  * freshness_factor(trust.last_validated_at)
  * success_rate(trust.consecutive_successes, trust.consecutive_failures)
```

- `freshness_factor` returns 1.0 for `FRESH`, 0.8 for `AGING`, 0.5 for `STALE`, 0.0 for `UNKNOWN`.
- `success_rate` returns `consecutive_successes / (consecutive_successes + consecutive_failures + 1)`.

For non-capability engine classes, `capability_validation_score` is 0 and the term has no effect. The weight `w_capability_trust` is system-configurable (default: 0.15).

### 12.5 Feasibility Filtering

An installed capability is feasible for a task if all of the following hold (amending RFC-0005 Section 10):

1. **Tag match.** At least one of the capability's tags matches the task's required capabilities (from the cell's intent or routing hints).
2. **Input contract satisfied.** The cell's declared inputs can satisfy the capability's `InputContract`: correct types, compatible schemas, and adequate trust states for each required slot.
3. **Dependencies available.** All capability dependencies (other capabilities, engine classes, memory surfaces) are currently available in the local environment.
4. **Trust state adequate.** The capability's freshness class is not `UNKNOWN`. Capabilities with `STALE` freshness are feasible but receive reduced scoring (Section 12.4).
5. **Policy compatible.** The capability's execution requirements (network access, data sensitivity, resource budget) are compatible with the cell's execution policy (RFC-0003 Section 14).

If any condition fails, the capability is filtered out before route scoring. This is a fast-path check — the routing plane evaluates feasibility before computing the full RouteScore.

### 12.6 Installed Capability Executor Adapter

The executor adapter is the runtime component that translates a capability's procedure into concrete cell executions. It plugs into the RFC-0003 executor adapter framework (Section 31.1) alongside the existing adapters for processes, local models, remote models, and retrieval services.

**Execution flow:**

1. **Load.** Retrieve the capability object and its procedure definition from state.
2. **Bind parameters.** Resolve the procedure's declared parameters from the cell's input bindings. Fail with `ERR_MISSING_PARAMETER` if a required parameter cannot be resolved.
3. **Initialize binding namespace.** Create a namespace mapping parameter names to their resolved State Object references.
4. **Step execution.** For each step in the procedure, in order:
   a. Evaluate the step's `condition` (if present). If false, skip the step and set its output to null.
   b. Create a child cell of the declared `cell_type`.
   c. Apply the step's `engine_preference` as routing hints on the child cell.
   d. Resolve `input_bindings` from the binding namespace.
   e. Submit the child cell to the scheduler (RFC-0005 Section 15).
   f. Await completion. If the child cell fails, apply the step's `fallback` policy.
   g. If step `validation` is defined, evaluate it against the output. On failure, apply the `on_failure` policy (retry, fallback, skip, or fail).
   h. Register the step's output in the binding namespace under `output_name`.
5. **Aggregate outputs.** Collect the final outputs per the `OutputContract` slot names.
6. **Lightweight validation.** Run a schema check against the output contract (not the full validation suite — that is for benchmarking, not production execution).
7. **Return.** Return outputs to the calling cell.
8. **Feedback.** Update the capability's `CapabilityTrust` record: increment success or failure count, update mean latency and cost, recompute confidence.

**Error handling:** If a step fails and its `FallbackPolicy` is exhausted (max retries reached, no fallback step), the procedure's `ErrorStrategy` governs the response:
- `ABORT`: The entire capability execution fails. The calling cell receives an error.
- `SKIP_AND_CONTINUE`: The failed step's output is set to null and execution continues. Downstream steps that depend on the failed step's output must handle null inputs.
- `COMPENSATE`: The compensation step (if defined) is executed before aborting, allowing cleanup.
---

## 13. Capability-Aware Memory

The Capability Object is a first-class participant in the Memory Control Plane defined by RFC-0004. Capabilities have distinct storage needs at each lifecycle stage, distinct freshness semantics, and distinct graph relations. This section specifies the memory integration in full.

### 13.1 New Memory Object Classes

The following State Object classes are added to the set defined in RFC-0004 Section 8.1:

- `capability.definition` — the envelope containing identity, contracts, metadata, lifecycle state, and version history for a single capability
- `capability.procedure` — the ordered list of steps, engine class assignments, and branching hints that describe how the capability is executed
- `capability.validation_suite` — the set of test cases, assertions, and benchmark specifications used to validate the capability
- `capability.validation_report` — the immutable record of a single validation run, including pass/fail per case, timing, cost, and confidence scores
- `capability.benchmark` — a standalone performance measurement record capturing latency, cost, confidence, and engine class used, timestamped and versioned
- `capability.bundle` — the portable archive containing a definition, its current procedure, validation suite, and most recent passing validation report, suitable for network distribution

Each class follows the State Object Model envelope conventions defined in RFC-0002 and carries a `CapabilityID` with the `cap_` prefix.

### 13.2 Trust State Tracking

Capability objects map onto the memory validation states defined in RFC-0004 Section 16.2 with capability-specific semantics:

| Validation State | Capability Semantics |
|---|---|
| `unvalidated` | Capability definition exists but validation suite has never been run or does not yet exist |
| `provisional` | Validation suite passes but benchmark coverage is incomplete, or the suite has run fewer than the configured minimum iterations |
| `validated` | Validation suite passes with full benchmark coverage; the capability is eligible for installation |
| `contested` | A subsequent validation run has produced failures or regressions against a previously validated version |
| `superseded` | A newer version of this capability has been validated and installed; this version remains in memory for rollback and history |
| `stale` | One or more staleness triggers (Section 14.3) have fired; the capability remains installed but the routing plane must re-validate before continued use |
| `quarantined` | The capability has been administratively suspended due to trust concerns, security review, or repeated validation failure |

Transitions between states are recorded as trace events on the `capability.definition` object. A capability may only be installed (registered with the routing plane) when its validation state is `validated`.

### 13.3 Freshness and Staleness Triggers

A capability transitions from `validated` to `stale` when any of the following triggers fire:

1. **Dependency update.** A capability listed in the dependency set (Section 18) has been superseded or retired.
2. **Engine version change.** An engine class referenced by the procedure has been updated, removed, or had its capability tags modified in the registry.
3. **Benchmark age.** The most recent `capability.benchmark` exceeds the configured staleness threshold (default: 30 days).
4. **Referenced memory decay.** A memory object referenced by the validation suite or procedure has transitioned to `stale`, `contested`, or `quarantined` in the Memory Control Plane.
5. **Explicit invalidation.** A user or administrative cell marks the capability as stale.

When a capability transitions to `stale`, the system must:

- emit a `capability.staleness_detected` event
- downgrade the capability's routing score (Section 19.4) by the configured staleness penalty
- schedule a re-validation run if auto-revalidation is enabled
- notify the user if the capability is actively installed

### 13.4 New Graph Relations

The following graph relation types are added to the set defined in RFC-0004 Section 12.2:

- `supersedes_capability` — directed edge from the newer capability version to the older one; carries a timestamp and optional migration notes
- `depends_on_capability` — directed edge from a capability to another capability it requires; carries the dependency constraint (version pin, tag match, or ID)
- `validated_by` — directed edge from a `capability.definition` to a `capability.validation_report`; one capability may have many such edges over time
- `produced_by_extraction` — directed edge from a `capability.definition` to the execution trace(s) from which it was extracted (Section 16)
- `installed_on` — directed edge from a `capability.definition` to the routing registry entry where it is installed

All edges follow the graph rules from RFC-0004 Section 12.3: every relation must have a type, weighted relations should record confidence, and graph creation should be policy- and cost-aware.

### 13.5 Memory Tier Placement

Capability objects distribute across the canonical memory tiers (RFC-0004 Section 7.1) as follows:

| Tier | Capability Content | Purpose |
|---|---|---|
| **L0** (execution-local) | Active procedure steps, current validation inputs, runtime execution state | Hot execution context for capability cells |
| **L1** (transient cache) | Installed capability procedure lookups, routing feasibility cache, recently used validation results | Fast routing-time access to installed capability metadata |
| **L2** (durable local) | Full `capability.definition`, `capability.procedure`, `capability.validation_suite`, `capability.bundle` | Canonical local durable storage for all capability objects |
| **L3** (semantic retrieval) | Capability description embeddings, tag indexes, contract signature embeddings | Discovery via semantic matching (Section 15.3) |
| **L4** (long-term structured) | Supersession chains, version history, dependency graph, validation report archive | Long-term knowledge about capability evolution |
| **L5** (federated) | Published capability bundles, remote attestation records, federated discovery indexes | Network distribution (Section 17) |

### 13.6 Memory Admission Rules

Capability objects follow lifecycle-dependent admission rules:

1. **Draft capabilities** (validation state `unvalidated` or `provisional`) are admitted to **L2 only**. They must not appear in routing caches or semantic discovery indexes until validated.
2. **Validated capabilities** are admitted to **L2 + L3 + L4**. They become discoverable and their validation history enters the long-term graph.
3. **Installed capabilities** are admitted to **L1 + L2 + L3 + L4**. The L1 placement ensures low-latency routing lookup. Installation triggers L1 cache population.
4. **Retired capabilities** are demoted from L1 and L3. They remain in **L4** (history and supersession chains) and optionally **L2** (archive). Retired capabilities must not appear in routing feasibility or discovery results.
5. **Quarantined capabilities** are immediately evicted from **L1**. They remain in L2 and L4 for audit purposes but are excluded from L3 discovery.

Admission profile mapping:

| Lifecycle State | RFC-0004 Admission Profile |
|---|---|
| draft / unvalidated | `quarantined` (restricted visibility) |
| provisional | `retrieval_candidate` |
| validated | `long_term_candidate` |
| installed | `long_term_candidate` + L1 cache registration |
| retired | `cacheable` (L4 archive) |
| quarantined | `quarantined` |

---

## 14. Capability Discovery and Matching

### 14.1 Discovery Problem

A cell declares intent and constraints. The system must determine whether an installed capability can satisfy that intent more effectively than raw routing. Discovery is the process of finding candidate capabilities from the installed set, the local validated set, and optionally the federated network.

Discovery must be:

- fast enough to participate in routing without adding unacceptable latency
- precise enough to avoid false matches that waste validation and execution time
- recall-oriented enough to surface capabilities the user may not know exist

### 14.2 Tag-Based Matching

The simplest discovery mode matches the capability tag set from RFC-0005 Section 8.4 against the cell's intent and input types.

A tag match score is computed as:

```text
tag_score = |capability_tags ∩ required_tags| / |required_tags|
```

Where `required_tags` are derived from:

- the cell intent name and objective
- the cell input object types
- explicit capability tag constraints in the cell envelope

Tag-based matching is deterministic, low-latency, and suitable for L1 cache lookup.

### 14.3 Semantic Matching

When tag-based matching produces insufficient candidates or the cell intent is expressed in natural language, the system falls back to semantic matching.

Semantic matching operates against the L3 tier:

1. The cell intent description is embedded using the same embedding model used for capability description indexing.
2. A nearest-neighbor search is performed against `capability.definition` description embeddings in the L3 vector index.
3. Results above the configured similarity threshold (default: 0.75) are returned as candidates.
4. Candidates are filtered by feasibility (Section 15.4) before scoring.

Semantic matching is more expensive than tag matching but enables discovery of capabilities that use different terminology for equivalent operations.

### 14.4 Contract Matching

After tag or semantic matching produces a candidate set, contract matching verifies structural compatibility:

1. **Input contract satisfiability.** The candidate capability's declared input contract is compared against the cell's available inputs. All required input types must be satisfiable.
2. **Output contract compatibility.** The candidate capability's declared output types must include or be compatible with the cell's requested outputs.
3. **Constraint compatibility.** The candidate capability's declared constraints (latency, cost, engine class) must be satisfiable within the cell's budget profile.
4. **Policy compatibility.** The candidate capability's required policy tags must not conflict with the cell's execution policy.

Contract matching is deterministic and operates on structured metadata, not embeddings.

### 14.5 Discovery Rules

1. Discovery must check installed capabilities first, then validated-but-not-installed, then federated.
2. Tag-based matching must be attempted before semantic matching.
3. Contract matching must be applied to all candidates regardless of discovery mode.
4. Discovery results must be ranked by a composite score combining tag overlap, semantic similarity, contract fit, and historical performance.
5. Discovery must complete within a configurable timeout (default: 200ms for local, 2000ms including federated).
6. Discovery results must be traceable: the routing trace must record which capabilities were considered and why each was selected or rejected.

---

## 15. Capability Learning and Extraction

### 15.1 Extraction from Traces

The system can derive new capability candidates by analyzing execution traces of complex multi-step tasks that achieve high validation scores. This is the mechanism by which the system learns from its own successful execution patterns.

An extraction candidate is identified when:

- a cell or cell tree completes with validation state `validated` and confidence above the extraction threshold (default: 0.85)
- the execution involved multiple steps, engine assignments, or decomposition
- the task pattern has been observed at least `N` times with consistent success (default: 3)
- no existing installed capability already covers the same intent with equal or better performance

### 15.2 Extraction Process

Extraction is performed by a dedicated `capability.extract` cell:

1. **Trace analysis.** The extraction cell reads the execution trace, identifying the decomposition structure, engine assignments, input/output types at each step, and validation outcomes.
2. **Pattern identification.** The cell identifies the reusable pattern: which steps are task-specific and which generalize across instances of the same intent class.
3. **Engine class generalization.** Specific engine IDs are generalized to engine class requirements (e.g., `eng_local_qwen3_8b` becomes `local_model` with capability tag `structured_extraction`).
4. **Contract extraction.** Input and output contracts are derived from the observed input/output types across multiple trace instances.
5. **Procedure generation.** A draft `capability.procedure` is constructed from the generalized step sequence.
6. **Validation suite generation.** A draft `capability.validation_suite` is constructed from the observed inputs and outputs, using the successful executions as reference cases.

The extraction process produces a complete draft capability with validation state `unvalidated`.

### 15.3 Human-in-the-Loop

Extracted capabilities enter draft status and are not automatically installed. The system surfaces extraction candidates to the user through:

- a `capability.extraction_candidate` event
- an entry in the capability discovery results marked as `draft`
- a CLI notification via `cap discover --include-drafts`

The user may:

- approve the draft for validation (triggers validation suite execution)
- modify the procedure or validation suite before validation
- reject the draft (transitions to `quarantined` with reason `rejected_by_user`)
- defer review (the draft remains in L2 storage)

Auto-validation may be enabled by policy, in which case extracted capabilities proceed to validation without user intervention but still require explicit installation.

### 15.4 Refinement from Experience

After a capability is installed, continued execution produces additional traces that may reveal improved patterns:

- a more efficient engine class assignment
- a better decomposition strategy
- additional edge cases that should be added to the validation suite
- performance regressions that indicate the procedure should be updated

Refinement produces a new version of the capability, linked to the previous version via a `supersedes_capability` graph edge.

### 15.5 Learning Rules

1. Extraction is suggestive, not automatic. The system proposes capabilities; it does not silently install them without human awareness.
2. Extracted capabilities must go through the full lifecycle: draft, validate, install. There is no shortcut.
3. Refinement produces new versions, not in-place mutations. The previous version remains accessible for rollback.
4. The system must surface extraction candidates to the user with sufficient context: the source traces, the proposed procedure, and the expected improvement over raw routing.
5. Extraction frequency should be bounded to avoid overwhelming the user with low-value suggestions. The system should batch candidates and prioritize by expected utility.

---

## 16. Network Capability Distribution

### 16.1 Publishing

A validated and installed capability may be published to the network, extending the capability advertisement mechanism defined in RFC-0006 Section 11. Publishing makes the capability discoverable by peer nodes but does not grant them automatic access to execute or install it.

Publishing is performed by a `capability.publish` cell that:

1. Constructs a `capability.bundle` containing the definition, procedure, validation suite, and most recent passing validation report.
2. Signs the bundle with the local node's Ed25519 identity key.
3. Registers the capability in the node's capability advertisement.
4. Optionally pushes the advertisement update to connected peers.

### 16.2 Capability Advertisement Schema Extension

The RFC-0006 Section 11.2 capability advertisement is extended with an `installed_capabilities` array:

```json
{
  "node_id": "node_01ABC...",
  "timestamp": "2026-04-13T18:00:00Z",
  "engines": [],
  "memory_surfaces": [],
  "installed_capabilities": [
    {
      "capability_id": "cap_01JR...",
      "name": "extract_action_items",
      "version": 3,
      "tags": ["structured_extraction", "meeting_processing"],
      "input_contract_summary": {
        "required_types": ["document.transcript"]
      },
      "output_contract_summary": {
        "output_types": ["task.result"]
      },
      "validation_state": "validated",
      "last_validated_at": "2026-04-10T14:30:00Z",
      "benchmark_summary": {
        "median_latency_ms": 2400,
        "median_confidence": 0.91,
        "median_cost_usd": 0.003
      },
      "attestation": {
        "bundle_hash": "sha256:a1b2c3d4...",
        "validation_report_hash": "sha256:e5f6a7b8...",
        "signed_at": "2026-04-10T14:35:00Z",
        "signature": "ed25519:..."
      }
    }
  ]
}
```

### 16.3 Attestation Model

Each published capability carries an attestation that enables remote nodes to assess trust without executing the validation suite locally:

```json
{
  "capability_id": "cap_01JR...",
  "bundle_hash": "sha256:a1b2c3d4e5f6...",
  "validation_report_hash": "sha256:e5f6a7b8c9d0...",
  "timestamp": "2026-04-10T14:35:00Z",
  "node_id": "node_01ABC...",
  "signature": "ed25519:..."
}
```

Attestation semantics:

1. The `bundle_hash` is the SHA-256 hash of the complete `capability.bundle` content.
2. The `validation_report_hash` is the SHA-256 hash of the most recent passing `capability.validation_report`.
3. The `signature` is the Ed25519 signature of the concatenation of `capability_id`, `bundle_hash`, `validation_report_hash`, and `timestamp`, signed by the node's identity key.
4. Attestation is a **trust signal only**. It does not substitute for local validation. A receiving node may choose to trust the remote attestation (install directly) or require local re-validation depending on the trust zone of the advertising peer.

### 16.4 Remote Capability Installation

When a node discovers a published capability from a peer and wishes to install it locally:

1. **Fetch bundle.** The local node requests the `capability.bundle` from the remote peer via the network transport.
2. **Verify attestation.** The local node verifies the bundle hash and signature against the peer's known identity key.
3. **Run local validation.** If the peer is in a trust zone that requires local re-validation (anything below `trusted-edge`), the local node executes the validation suite against local engines. If the peer is `trusted-edge` or higher, the local node may skip local validation and accept the remote attestation.
4. **Install locally.** On successful validation (local or attested), the capability proceeds through the normal installation lifecycle on the local node.

Remote attestation is a trust signal. It reduces the cost of adoption but does not override local authority.

### 16.5 Replication Contract Extension

The replication scope filters defined in RFC-0006 Section 12.4 are extended to include capability object types:

```json
{
  "scope": {
    "taxonomy_paths": ["capabilities/shared"],
    "object_types": [
      "capability.definition",
      "capability.procedure",
      "capability.validation_suite",
      "capability.validation_report",
      "capability.benchmark",
      "capability.bundle"
    ],
    "minimum_validation_state": "validated"
  }
}
```

This allows replication contracts to selectively synchronize capability objects between nodes, subject to the same policy enforcement and encryption requirements as all other replicated state.

### 16.6 Distribution Rules

1. Only capabilities with validation state `validated` or higher may be published.
2. Publication requires a valid `capability.bundle` with a current attestation.
3. Remote nodes must verify attestation signatures before trusting any capability metadata.
4. Remote installation must follow the local lifecycle. Remote attestation may accelerate but not bypass it.
5. Capability advertisements follow the same TTL, refresh, and staleness rules as engine advertisements in RFC-0006 Section 11.
6. A node must not advertise capabilities it has not locally validated or for which it does not hold a valid attestation from a trusted peer.

---

## 17. Dependency Management

### 17.1 Dependency Types

A capability may declare dependencies on external resources. Each dependency has a type and a resolution constraint:

| Dependency Type | Description | Example |
|---|---|---|
| `capability` | Another capability required as a sub-step or prerequisite | `cap_01XY...` or tag `structured_extraction` |
| `engine_class` | An engine class that must be available in the routing registry | `local_model` with tag `long_context_reasoning` |
| `memory_surface` | A memory surface that must be present for the capability to function | `semantic_retrieval` surface with taxonomy scope `work/` |
| `system_version` | A minimum Anunix system version for API compatibility | `>= 0.3.0` |

Dependencies are declared in the `capability.definition` envelope:

```json
{
  "capability_id": "cap_01JR...",
  "dependencies": [
    {
      "type": "capability",
      "constraint": {
        "id": "cap_01XY..."
      },
      "required": true
    },
    {
      "type": "capability",
      "constraint": {
        "tags": ["schema_validation"],
        "minimum_validation_state": "validated"
      },
      "required": true
    },
    {
      "type": "engine_class",
      "constraint": {
        "class": "local_model",
        "capability_tags": ["structured_extraction"],
        "minimum_context_tokens": 32768
      },
      "required": true
    },
    {
      "type": "memory_surface",
      "constraint": {
        "surface_type": "semantic_retrieval",
        "taxonomy_scope": "work/"
      },
      "required": false
    },
    {
      "type": "system_version",
      "constraint": {
        "minimum": "0.3.0"
      },
      "required": true
    }
  ]
}
```

### 17.2 Dependency Resolution at Installation Time

When a capability is installed, the system resolves all declared dependencies:

1. **Capability dependencies by ID** are checked against the local capability registry. The referenced capability must be installed with validation state `validated` or better.
2. **Capability dependencies by tag** are resolved by querying the local capability registry for installed capabilities matching all specified tags. If multiple candidates exist, the highest-scoring candidate is selected.
3. **Engine class dependencies** are checked against the RFC-0005 capability registry. The required engine class must be registered with `available` status and matching capability tags.
4. **Memory surface dependencies** are checked against the Memory Control Plane. Required surfaces must exist and be queryable.
5. **System version dependencies** are checked against the running system version.

If any required dependency cannot be resolved, installation fails with a structured error listing all unresolved dependencies. Optional dependencies that cannot be resolved are logged as warnings but do not block installation.

### 17.3 Cascading Effects

Dependency relationships create cascading lifecycle effects:

1. **Dependency supersession.** When a capability dependency is superseded by a newer version, the dependent capability transitions to `stale` (Section 14.3, trigger 1). The dependent may continue to function if the superseding version is backward-compatible.
2. **Dependency retirement.** When a capability dependency is retired, the dependent capability must be re-evaluated. If no substitute is available (by tag or explicit migration), the dependent transitions to `stale` and a `capability.dependency_broken` event is emitted.
3. **Engine class removal.** When an engine class required by a capability is removed from the registry or transitions to `offline`, the capability transitions to `stale`.
4. **Memory surface removal.** When a required memory surface becomes unavailable, optional-dependency capabilities continue to function with degraded behavior; required-dependency capabilities transition to `stale`.

Cascading effects are evaluated lazily: the system checks dependencies at routing time and during periodic health sweeps, not synchronously on every upstream change.

### 17.4 Dependency Graph as Graph State Objects

The dependency graph is materialized as `graph.node` and `graph.edge_set` State Objects in the Memory Control Plane:

- Each `capability.definition` is a graph node.
- Each dependency relationship is a directed `depends_on_capability` edge.
- Each supersession relationship is a directed `supersedes_capability` edge.

This allows the dependency graph to be queried, traversed, and visualized using the same graph services defined in RFC-0004 Section 12.

### 17.5 Dependency Rules

1. **No dependency cycles.** The dependency graph must be a directed acyclic graph. Installation must reject any capability whose addition would create a cycle.
2. **Version pinning.** Capability dependencies may be pinned to a specific version or left floating (resolved to the latest installed version matching constraints).
3. **Tag-based substitution.** When a capability dependency is declared by tag rather than ID, any installed capability matching the tag set may satisfy the dependency. This enables loose coupling.
4. **Dependency depth limit.** The system enforces a configurable maximum dependency depth (default: 10) to prevent deep transitive chains.
5. **Dependency resolution must be deterministic.** Given the same installed capability set and the same dependency declaration, resolution must produce the same result.

---

## 18. Amendments to Existing RFCs

This section specifies the exact amendments required to prior RFCs.

### 18.1 Amendments to RFC-0002: State Object Model

**Section 5 (Object Types).** Add `capability` as the 7th top-level object type, alongside `document`, `memory`, `index`, `graph`, `trace`, and `task`.

**Section (ID Prefixes).** Add `CapabilityID` with the prefix `cap_`. Capability IDs follow the same format and generation rules as other State Object IDs.

**Section (Envelope Schema).** The standard envelope schema applies to all `capability.*` subtypes. No envelope modifications are required; capability-specific metadata is carried in the `metadata` field.

### 18.2 Amendments to RFC-0003: Execution Cell Runtime

**Section 7.1 (Initial Cell Type Families).** Add a new family:

#### Capability Cells
- `capability.validate` — executes a validation suite against a capability's procedure
- `capability.install` — registers a validated capability with the routing plane
- `capability.benchmark` — runs performance benchmarks against an installed capability
- `capability.extract` — analyzes execution traces to propose new capability candidates
- `capability.refine` — produces a new capability version from accumulated execution feedback
- `capability.invalidate` — transitions a capability to `contested` or `stale` based on new evidence
- `capability.retire` — transitions a capability to `superseded` and removes it from active routing

**Section 7.2 (Type Registry).** Each capability cell type must be registered with its expected input classes (capability objects, traces, validation suites), expected output classes (validation reports, benchmarks, updated definitions), and allowed engine classes.

### 18.3 Amendments to RFC-0004: Memory Control Plane

**Section 8.1 (Core Memory-Bearing Object Types).** Add:
- `capability.definition`
- `capability.procedure`
- `capability.validation_suite`
- `capability.validation_report`
- `capability.benchmark`
- `capability.bundle`

**Section 12.2 (Graph Relation Types).** Add:
- `supersedes_capability`
- `depends_on_capability`
- `validated_by`
- `produced_by_extraction`
- `installed_on`

**Section 17 (Freshness, Decay, and Forgetting).** Add capability-specific staleness triggers as defined in Section 14.3 of this RFC. Capability staleness is driven by dependency changes, engine version changes, benchmark age, and referenced memory decay, in addition to the standard time-based and usage-based decay rules.

**Section 10.3 (Admission Profiles).** Add the admission profile mapping defined in Section 14.6 of this RFC. Draft capabilities use the `quarantined` profile; validated capabilities use `long_term_candidate`; installed capabilities additionally populate L1 cache.

### 18.4 Amendments to RFC-0005: Routing Plane and Unified Scheduler

**Section 7.1 (Canonical Engine Classes).** Add `installed_capability` as a pseudo-engine class. When a capability is installed, it appears in the routing plane as a composite engine that encapsulates its procedure's engine class requirements.

**Section 8.2 (Registry Schema).** Extend the capability registry to include installed capabilities alongside raw engines:

```json
{
  "engine_id": "cap_01JR...",
  "engine_class": "installed_capability",
  "status": "available",
  "capabilities": ["structured_extraction", "meeting_processing"],
  "constraints": {
    "input_types": ["document.transcript"],
    "output_types": ["task.result"]
  },
  "cost_model": {
    "kind": "capability_estimate",
    "benchmark_median_cost_usd": 0.003
  },
  "quality_profile": {
    "validation_state": "validated",
    "benchmark_median_confidence": 0.91,
    "benchmark_median_latency_ms": 2400
  }
}
```

**Section 13.3 (Scoring Model).** Extend the route scoring formula with a capability bonus term:

```text
RouteScore =
  w_confidence * confidence_estimate
- w_latency    * latency_estimate
- w_cost       * cost_estimate
+ w_locality   * locality_score
+ w_policy     * policy_margin
+ w_history    * historical_success_score
+ w_capability * capability_bonus
```

Where `capability_bonus` reflects:

- the capability's validation state (validated > provisional)
- the capability's benchmark performance relative to raw routing alternatives
- the capability's freshness (non-stale > stale)
- the capability's version age (newer validated versions score higher)

**Section 10 (Feasibility Filtering).** Add capability-specific feasibility checks:

1. If the cell intent matches an installed capability, verify that the capability's input contract is satisfiable by the cell's inputs.
2. If a capability is stale, apply the configured staleness penalty to its score or exclude it from feasibility depending on policy.
3. If a capability's required engine classes are unavailable, exclude the capability from feasibility.

### 18.5 Amendments to RFC-0006: Network Plane and Federated Execution

**Section 11.2 (Capability Advertisement).** Extend the advertisement schema to include the `installed_capabilities` array as defined in Section 17.2 of this RFC.

**Section 12.4 (Replication Scope Filters).** Add `capability.definition`, `capability.procedure`, `capability.validation_suite`, `capability.validation_report`, `capability.benchmark`, and `capability.bundle` to the set of object types available for replication scope filtering, as defined in Section 17.5 of this RFC.

---

## 19. APIs

### 19.1 Create Capability

```http
POST /capabilities
```

Request:

```json
{
  "name": "extract_action_items",
  "description": "Extract structured action items from meeting transcripts",
  "tags": ["structured_extraction", "meeting_processing"],
  "input_contract": {
    "required": [
      {"name": "transcript", "type": "document.transcript"}
    ],
    "optional": [
      {"name": "context", "type": "memory.summary"}
    ]
  },
  "output_contract": {
    "outputs": [
      {"name": "action_items", "type": "task.result"}
    ]
  },
  "procedure": {
    "steps": [
      {
        "name": "extract",
        "engine_class": "local_model",
        "capability_tags": ["structured_extraction"],
        "input_refs": ["transcript"],
        "output_name": "raw_extraction"
      },
      {
        "name": "validate",
        "engine_class": "validation_service",
        "capability_tags": ["schema_validation"],
        "input_refs": ["raw_extraction"],
        "output_name": "action_items"
      }
    ]
  }
}
```

Response:

```json
{
  "capability_id": "cap_01JR...",
  "version": 1,
  "validation_state": "unvalidated",
  "created_at": "2026-04-13T18:00:00Z"
}
```

### 19.2 Get Capability

```http
GET /capabilities/{id}
```

### 19.3 List Capabilities

```http
GET /capabilities?status=installed&tags=structured_extraction,meeting_processing
```

Query parameters:

- `status` — filter by lifecycle state (`draft`, `validated`, `installed`, `retired`, `stale`, `quarantined`)
- `tags` — comma-separated tag filter (all tags must match)
- `validation_state` — filter by validation state
- `limit` — maximum results (default: 50)
- `offset` — pagination offset

### 19.4 Install Capability

```http
POST /capabilities/{id}/install
```

Resolves dependencies, verifies validation state, registers with routing plane.

Response:

```json
{
  "capability_id": "cap_01JR...",
  "installation_state": "installed",
  "resolved_dependencies": [
    {"type": "engine_class", "resolved": "eng_local_qwen3_8b"},
    {"type": "capability", "resolved": "cap_01XY..."}
  ],
  "routing_registration": {
    "engine_id": "cap_01JR...",
    "engine_class": "installed_capability"
  }
}
```

### 19.5 Validate Capability

```http
POST /capabilities/{id}/validate
```

Triggers execution of the capability's validation suite. Returns the validation report on completion.

### 19.6 Refine Capability

```http
POST /capabilities/{id}/refine
```

Request:

```json
{
  "source_traces": ["trace_01AA...", "trace_01BB..."],
  "refinement_mode": "procedure_update"
}
```

Creates a new version of the capability based on accumulated execution feedback.

### 19.7 Supersede Capability

```http
POST /capabilities/{id}/supersede
```

Request:

```json
{
  "superseded_by": "cap_01NEW..."
}
```

### 19.8 Retire Capability

```http
POST /capabilities/{id}/retire
```

Request:

```json
{
  "reason": "Replaced by cap_01NEW... with better decomposition strategy"
}
```

### 19.9 Publish Capability

```http
POST /capabilities/{id}/publish
```

Constructs a bundle, signs an attestation, and adds the capability to the node's network advertisement.

### 19.10 Discover Capabilities

```http
POST /capabilities/discover
```

Request:

```json
{
  "intent": "extract action items from a transcript",
  "input_types": ["document.transcript"],
  "output_types": ["task.result"],
  "tags": ["structured_extraction"],
  "include_federated": true,
  "include_drafts": false,
  "max_results": 10
}
```

Response:

```json
{
  "results": [
    {
      "capability_id": "cap_01JR...",
      "name": "extract_action_items",
      "match_score": 0.94,
      "match_modes": ["tag", "contract"],
      "validation_state": "validated",
      "source": "local"
    }
  ]
}
```

### 19.11 Get Capability Bundle

```http
GET /capabilities/{id}/bundle
```

Returns the portable `capability.bundle` archive.

### 19.12 Get Validation Reports

```http
GET /capabilities/{id}/validation-reports
```

Returns the validation report history for the capability.

---

## 20. CLI Surface

Suggested initial CLI:

```bash
cap create --name extract_action_items --from-file capability.json
cap list --status installed --tags structured_extraction
cap get cap_01JR...
cap validate cap_01JR...
cap install cap_01JR...
cap refine cap_01JR... --traces trace_01AA...,trace_01BB...
cap supersede cap_01OLD... --by cap_01NEW...
cap retire cap_01JR... --reason "replaced by improved version"
cap publish cap_01JR...
cap discover --intent "extract action items" --input-types document.transcript
cap bundle cap_01JR... --output ./extract_action_items.bundle
cap benchmark cap_01JR... --iterations 10
cap deps cap_01JR...
cap history cap_01JR...
```

Advanced examples:

```bash
# Create a capability from a procedure file and validate immediately
cap create --name summarize_meeting --from-file summarize.json --validate

# Discover capabilities matching an intent, including federated peers
cap discover --intent "summarize long documents" --include-federated --include-drafts

# View the full dependency tree for an installed capability
cap deps cap_01JR... --tree --include-transitive

# View the supersession history chain
cap history cap_01JR... --chain

# Run benchmarks and display comparison with previous version
cap benchmark cap_01JR... --compare cap_01OLD... --iterations 20

# Install a capability from a remote bundle file
cap install --from-bundle ./extract_action_items.bundle --validate-locally
```

---

## 21. POSIX Compatibility

### 21.1 Compatibility Thesis

Capabilities have no direct POSIX analogue. The closest classical concepts are shared libraries (`libfoo.so`) for reusable functionality and installed packages (`apt install foo`) for managed dependency resolution. Neither captures the validation, trust, or learning semantics that make capabilities distinct.

### 21.2 Mapping

| Classical Concept | Capability Plane Equivalent |
|---|---|
| Shared library (`.so` / `.dylib`) | Installed capability with defined input/output contracts |
| Package manager (`apt`, `pip`) | Capability install with dependency resolution |
| Package repository | Federated capability discovery via network advertisements |
| Package signature verification | Ed25519 attestation on capability bundles |
| `PATH` lookup | Capability discovery via tag and semantic matching |
| `/usr/lib/` | L2 durable storage for capability definitions |

### 21.3 Virtual Namespace

Installed capabilities may be exposed as executable entries in a virtual `/cap/` namespace:

```text
/cap/extract_action_items
/cap/summarize_meeting
/cap/classify_document
```

This allows conventional tools and scripts to invoke capabilities by name, bridging the gap between POSIX-style process invocation and capability-mediated execution. The virtual namespace translates invocation into the standard capability execution cell pipeline.

### 21.4 Initial Strategy

The initial prototype should prioritize native APIs and CLI. POSIX compatibility via the `/cap/` virtual namespace is deferred to a later phase when the execution cell runtime supports process-level interop (RFC-0003 Mode B).

---

## 22. Reference Prototype Architecture

### 22.1 Initial Components

1. **Capability Service**
   - CRUD operations for capability definitions, procedures, and validation suites
   - Version management and lifecycle state machine
   - Admission control and memory tier placement

2. **Capability Validator**
   - Executes validation suites against capability procedures
   - Produces `capability.validation_report` objects
   - Supports configurable iteration counts and confidence thresholds

3. **Capability Installer**
   - Resolves dependencies
   - Registers capabilities with the routing plane as `installed_capability` engine entries
   - Manages L1 cache population and eviction

4. **Capability Executor Adapter**
   - Runtime adapter that translates a capability procedure into a cell execution plan
   - Wires procedure steps to engine assignments via the routing plane
   - Handles step sequencing, intermediate state management, and output collection

5. **Capability Discovery Service**
   - Tag-based matching against the capability registry
   - Semantic matching against L3 vector indexes
   - Contract matching for structural compatibility verification
   - Result ranking and federation proxy

6. **Capability Extractor**
   - Trace analysis for extraction candidate identification
   - Pattern generalization and procedure generation
   - Validation suite generation from observed inputs/outputs
   - Candidate surfacing and notification

### 22.2 Recommended Initial Stack

- Python for all service logic and orchestration
- PostgreSQL for capability definitions, dependency graphs, and lifecycle state
- pgvector for semantic discovery embeddings (capability descriptions)
- local filesystem or S3-compatible blob store for capability bundles
- message queue for async validation and benchmark execution
- consistent with the stack choices in RFC-0003 Section 31.2, RFC-0004 Section 25.2, and RFC-0006 Section 19.2

---

## 23. Observability

### 23.1 Required Metrics

- capability count by validation state (unvalidated, provisional, validated, installed, stale, retired, quarantined)
- installation count and failure rate
- validation execution count, pass rate, and duration
- benchmark execution count and performance distribution
- discovery query count and hit rate
- discovery latency (tag, semantic, federated)
- extraction candidate count and acceptance rate
- dependency resolution success and failure counts
- staleness transition count by trigger type
- supersession count
- network publication and remote installation counts

### 23.2 Debug Questions the System Must Answer

- Why was this capability selected over raw routing for this cell?
- Why was this capability not selected despite matching tags?
- Why did installation fail for this capability?
- Which dependencies could not be resolved?
- Why did this capability transition to stale?
- What execution traces produced this extraction candidate?
- How does this capability's benchmark performance compare to its predecessor?
- Which nodes have installed this capability via federation?
- Why was this discovery query slow?

---

## 24. Failure Modes

### 24.1 Capability Drift

The procedure encoded in a capability no longer matches the actual best execution pattern due to engine improvements, memory changes, or workload evolution.

**Mitigation:** staleness triggers (Section 14.3), periodic re-benchmarking, refinement from continued execution feedback.

### 24.2 Over-Installation

Too many capabilities are installed, polluting the routing plane with low-value entries and increasing feasibility check overhead.

**Mitigation:** installation budgets, periodic review prompts, usage-based retirement suggestions, routing score decay for unused capabilities.

### 24.3 Circular Dependencies

A capability dependency graph contains a cycle, preventing installation.

**Mitigation:** cycle detection at installation time (Section 18.5, Rule 1). The dependency resolver rejects any addition that would create a cycle.

### 24.4 Trust Inflation

Capabilities accumulate trust through repeated validation against weak or narrow test suites, creating a false sense of reliability.

**Mitigation:** validation suite diversity scoring, benchmark coverage requirements, periodic validation suite expansion from new execution traces.

### 24.5 Supersession Cascade

Superseding a widely-depended-upon capability triggers a wave of staleness transitions across many dependents, potentially destabilizing the installed capability set.

**Mitigation:** lazy staleness evaluation, configurable cascade depth limits, staged supersession with backward-compatibility grace periods.

### 24.6 Network Trust Poisoning

A malicious or compromised peer publishes a capability with a fraudulent attestation, causing other nodes to install harmful or low-quality procedures.

**Mitigation:** attestation signature verification, mandatory local re-validation for capabilities from untrusted zones, trust zone restrictions on auto-installation, bundle hash verification.

### 24.7 Extraction False Positives

The extraction process proposes capabilities from coincidentally successful trace patterns that do not generalize.

**Mitigation:** minimum trace count threshold, human-in-the-loop review, draft status with mandatory validation before installation, extraction confidence scoring.

---

## 25. Implementation Plan

### Phase 1: Capability Object and Registry

- define `capability.*` State Object types and register in RFC-0002 type registry
- define capability-specific error types and event types
- implement Pydantic models for `capability.definition`, `capability.procedure`, `capability.validation_suite`
- implement CRUD service for capability objects
- implement capability registry with tag-based lookup
- store in PostgreSQL with standard envelope schema

### Phase 2: Validation and Installation

- implement `capability.validate` cell type
- implement `capability.install` cell type
- implement `capability.benchmark` cell type
- implement validation suite execution engine
- implement lifecycle state machine with transition rules
- implement routing plane registration for installed capabilities
- implement capability executor adapter (procedure-to-cell-plan translation)

### Phase 3: Routing Integration

- register `installed_capability` as engine class in RFC-0005 capability registry
- implement feasibility checks for capability input/output contracts
- implement capability bonus term in route scoring
- implement discovery service (tag-based matching)
- implement runtime wiring: routing plane selects capability, executor adapter translates to cell plan

### Phase 4: Lifecycle Management and Memory

- implement supersession and retirement workflows
- implement dependency resolver with cycle detection
- implement staleness trigger evaluation
- implement memory admission rules (Section 14.6)
- implement L3 semantic discovery embeddings (pgvector)
- implement `capability.refine`, `capability.invalidate`, and `capability.retire` cell types
- implement dependency graph as graph State Objects

### Phase 5: Network Distribution and Extraction

- extend RFC-0006 capability advertisements with `installed_capabilities`
- implement Ed25519 attestation signing and verification
- implement `capability.publish` cell type
- implement remote bundle fetch and local re-validation workflow
- implement `capability.extract` cell type with trace analysis
- implement extraction candidate surfacing and notification
- implement federated discovery proxy

---

## 26. Open Questions

1. **Conditional branching in procedures.** Should procedures support conditional steps (e.g., "if extraction confidence < 0.7, add a verification step"), or should all branching be handled by decomposition at the cell level?

2. **Minimum trace count for extraction.** What is the right default minimum number of successful traces before the system proposes extraction? Too low risks false positives; too high delays useful capability formation.

3. **Budget caps for capability execution.** Should capabilities declare their own budget caps independently from the invoking cell's budget profile, or should the cell budget always govern?

4. **Dependencies on unavailable engine classes.** If a capability depends on an engine class that is available locally but not on a remote node, should the capability be advertised to that node at all?

5. **Stochastic validation tests.** Should validation suites support stochastic test cases where the expected output is a distribution or confidence range rather than an exact match?

6. **Default staleness threshold.** What is the right default benchmark age threshold before a capability is marked stale? 30 days is the initial proposal, but this may vary significantly by capability type and domain.

7. **Manual authoring vs. trace-only derivation.** Should the system support fully hand-authored capabilities that were never derived from traces, or should every capability have at least one reference trace for grounding?

8. **Counterfactual performance comparison.** Should the system periodically execute tasks both with and without an installed capability to measure the actual performance delta, and if so, how should it manage the cost?

---

## 27. Decision Summary

This RFC makes the following decisions:

1. The Capability Object is a first-class State Object type with six memory-bearing subtypes and a dedicated `cap_` ID prefix.
2. Capabilities follow a strict lifecycle: draft, validate, install, refine, supersede, retire. No lifecycle stage may be skipped.
3. Validation is mandatory before installation. Remote attestation may accelerate but not bypass local lifecycle requirements.
4. Capabilities integrate with the Memory Control Plane through lifecycle-dependent tier placement and admission rules.
5. Discovery supports three modes — tag-based, semantic, and contract-based — applied in order of increasing cost.
6. Extraction from execution traces is the primary mechanism for capability formation, but extraction is suggestive, not automatic.
7. Dependencies are explicitly declared, acyclic, and resolved at installation time with deterministic semantics.
8. Network distribution uses Ed25519-signed attestations and extends the existing RFC-0006 advertisement and replication mechanisms.
9. Installed capabilities appear in the routing plane as a composite engine class, extending RFC-0005 feasibility and scoring.
10. The initial implementation uses Python, PostgreSQL, and pgvector, consistent with the stack choices across all prior RFCs.

---

## 28. Conclusion

The Capability Object completes the loop that makes Anunix a self-improving system. Execution produces traces. Traces inform extraction. Extracted capabilities are validated and installed. The routing plane selects them when they outperform raw routing. Continued execution feeds back into refinement, producing better versions. This is what makes Anunix not just an operating system that runs AI workloads, but an operating system that learns from them.

The design is deliberately conservative about automation. Extraction is suggestive. Installation requires validation. Trust is earned through benchmarks, not assumed from authorship. Supersession preserves history. Dependencies are explicit. Every decision is traceable. The system proposes; the user disposes. This is the right posture for a capability system that must earn trust incrementally rather than demanding it upfront.

Without this layer, the system routes every task from scratch, unable to accumulate operational knowledge about what works. With it, the operating environment develops a growing repertoire of validated, composable, observable execution patterns — and the feedback loop to keep them honest.