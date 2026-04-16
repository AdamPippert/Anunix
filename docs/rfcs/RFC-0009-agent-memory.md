# RFC-0009: Agent Memory and Semantic Retrieval

| Field      | Value                                                     |
|------------|-----------------------------------------------------------|
| RFC        | 0009                                                      |
| Title      | Agent Memory and Semantic Retrieval                        |
| Author     | Adam Pippert                                              |
| Status     | Draft                                                     |
| Created    | 2026-04-16                                                |
| Updated    | 2026-04-16                                                |
| Depends On | RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0007, RFC-0008|

---

## Executive Summary

An AI agent that cannot remember what it has done, what it has learned, or what it has observed is not an agent — it is a stateless function call. Every task an Anunix agent completes generates knowledge: which approaches worked, which failed, what the environment looked like, what decisions were made and why. In a classical system, this knowledge evaporates when the process exits. In Anunix, it must persist, organize itself, and become retrievable — not as raw logs, but as structured, graph-connected, semantically searchable memory.

RFC-0009 introduces the **Agent Memory System**: a kernel subsystem that gives agents episodic, semantic, and graph-structured memory with biologically-inspired consolidation. Agents accumulate **Memory Objects** (a new State Object type) during operation. These objects carry metadata that encodes graph relationships (edges with direction), enabling knowledge graph traversal. A kernel-resident lightweight embedding model provides semantic similarity search, so agents can retrieve memories by meaning, not just by key.

Memory is not free. Each agent operates within a **memory budget** determined by system resources and agent type. Memories decay based on access frequency, relevance scoring, and age — in that priority order. During off-peak utilization periods, the system enters a **consolidation phase** ("dreaming") where short-term episodic memories are evaluated, compressed, promoted to long-term storage, or discarded. This consolidation is scheduled by a time-series model that learns the system's utilization patterns and targets the average minima.

The design supports three deployment scales: kernelspace agents with local episodic memory (phone-class devices), userspace agents with graph and embedding search (workstation-class), and enterprise agents with remote distributed memory via raw storage mounts or trust-zone network peers.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert / public collaborators
**Depends on:** RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0007, RFC-0008

---

## 2. Problem Statement

### 2.1 The Stateless Agent Problem

When an Anunix agent completes a task — discovers hardware, calls a model API, processes a user request — it produces results but retains no memory of the experience. The next invocation starts from zero. It does not know that it already discovered this hardware configuration yesterday, that a particular API endpoint was unreliable last week, or that the user prefers concise answers. Every interaction is a first interaction.

### 2.2 Why Existing Primitives Are Insufficient

- **State Objects (RFC-0002)** can store data, but provide no semantic indexing, no graph structure, and no decay management. An agent could write observations as State Objects, but retrieval requires knowing the OID — there is no "find memories relevant to this query."

- **Memory Control Plane (RFC-0004)** defines six tiers (L0-L5) with decay and promotion, but the tier placement is based on access patterns, not semantic relevance. An infrequently-accessed but critically important memory decays just like irrelevant noise.

- **Capability Objects (RFC-0007)** capture validated procedures but not experiential knowledge. An agent's memory that "the DNS server was slow yesterday" is not a capability — it is contextual knowledge that should inform future routing decisions.

### 2.3 Why the Kernel Must Participate

Memory management cannot be a userspace library because:

1. **Embedding search** must be fast enough for real-time agent decisions. Kernel-level avoids IPC overhead.
2. **Memory budgets** must be enforced across all agents. Only the kernel has global resource visibility.
3. **Consolidation scheduling** requires system-wide utilization monitoring. The kernel owns the scheduler and timer.
4. **Graph traversal** crosses object boundaries. The kernel mediates all object access.
5. **Credential-scoped memory** must integrate with RFC-0008 access control.

---

## 3. Goals

### 3.1 Primary Goals

1. **Episodic memory as default.** Every kernelspace agent gets an append-only log of observations, actions, and outcomes — no configuration required.

2. **Graph relationships in metadata.** Memory Objects carry edges (typed, directed) as metadata. The knowledge graph emerges from object metadata, not from a separate index. The store remains flat; the graph is an overlay.

3. **Kernel-level semantic retrieval.** A lightweight embedding model runs in the kernel, providing similarity search over memory objects. Must work on phone-class devices.

4. **Three-tier decay.** Access-based decay (unused memories demote first), then relevance scoring (model evaluates importance), then time-based (age as tiebreaker). Different policies for kernelspace (never forget critical system knowledge) vs userspace (aggressively prune irrelevant memories).

5. **Memory budgets per agent.** Based on system memory and agent type (kernelspace, userspace, userspace+external). Slight headroom above strict allocation. Consensus mechanism to prevent memory hog agents.

6. **Consolidation during utilization minima.** The "dream" phase evaluates, compresses, and promotes short-term memories. Scheduled by a time-series model that learns resource utilization patterns.

7. **Private and shared memory.** Agents have private memory for dependency routing. All other memory is scoped by capability bindings.

8. **Remote memory via mounts and peers.** Raw storage mounts on flat networks (tailnet/local). Trust-zone network peers otherwise (RFC-0006). Primary use: distributed access, tiered caching, long-term retention.

### 3.2 Non-Goals

- **Natural language understanding.** The embedding model provides vector similarity, not comprehension. Understanding is the model's job.
- **Training or fine-tuning.** The embedding model is pre-trained and static. Updating it is out of scope.
- **Replacing the Memory Control Plane.** RFC-0004's tier system manages physical memory placement. RFC-0009 manages semantic memory organization. They complement each other.

---

## 4. Core Definitions

### 4.1 Memory Object

A **Memory Object** is a State Object of type `ANX_OBJ_MEMORY` containing:
- **Content:** The memory payload (text, structured data, or binary)
- **Embedding:** A fixed-size vector representation for similarity search
- **Graph edges:** Typed, directed edges to other Memory Objects stored as metadata
- **Episode context:** Task ID, agent ID, timestamp, sequence number
- **Decay state:** Access count, last access time, relevance score, decay score

### 4.2 Episode

An **Episode** is a sequence of Memory Objects produced during a single agent task. Episodes are bounded by task start/completion events. The episode provides temporal ordering and causal context.

### 4.3 Memory Graph

The **Memory Graph** is the set of all graph edges across all Memory Objects. Edges are stored as metadata on the source object. Edge types include: `CAUSED_BY`, `DERIVED_FROM`, `RELATED_TO`, `CONTRADICTS`, `SUPERSEDES`, `DEPENDS_ON`.

### 4.4 Consolidation ("Dreaming")

**Consolidation** is a background process that runs during detected utilization minima:
1. Evaluates short-term episodic memories for importance
2. Merges duplicate or near-duplicate memories
3. Promotes important memories to long-term storage
4. Discards low-relevance, low-access memories
5. Updates graph edges based on discovered relationships
6. Recomputes embeddings for modified memories

### 4.5 Memory Budget

A **Memory Budget** is a per-agent resource allocation:
- **Kernelspace agent:** Fixed allocation based on 10% of available heap, divided by active kernel agent count. Minimum 256KB.
- **Userspace agent:** Fixed allocation based on 20% of available heap. Minimum 1MB.
- **Userspace + external:** Local budget + remote tier capacity (unbounded, limited by remote storage).

---

## 5. Design Principles

### 5.1 Flat Store, Rich Metadata

The object store remains flat. Graph structure lives in object metadata, not in a separate database. This means every Memory Object is independently valid — it can be moved, replicated, or cached without breaking graph integrity. Edges reference OIDs; the graph materializes from metadata at query time.

### 5.2 Access-First Decay

The most important signal for memory relevance is whether anyone uses it. A memory accessed frequently stays; a memory never accessed after creation decays. Relevance scoring (by the embedding model) breaks ties. Time is the final tiebreaker, not the primary signal.

### 5.3 Phone-Scale Embedding

The embedding model must run on a phone. This means: no GPU required, sub-megabyte model size, sub-millisecond per embedding on a modern ARM core. A 128-dimension embedding from a TF-IDF + hashing approach or a quantized micro-transformer is acceptable. Accuracy matters less than being present — a 70% relevant result found in 100μs beats a 95% relevant result that requires a network round-trip.

### 5.4 Dreaming Is Not Optional

Consolidation is a system-level concern, not an agent decision. The kernel schedules it based on resource availability. An agent cannot opt out of consolidation — it can only influence importance scoring by marking memories as critical.

---

## 6. Implementation Phases

### Phase 1 — Episodic Memory Store
- `ANX_OBJ_MEMORY` type in State Object enum
- Memory Object creation, storage, and retrieval by OID
- Episode tracking (task-scoped append-only log)
- Basic graph edges in metadata (typed, directed)
- Memory budget enforcement (allocation + limit)
- Shell: `memory list`, `memory show <oid>`

### Phase 2 — Kernel Embedding
- Lightweight embedding model (TF-IDF + hashing or micro-transformer)
- Embedding computed on Memory Object creation
- Similarity search: `anx_memory_search(query, k)` returns top-k similar memories
- Shell: `memory search <query>`

### Phase 3 — Consolidation
- Time-series model for utilization pattern learning
- Background consolidation during detected minima
- Merge, promote, prune operations
- Relevance scoring integration with decay

### Phase 4 — Remote Memory
- Raw mount backend for flat-network storage
- Trust-zone peer backend for federated memory
- Tiered caching: local hot set + remote cold storage
- Deduplication across remote tiers

---

## 7. Amendments to Existing RFCs

### 7.1 RFC-0002: State Object Model
- Add `ANX_OBJ_MEMORY` to `enum anx_object_type`
- Memory Objects carry graph edge metadata in system_meta store

### 7.2 RFC-0003: Execution Cell Runtime
- Cells track their episode context (episode ID, sequence counter)
- Cell completion triggers episode finalization

### 7.3 RFC-0004: Memory Control Plane
- Agent memory budgets are a new dimension of memory plane management
- Consolidation interacts with tier promotion/demotion

### 7.4 RFC-0005: Routing Plane
- Route scoring can consider agent memory (e.g., "this engine was unreliable recently")
- Memory-aware routing is an input to feasibility filtering

---

## 8. Kernel API (Phase 1)

```c
/* Create a memory object in the current episode */
int anx_memory_create(const char *content, uint32_t content_len,
                       anx_oid_t *oid_out);

/* Add a graph edge between two memory objects */
int anx_memory_add_edge(const anx_oid_t *from, const anx_oid_t *to,
                         const char *edge_type);

/* Retrieve a memory object by OID */
int anx_memory_read(const anx_oid_t *oid,
                     void *buf, uint32_t buf_len, uint32_t *actual);

/* List memories in the current episode */
int anx_memory_episode_list(anx_oid_t *oids, uint32_t max,
                             uint32_t *count);

/* Get memory budget status for an agent */
int anx_memory_budget(uint32_t *used_bytes, uint32_t *limit_bytes);
```
