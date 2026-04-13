# RFC-0004: Memory Control Plane

| Field      | Value                                      |
|------------|--------------------------------------------|
| RFC        | 0004                                       |
| Title      | Memory Control Plane                       |
| Author     | Adam Pippert                               |
| Status     | Draft                                      |
| Created    | 2026-04-13                                 |
| Updated    | 2026-04-13                                 |
| Depends On | RFC-0001, RFC-0002, RFC-0003               |

---

## Executive Summary

This RFC defines the **Memory Control Plane**, the subsystem responsible for turning raw and derived State Objects into durable, retrievable, policy-aware memory for an AI-native operating system.

In a classical operating system, memory is mostly divided into:

- transient working memory
- persistent storage
- opaque caches

That decomposition is no longer sufficient. In an AI-native system, memory must be treated as an **active, tiered, policy-driven substrate** that determines:

- what is worth keeping
- how it should be represented
- where it should live
- how it should be validated
- how it should be retrieved
- when it should decay
- when it should be forgotten
- when it should be recomputed instead of stored

This RFC specifies:

- the goals and scope of the Memory Control Plane
- the canonical memory tiers
- taxonomy and graph integration
- retrieval surfaces
- promotion and demotion rules
- memory admission and validation
- contradiction handling and anti-knowledge
- forgetting, decay, and garbage collection
- APIs and CLI
- prototype implementation guidance

The design goal is to preserve the operational clarity of traditional storage systems while enabling a new class of systems that use memory as a first-class substrate for both deterministic and model-mediated computation.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert / public collaborators
**Depends on:** RFC-0001, RFC-0002, RFC-0003
**Blocks:** RFC-0005, RFC-0006, RFC-0007

---

## 2. Problem Statement

Most operating systems treat persistent state as files and databases, and treat memory as either:

- execution-local RAM
- kernel page cache
- application-managed cache
- storage hierarchy implementation detail

That model works for classical workloads, but breaks down for AI-native workloads because useful memory is not just persistence. It is a structured, evolving system of:

- observations
- derived knowledge
- summaries
- embeddings
- graph relations
- taxonomic placement
- trust states
- contradiction tracking
- retention rules
- retrieval indexes

Without a dedicated memory control plane, these concerns become fragmented across applications and services. The result is:

- duplicated indexing logic
- inconsistent retention behavior
- weak provenance
- memory bloat
- hallucination propagation
- poor retrieval quality
- unclear trust boundaries

The system needs a unified memory layer that sits above raw storage and below application-specific reasoning.

---

## 3. Goals

### 3.1 Primary Goals

1. **Tiered memory**
   - Support multiple latency, durability, and value tiers.

2. **Structured memory**
   - Organize memory using taxonomy, graph relations, and retrieval indexes.

3. **Policy-aware retention**
   - Control promotion, replication, access, decay, and deletion through explicit policy.

4. **Trust-aware memory**
   - Prevent low-confidence or contradictory material from being treated as stable knowledge without validation.

5. **Retrieval-ready design**
   - Make memory natively usable by classical search, semantic retrieval, and graph traversal.

6. **Lifecycle management**
   - Define clear rules for admission, normalization, linking, promotion, decay, and forgetting.

7. **State Object integration**
   - Use State Objects as the canonical memory carriers where appropriate.

8. **Network-first extensibility**
   - Allow memory tiers to span local and remote resources without collapsing policy boundaries.

### 3.2 Non-Goals

1. Making every artifact into permanent long-term memory.
2. Replacing all databases with one universal store.
3. Treating all memory as vector search.
4. Hiding trust and contradiction issues behind opaque scoring.
5. Moving most semantic reasoning into kernel space.

---

## 4. Core Definitions

### 4.1 Memory Control Plane

The **Memory Control Plane** is the subsystem responsible for managing the lifecycle, placement, indexing, linking, validation, and retention of memory-bearing state.

### 4.2 Memory Object

A **Memory Object** is a State Object or derived construct that is eligible for memory operations such as indexing, linking, promotion, or retrieval.

### 4.3 Memory Tier

A **Memory Tier** is a class of storage or representation defined by latency, durability, cost, and semantic richness.

### 4.4 Promotion

**Promotion** is the act of moving or copying a memory object or representation to a tier or surface that is more durable, more retrievable, or more semantically valuable.

### 4.5 Demotion

**Demotion** is the act of moving, compressing, or deprioritizing memory to a lower-value or lower-cost tier.

### 4.6 Decay

**Decay** is the process by which memory loses retrieval priority, authority, or persistence over time according to policy and usage patterns.

### 4.7 Forgetting

**Forgetting** is the intentional removal, tombstoning, or expiration of memory objects or relations.

### 4.8 Anti-Knowledge

**Anti-knowledge** is memory about invalidity, contradiction, uncertainty, or obsolescence that prevents stale or false material from being treated as reliable knowledge.

---

## 5. Design Principles

### 5.1 Memory Is Not Just Storage
The system must distinguish between raw persistence and usable memory.

### 5.2 Not All Memory Deserves Equal Treatment
Some memory should remain ephemeral; some should become durable; some should be forgotten.

### 5.3 Taxonomy and Graph Must Coexist
Hierarchical classification and relational structure solve different problems and should not be collapsed into one representation.

### 5.4 Trust Must Travel With Memory
Confidence, validation, contradiction, and freshness must be visible and queryable.

### 5.5 Retrieval Should Be Multi-Modal
Lexical, semantic, structural, temporal, and policy-aware retrieval should all be supported.

### 5.6 Promotion Must Be Earned
Material should not be promoted into trusted long-term memory merely because it exists.

### 5.7 Forgetting Is a First-Class Operation
Retention without decay leads to bloat and degraded relevance.

---

## 6. Memory Model Overview

The Memory Control Plane manages memory through four interacting structures:

```text
Memory =
  Tiering
+ Taxonomy
+ Graph
+ Retrieval Indexes
```

These operate over State Objects and their derived representations.

### 6.1 Memory Functions

The Memory Control Plane must support:

- admission
- normalization
- indexing
- linking
- validation
- promotion
- demotion
- retrieval
- contradiction handling
- decay
- deletion or tombstoning
- re-derivation

### 6.2 Memory as a Control Plane

The Memory Control Plane does not replace storage systems. It coordinates them. It should be able to direct:

- envelope stores
- blob stores
- lexical indexes
- vector indexes
- graph stores
- caches
- remote replicas

---

## 7. Memory Tiers

### 7.1 Canonical Tier Model

The initial canonical tier model is:

- **L0**: active execution-local working set
- **L1**: local transient cache
- **L2**: local durable object store
- **L3**: local semantic retrieval tier
- **L4**: long-term structured memory
- **L5**: remote or federated memory extension

### 7.2 Tier Definitions

#### L0: Active Execution-Local Working Set

Purpose:
- current cell inputs
- current cell outputs
- hot runtime buffers
- immediate token and stream state

Characteristics:
- lowest latency
- not durable by default
- scoped to execution contexts

Examples:
- current transcript chunk in use
- active prompt bundle
- intermediate plan state

#### L1: Local Transient Cache

Purpose:
- reusable but short-lived artifacts
- recently accessed representations
- temporary retrieval materialization

Characteristics:
- low latency
- local only
- evictable
- reproducible where possible

Examples:
- embedding cache
- parsed document cache
- recent graph neighborhood cache

#### L2: Local Durable Object Store

Purpose:
- canonical local durable state
- blob payloads
- object envelopes
- versioned artifacts

Characteristics:
- durable
- addressable
- audit-friendly
- local-first

Examples:
- transcript object
- meeting recording
- summary object
- validation report

#### L3: Local Semantic Retrieval Tier

Purpose:
- fast retrieval surfaces
- chunk indexes
- vector and lexical retrieval support
- retrieval-oriented representations

Characteristics:
- semantically optimized
- partially derived
- rebuildable from lower tiers when needed

Examples:
- semantic chunks
- embedding sets
- lexical postings
- rerankable candidate sets

#### L4: Long-Term Structured Memory

Purpose:
- durable knowledge organization
- graph structures
- taxonomic placement
- higher-value summaries and linked concepts

Characteristics:
- persistent
- trust-aware
- slower to promote
- harder to mutate casually

Examples:
- customer relationship nodes
- project memory summaries
- topic hierarchies
- validated fact candidates

#### L5: Remote or Federated Memory Extension

Purpose:
- remote object storage
- remote graph replicas
- shared organizational memory
- edge or cloud memory extension

Characteristics:
- higher latency
- policy-sensitive
- partially available
- replication-controlled

Examples:
- encrypted remote backups
- federated team memory
- edge caches
- cloud vector index replicas

---

## 8. Memory Object Classes

### 8.1 Core Memory-Bearing Object Types

The following initial State Object classes are memory-relevant:

- `memory.note`
- `memory.chunk`
- `memory.summary`
- `memory.fact_candidate`
- `document.transcript`
- `document.markdown`
- `index.embedding_set`
- `index.semantic_chunks`
- `graph.node`
- `graph.edge_set`
- `trace.validation`
- `task.result`
- `model.feedback`

### 8.2 Non-Memory Objects

Some State Objects may never become long-term memory and should remain primarily operational or ephemeral:

- temporary buffers
- retry scratch artifacts
- staging objects
- low-value intermediate tool outputs

### 8.3 Memory Capsules

The system may optionally define a higher-level construct called a **Memory Capsule**:

```json
{
  "id": "mc_01JR...",
  "root_ref": "so_01ABC...",
  "member_refs": [
    "so_01ABC...",
    "so_01DEF...",
    "so_01GHI..."
  ],
  "taxonomy_paths": [
    "work/customers/acme/meetings"
  ],
  "graph_links": [],
  "validation_state": "provisional"
}
```

A Memory Capsule groups related objects into a coherent retrievable unit without forcing monolithic storage.

---

## 9. Memory Lifecycle

### 9.1 Canonical Lifecycle

The default memory lifecycle is:

```text
observe
  -> normalize
  -> classify
  -> index
  -> validate
  -> promote
  -> link
  -> retrieve/use
  -> decay
  -> archive/forget/rederive
```

### 9.2 Lifecycle Semantics

#### observe
New material enters the system through ingestion or execution output.

#### normalize
The material is parsed, cleaned, segmented, typed, or represented in a standard form.

#### classify
Taxonomic hints and domain placement are assigned.

#### index
Retrieval-oriented structures are created.

#### validate
Trust, freshness, provenance, and contradiction are assessed.

#### promote
The object or representation is admitted into higher-value memory surfaces.

#### link
Relations are created into graph and lineage structures.

#### retrieve/use
The object participates in task execution, search, or reasoning.

#### decay
Priority, weight, or accessibility may decline according to policy.

#### archive/forget/rederive
The object is moved to cold storage, removed, or rebuilt later if needed.

---

## 10. Memory Admission

### 10.1 Admission Purpose

Not every object should enter the full memory system.

Admission determines:

- whether an object is memory-worthy
- which tiers it should enter
- which representations should be generated
- whether validation is required first

### 10.2 Admission Inputs

Admission decisions may consider:

- object type
- source credibility
- provenance
- access policy
- observed reuse likelihood
- freshness horizon
- cost to regenerate
- privacy sensitivity
- trust score
- relation density potential

### 10.3 Admission Profiles

Initial profiles:

- `ephemeral_only`
- `cacheable`
- `retrieval_candidate`
- `long_term_candidate`
- `graph_candidate`
- `quarantined`

### 10.4 Admission Rules

1. Low-value or easily reproducible artifacts should default to ephemeral or cacheable.
2. Sensitive artifacts must not be promoted beyond allowed tiers.
3. Contradicted or low-confidence artifacts may enter quarantine or contested memory rather than stable memory.
4. Long-term promotion should require provenance and minimum validation state.

---

## 11. Taxonomy Integration

### 11.1 Purpose

Taxonomy provides hierarchical organization that reduces search space and improves routing and memory compression.

### 11.2 Taxonomy Representation

Taxonomy paths are symbolic and human-readable in the initial design:

```text
work/customers/acme/meetings
research/model-routing/rfcs
personal/family-office/education
```

### 11.3 Taxonomy Rules

1. Taxonomy is placement, not identity.
2. Objects may belong to multiple taxonomy paths.
3. Taxonomy paths may be inferred, assigned, or corrected.
4. Taxonomy changes should be versioned or traceable when consequential.

### 11.4 Taxonomic Compression

The taxonomy can serve as a memory compression mechanism by permitting:

- path-based routing
- inheritance of policy defaults
- scoped retrieval
- summarization at branch boundaries

### 11.5 Taxonomy Objects

The system may store taxonomy branches as State Objects or dedicated registry records. Minimum branch metadata should include:

- path
- display name
- parent path
- policy inheritance
- freshness or retention defaults

---

## 12. Graph Integration

### 12.1 Purpose

Graph structure captures non-hierarchical relationships that taxonomy cannot represent well.

### 12.2 Graph Relation Types

Initial relation classes:

- `related_to`
- `derived_from`
- `supports_claim`
- `contradicts_claim`
- `references_entity`
- `belongs_to_taxonomy`
- `summarizes`
- `chunk_of`
- `supersedes`
- `validated_by`

### 12.3 Graph Rules

1. Every relation must have a type.
2. Weighted relations should record confidence or authority where appropriate.
3. Contradiction edges are first-class.
4. Graph creation should be policy- and cost-aware, not unconditional.

### 12.4 Neighborhood Retrieval

The graph layer should support:

- forward adjacency
- reverse adjacency
- type-filtered traversal
- weighted neighborhood expansion
- contradiction-aware neighborhood pruning

---

## 13. Retrieval Surfaces

### 13.1 Retrieval Modes

The Memory Control Plane must support at least the following retrieval modes:

- exact object lookup
- lexical retrieval
- semantic retrieval
- graph traversal
- taxonomy-scoped retrieval
- temporal retrieval
- policy-filtered retrieval
- trust-aware retrieval

### 13.2 Retrieval Surface Classes

#### Object Lookup
Retrieve by object ID or stable alias.

#### Lexical Surface
Keyword and phrase retrieval over text-bearing objects and chunks.

#### Semantic Surface
Vector or late-interaction retrieval over semantically indexed representations.

#### Graph Surface
Neighborhood, path, and relation traversal.

#### Taxonomy Surface
Restrict or bias retrieval to relevant branches.

#### Temporal Surface
Select by recency, time windows, or freshness class.

### 13.3 Retrieval Composition

Complex retrieval should be compositional, for example:

```text
taxonomy scope
  -> semantic retrieval
  -> graph expansion
  -> trust filter
  -> rerank
```

### 13.4 Trust-Aware Retrieval

Retrieval results should be eligible for filtering or reranking by:

- validation state
- contradiction status
- freshness
- provenance quality
- policy eligibility

---

## 14. Memory Representations

### 14.1 Representation Types

The Memory Control Plane may manage or request generation of:

- raw text
- normalized text
- semantic chunks
- embedding sets
- summaries
- entity maps
- graph links
- fact candidates
- contradiction reports
- retrieval caches

### 14.2 Representation Generation Rules

1. Representations are optional and workload-driven.
2. Generation must consider storage and compute cost.
3. Representations must be derived with provenance.
4. Representations may have independent validation state.

### 14.3 Chunking

Chunking should be treated as a memory design decision, not merely a preprocessing trick.

Chunking inputs may include:

- content type
- retrieval mode
- expected question granularity
- taxonomic placement
- graph relation density

---

## 15. Promotion and Demotion

### 15.1 Promotion Purpose

Promotion moves memory to higher-value surfaces when justified by trust, usage, or strategic importance.

### 15.2 Promotion Triggers

Promotion may occur based on:

- repeated access
- explicit user pinning
- successful validation
- graph centrality
- strategic taxonomy placement
- high regeneration cost
- business or operational importance

### 15.3 Promotion Targets

Examples:

- L1 -> L3 for frequent retrieval
- L2 -> L4 for validated durable memory
- L3 -> L4 for stable summaries or fact candidates
- L2/L4 -> L5 for policy-approved replication

### 15.4 Demotion Triggers

Demotion may occur based on:

- staleness
- low access frequency
- contradiction
- lower confidence
- storage pressure
- policy change
- successful compression or summarization

### 15.5 Promotion Rules

1. Promotion into L4 should generally require validation.
2. Promotion into L5 should require explicit replication policy.
3. Promotion should preserve lineage to lower-tier sources.
4. Promotion should not imply immutability; it should imply higher governance.

---

## 16. Trust, Validation, and Anti-Knowledge

### 16.1 Trust Model

Memory must explicitly model:

- confidence
- freshness
- provenance quality
- validation outcomes
- contradiction state

### 16.2 Memory Validation States

Recommended memory-specific states:

- `unvalidated`
- `provisional`
- `validated`
- `contested`
- `superseded`
- `stale`
- `quarantined`

### 16.3 Contradiction Handling

When two memory objects conflict, the system should support:

- contradiction edges
- scoped suppression
- ranked authority
- temporal supersession
- explicit contest status

### 16.4 Anti-Knowledge Objects

The system may materialize contradiction and invalidation as dedicated objects:

- `memory.invalidity_notice`
- `memory.contradiction_report`
- `trace.validation`

These prevent stale knowledge from being silently used.

### 16.5 Trust-Aware Promotion Rule

No probabilistic or inferred memory should be promoted to high-authority long-term memory without at least one of:

- deterministic verification
- corroboration
- repeated successful use
- trusted source provenance
- explicit human approval

---

## 17. Freshness, Decay, and Forgetting

### 17.1 Freshness Classes

The system may define freshness classes such as:

- `volatile`
- `short_horizon`
- `medium_horizon`
- `long_horizon`
- `archival`

### 17.2 Decay Model

Decay may affect:

- retrieval rank
- promotion eligibility
- graph edge weight
- default inclusion in memory contexts
- cache residency

### 17.3 Forgetting Modes

#### Hard Delete
Object or representation is deleted subject to policy and legal constraints.

#### Tombstone
Object is logically removed but identity and limited audit metadata remain.

#### Archive
Object is moved to lower-cost cold storage.

#### Re-derive
Object is removed from hot memory because it can be reconstructed.

### 17.4 Forgetting Rules

1. Forgetting must respect retention and compliance policy.
2. Contradicted memory should be suppressed quickly even if not deleted.
3. Low-value derived artifacts should be first candidates for forgetting.
4. High-cost-to-regenerate artifacts should prefer archive over delete.

---

## 18. Memory Compression and Summarization

### 18.1 Purpose

Long-lived memory cannot scale if it stores only raw detail.

### 18.2 Compression Modes

- structural summarization
- branch summarization
- representation compaction
- graph edge pruning
- chunk consolidation

### 18.3 Compression Rules

1. Compression must preserve lineage to original objects.
2. Summaries must not silently replace sources.
3. Compression should be reversible where feasible.
4. Summary objects should carry validation and freshness state.

---

## 19. Network-First Memory Behavior

### 19.1 Principle

Remote memory is not an afterthought; it is a normal extension of the memory hierarchy.

### 19.2 Remote Memory Uses

- encrypted backup
- federated team memory
- edge replica
- remote semantic index
- shared graph surface
- cold archive

### 19.3 Replication Rules

1. Replication must respect per-object policy.
2. Sensitive memory should default to local-only unless explicitly allowed.
3. Remote replicas must preserve provenance, policy, and validation metadata.
4. Network partitions must degrade gracefully without corrupting local authority.

### 19.4 Local Authority

Unless explicitly configured otherwise, local durable state should remain the authority boundary for private memory.

---

## 20. APIs

### 20.1 Memory Admission

```http
POST /memory/admit
```

Request:

```json
{
  "state_object_ref": "so_01ABC...",
  "profile": "retrieval_candidate"
}
```

### 20.2 Promote Memory

```http
POST /memory/promote
```

Request:

```json
{
  "state_object_ref": "so_01ABC...",
  "target_tier": "L4"
}
```

### 20.3 Demote Memory

```http
POST /memory/demote
```

### 20.4 Retrieve Memory

```http
POST /memory/retrieve
```

Request:

```json
{
  "query": "customer escalation about architecture review",
  "taxonomy_scope": [
    "work/customers/acme"
  ],
  "modes": [
    "semantic",
    "graph"
  ],
  "minimum_validation_state": "provisional"
}
```

### 20.5 Link Memory Objects

```http
POST /memory/link
```

### 20.6 Mark Contradiction

```http
POST /memory/contradict
```

### 20.7 Decay or Forget

```http
POST /memory/decay
POST /memory/forget
```

### 20.8 Rebuild Retrieval Surfaces

```http
POST /memory/reindex
```

---

## 21. CLI Surface

Suggested initial CLI:

```bash
memory admit so_01ABC... --profile retrieval_candidate
memory promote so_01ABC... --tier L4
memory retrieve --query "quarterly planning notes" --scope work/projects
memory link so_01ABC... --type related_to --target so_01DEF...
memory contradict so_01ABC... --target so_01XYZ...
memory decay so_01ABC...
memory forget so_01ABC... --mode tombstone
memory reindex --type document.transcript
```

---

## 22. Data Structures and Reference Schema

### 22.1 Memory Placement Record

```json
{
  "state_object_ref": "so_01ABC...",
  "tiers": ["L2", "L3"],
  "admission_profile": "retrieval_candidate",
  "freshness_class": "medium_horizon",
  "last_accessed_at": "2026-04-13T18:30:00Z",
  "promotion_score": 0.74,
  "decay_score": 0.18
}
```

### 22.2 Retrieval Index Registration

```json
{
  "state_object_ref": "so_01ABC...",
  "representations": [
    {
      "name": "semantic_chunks",
      "index_type": "vector",
      "index_ref": "idx_vec_local_001"
    },
    {
      "name": "raw_text",
      "index_type": "lexical",
      "index_ref": "idx_lex_local_001"
    }
  ]
}
```

### 22.3 Memory Trust Record

```json
{
  "state_object_ref": "so_01ABC...",
  "validation_state": "provisional",
  "confidence": 0.83,
  "freshness_state": "current",
  "contradiction_count": 0,
  "last_validated_at": "2026-04-13T18:31:00Z"
}
```

---

## 23. Security and Policy

### 23.1 Baseline Requirements

1. Memory promotion must honor object policy.
2. Retrieval must enforce access control.
3. Remote replication requires explicit approval.
4. Memory surfaces exposed to models must honor execution policy.

### 23.2 Sensitive Memory Classes

Examples of memory that may require stricter handling:

- customer data
- personal family data
- financial records
- private notes
- model evaluation traces with sensitive inputs

### 23.3 Policy Inheritance

If a derived memory object is created from sensitive parents, the stricter effective policy should apply unless explicitly loosened by authorized action.

---

## 24. POSIX Compatibility

### 24.1 Compatibility Thesis

The Memory Control Plane does not eliminate files. It surrounds them with richer retrieval and lifecycle behavior.

### 24.2 Compatibility Modes

#### Mode A: Shadow Memory
Files remain primary. The memory system builds indexes, graph links, and taxonomy placement as sidecar state.

#### Mode B: Memory-Backed View
Memory objects become primary and a filesystem view is projected outward.

The initial prototype should prioritize Mode A.

---

## 25. Reference Prototype Architecture

### 25.1 Initial Components

1. **Admission Service**
   - decides which objects enter memory and how

2. **Index Service**
   - lexical and vector indexing

3. **Graph Service**
   - relation storage and traversal

4. **Taxonomy Service**
   - path registry and branch rules

5. **Promotion/Decay Service**
   - movement between tiers and forgetting

6. **Trust Service**
   - contradiction and validation tracking

### 25.2 Recommended Initial Stack

- PostgreSQL for envelopes, placement records, taxonomy registry, and adjacency tables
- pgvector for initial semantic retrieval
- local filesystem or S3-compatible blob store for payloads
- Tantivy-compatible or PostgreSQL full-text index for lexical retrieval
- Python services for orchestration
- optional Rust services later for high-volume indexing or graph traversal

---

## 26. Observability

### 26.1 Required Metrics

- admission counts by profile
- objects per tier
- promotion and demotion counts
- retrieval hit rate
- retrieval latency
- contradiction rate
- decay rate
- stale memory rate
- remote replication volume
- rebuild cost for retrieval surfaces

### 26.2 Debug Surfaces

- why this object was promoted
- why this object was retrieved
- why this object was demoted
- what contradictions exist
- which relations support a summary or claim
- what memory branch was used in a routed task

---

## 27. Failure Modes

### 27.1 Memory Bloat
Too many low-value objects and representations accumulate.

**Mitigation:** admission control, decay, aggressive cache eviction, summary compaction.

### 27.2 Graph Explosion
Excessive low-signal edges reduce utility.

**Mitigation:** typed edge thresholds, pruning, edge aging.

### 27.3 Trust Collapse
Low-confidence outputs get promoted into authoritative memory.

**Mitigation:** validation gates and contested-memory handling.

### 27.4 Retrieval Myopia
Only one retrieval mode dominates and misses relevant material.

**Mitigation:** retrieval composition and multi-surface reranking.

### 27.5 Policy Leakage
Sensitive memory replicates or becomes retrievable where it should not.

**Mitigation:** strict policy inheritance and remote replication controls.

### 27.6 Staleness Drift
Outdated memory continues to dominate retrieval.

**Mitigation:** freshness-aware ranking, supersession edges, decay rules.

---

## 28. Implementation Plan

### Phase 1: Local Admission and Indexing
- admit objects into L2/L3
- generate lexical and vector retrieval surfaces
- basic taxonomy assignment
- adjacency-based relations in PostgreSQL

### Phase 2: Trust and Contradiction
- add validation records
- contested memory handling
- contradiction edges
- promotion gates

### Phase 3: Long-Term Structured Memory
- L4 branch summaries
- graph neighborhood retrieval
- memory capsule support
- taxonomic compression

### Phase 4: Decay and Forgetting
- freshness classes
- decay engine
- archive/tombstone behavior
- re-derivation policy

### Phase 5: Remote and Federated Memory
- L5 replication
- encrypted remote extensions
- offline-sync behavior
- cross-boundary policy enforcement

---

## 29. Open Questions

1. When should `memory.chunk` be separate objects versus attached representations?
2. How aggressive should branch summarization be before recall quality suffers?
3. What is the best default trust threshold for L4 promotion?
4. Should graph centrality influence promotion directly, or only retrieval ranking?
5. How should deletion propagate across summaries, graph relations, and remote replicas?
6. Which memory decay model is best: time-based, usage-based, contradiction-based, or hybrid?
7. How should anti-knowledge be surfaced to users and agents without becoming noisy?

---

## 30. Decision Summary

This RFC makes the following decisions:

1. Memory is a first-class control plane, not just passive storage.
2. The canonical tier model is L0 through L5.
3. Taxonomy, graph, and retrieval indexes must coexist as distinct but connected structures.
4. Admission, promotion, demotion, decay, and forgetting are explicit operations.
5. Trust, contradiction, and freshness must be queryable memory attributes.
6. Long-term memory promotion requires stronger validation than cache admission.
7. Remote memory is allowed but remains policy-governed and local-authority-aware.
8. Initial implementation should favor PostgreSQL, pgvector, local blob storage, and userland services.

---

## 31. Conclusion

The Memory Control Plane is the subsystem that makes the architecture durable.

It gives the operating environment a principled way to decide:

- what should be remembered
- what should be retrievable
- what should be trusted
- what should be linked
- what should decay
- what should be forgotten
- what can be regenerated on demand

Without this layer, the rest of the system becomes a loose collection of caches, indexes, prompts, and application conventions. With it, memory becomes a coherent operating resource.
