# RFC-0001: Network-First, Model-Routed, Memory-Centric OS

| Field      | Value                                      |
|------------|--------------------------------------------|
| RFC        | 0001                                       |
| Title      | Architecture Thesis                        |
| Author     | Adam Pippert                               |
| Status     | Draft                                      |
| Created    | 2026-04-13                                 |
| Updated    | 2026-04-13                                 |
| Depends On | —                                          |

---

## Executive Summary

This RFC defines the core architectural direction for an AI-native operating system that:

- treats the network as a first-class execution and memory substrate
- routes computation across heterogeneous models via a unified scheduler
- organizes knowledge via a taxonomic + graph-based memory system

The goal is to preserve UNIX composability while redefining primitives around:

state → transformation → memory → routing → validation

This RFC establishes:

- system invariants
- core abstractions
- control planes
- execution model
- initial implementation constraints

---

## 1. Problem Statement

Modern operating systems assume:

- compute is local-first
- memory is passive
- network is transport-only
- programs are deterministic

AI-native workloads violate all of these:

- compute is distributed (local + remote models)
- memory is active (retrieval, compression, reasoning)
- network is required for correctness (not optional)
- outputs are probabilistic and require validation

Result: Current OS abstractions force AI systems into inefficient, ad-hoc orchestration layers.

---

## 2. Design Goals

### 2.1 Primary Goals

1. **Network-First Execution**
   - System must assume remote resources are part of normal execution

2. **Model-Routed Compute**
   - All tasks are dynamically decomposed and routed across models/tools

3. **Memory-Centric Architecture**
   - Persistent, structured, and evolving memory is a core OS concern

4. **Composability**
   - Maintain UNIX-style small, composable units

5. **Observability + Provenance**
   - Every transformation must be traceable and reproducible

### 2.2 Non-Goals

- Replacing Linux kernel immediately
- Embedding full AI orchestration into kernel space
- Forcing all applications to adopt new abstractions

---

## 3. Core System Invariants

### Invariant 1: Everything is a State Object

All data is represented as:

```
(state, metadata, provenance, policy)
```

### Invariant 2: Execution is Declarative

All execution is defined as:

```
intent → decomposition → routing → execution → validation → commit
```

### Invariant 3: Memory is Tiered and Active

Memory is not passive storage. It is:

- indexed
- linked
- validated
- promoted/demoted
- optionally recomputed

### Invariant 4: Network is Always Available (Logically)

Even when physically unavailable, the system:

- simulates degraded behavior
- uses cached or local alternatives
- degrades gracefully

### Invariant 5: Routing is First-Class

All non-trivial computation goes through a routing decision:

```
task → decomposition → routing → execution
```

---

## 4. Architecture Overview

### 4.1 Planes

The system is divided into five control planes:

| Plane | Responsibility |
|-------|---------------|
| State Plane | Data representation and storage |
| Memory Plane | Indexing, graphing, retention |
| Execution Plane | Execution cells |
| Routing Plane | Model/tool selection |
| Network Plane | Remote compute and memory |

---

## 5. Core Components

### 5.1 State Object Layer

**Definition:** A state object is the atomic unit of data.

```
StateObject {
  id
  type
  payload
  metadata
  provenance
  policy
  confidence
}
```

**Types:**

- File (raw bytes)
- Document
- Embedding set
- Graph node
- Execution trace
- Model output
- Memory capsule

### 5.2 Execution Cell Runtime

**Definition:** Execution cell = composable unit of work.

```
ExecutionCell {
  intent
  inputs
  constraints
  routing_policy
  validation_policy
}
```

**Behavior:**

1. Decompose task
2. Request routing decision
3. Execute via selected engines
4. Validate outputs
5. Commit to memory

### 5.3 Routing Engine (Model Router)

**Responsibility:** Dynamic selection of:

- local model
- remote model
- deterministic tool
- retrieval path

**Inputs:**

- task type
- latency constraints
- cost constraints
- confidence requirements
- data locality
- model capability graph

**Output:**

```
ExecutionPlan {
  subtasks
  assigned_engines
  fallback_paths
}
```

### 5.4 Memory Control Plane

**Structure:** Memory is a hybrid system:

```
Memory =
  Taxonomy (hierarchy)
+ Graph (relationships)
+ Indexes (retrieval)
+ Cache (recent)
```

### 5.5 Taxonomic Memory

**Purpose:** Organize knowledge into structured hierarchies:

- domains
- subdomains
- concepts
- entities

**Benefits:**

- reduces search space
- improves routing
- enables compression
- supports reasoning shortcuts

### 5.6 Graph Memory

**Purpose:** Capture relationships:

- references
- causality
- similarity
- contradiction

**Graph Edges:**

```
Edge {
  source
  target
  type
  weight
  timestamp
}
```

### 5.7 Memory Lifecycle

```
observe → normalize → index → validate → promote → link → decay
```

### 5.8 Network Plane

**Responsibilities:**

- remote execution
- remote memory access
- replication
- fallback routing

**Key Principle:** Network is not I/O. It is:

```
a distributed extension of compute + memory
```

### 5.9 Scheduler

**Responsibilities:** Unified scheduling across:

- CPU
- GPU/NPU
- memory tiers
- network paths
- model inference

**Decision Variables:**

- latency
- cost
- confidence
- energy
- locality

---

## 6. Data Flow Model

```
User Intent
  ↓
Execution Cell
  ↓
Task Decomposition
  ↓
Routing Engine
  ↓
Local / Remote Execution
  ↓
Validation Layer
  ↓
State Object Commit
  ↓
Memory Integration
```

---

## 7. Validation Layer (Critical)

All outputs must pass validation before promotion:

- self-consistency checks
- cross-source verification
- symbolic verification (if possible)
- confidence scoring

---

## 8. Compatibility Strategy

- POSIX compatibility layer
- Files mapped to State Objects
- Processes mapped to Execution Cells
- Pipes mapped to Streams

---

## 9. Failure Modes

### 9.1 Over-Routing

Too many subtask splits → overhead explosion

### 9.2 Memory Bloat

Unbounded graph + embeddings

### 9.3 Trust Collapse

No validation → hallucination propagation

### 9.4 Network Dependence

Poor offline fallback

---

## 10. Implementation Strategy

### Phase 1 (Userland Prototype)

- Linux base (Fedora preferred)
- State object layer in userland
- Model router service
- Memory service (graph + taxonomy)
- Execution cell runtime

### Phase 2 (Deep Integration)

- scheduler hooks
- filesystem overlay
- network-aware routing

### Phase 3 (Kernel Extensions)

- state object primitives
- memory hooks
- scheduling hints

---

## 11. Open Questions

1. Optimal taxonomy encoding (numeric vs symbolic vs hybrid)
2. Graph storage backend (Neo4j vs custom vs hybrid)
3. Routing learning strategy (rules vs RL vs hybrid)
4. Memory validation heuristics
5. Security boundaries for model execution

---

## 12. Conclusion

This RFC proposes a system where:

- network is compute
- memory is active
- routing is mandatory
- state is structured
- execution is composable

This is not an incremental improvement over UNIX.

It is a reinterpretation of UNIX principles under:

- distributed compute
- probabilistic reasoning
- persistent semantic memory
