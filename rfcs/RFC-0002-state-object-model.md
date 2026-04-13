# RFC-0002: State Object Model

| Field       | Value                          |
|-------------|--------------------------------|
| RFC         | 0002                           |
| Title       | State Object Model             |
| Author      | Adam Pippert                   |
| Status      | Draft                          |
| Created     | 2026-04-13                     |
| Requires    | RFC-0001 (Architecture Thesis) |
| Supersedes  | —                              |

## 1. Abstract

In a traditional UNIX system, the **file** is the universal abstraction: an opaque sequence of bytes identified by a path in a hierarchical namespace. This design is simple and composable, but it forces all higher-level semantics — structure, provenance, access policy, lifecycle — into userland conventions that the kernel cannot reason about or enforce.

The **State Object** is Anunix's replacement for the file. A State Object is a self-describing, policy-governed unit of state that carries its own metadata, provenance history, and access rules as intrinsic properties rather than external conventions. Where a UNIX file is passive data that programs interpret, a State Object is an active participant in the system: the kernel understands its type, tracks its lineage, enforces its policies, and can make scheduling and storage decisions based on its semantic properties.

This RFC defines:

- The structure and fields of a State Object
- The six canonical object types
- The identity and addressing model
- The metadata, provenance, and policy schemas
- The object lifecycle (creation through deletion)
- The POSIX compatibility mapping
- The kernel system call interface

State Objects are the foundational primitive of Anunix. Every other subsystem — Execution Cells (RFC-0003), the Memory Control Plane (RFC-0004), and the Scheduler (RFC-0005) — operates on State Objects as its unit of work.

---

## 2. Motivation

### 2.1 The POSIX File Model and Its Assumptions

The POSIX file model rests on a small set of assumptions that were reasonable in 1970 and remain useful today:

1. **Data is bytes.** A file is an uninterpreted byte sequence. Interpretation is the responsibility of the program that opens it.
2. **Names are paths.** Files live in a single hierarchical namespace. A path like `/home/user/report.pdf` is both the identity and the location of the data.
3. **Metadata is minimal.** The kernel tracks ownership, permissions (rwx bits), size, and timestamps. Everything else — MIME type, schema, authorship, version history — is a userland convention.
4. **Lifecycle is manual.** Files exist until someone deletes them. There is no kernel-level concept of expiration, retention policy, or archival.
5. **Operations are stateless.** `open`, `read`, `write`, and `close` operate on byte offsets. The kernel does not know or care what the bytes mean.

These assumptions produce a system that is universal (anything can be a byte sequence) and composable (any program can read any file). That universality is worth preserving. The problem is what it *excludes*.

### 2.2 What the File Model Cannot Express

The following properties are increasingly essential to modern workloads — and especially to AI-native workloads — but have no representation in the POSIX file model:

**Provenance.** Where did this data come from? What transformations produced it? Which model generated it, at what confidence level, with which parameters? In a file-based system, provenance is tracked by convention (README files, naming schemes, external databases) or not at all. The kernel has no mechanism to enforce provenance tracking or to answer queries like "show me everything derived from this dataset."

**Semantic type.** A file's type is encoded in its extension (`.csv`, `.json`, `.pt`) or in magic bytes at the start of the content. The kernel cannot use type information to make storage decisions (embeddings should live near a vector index), scheduling decisions (model weights need GPU-attached memory), or validation decisions (a JSON object should be parseable). Every program must rediscover the type and re-validate the content independently.

**Structure.** Many objects in an AI pipeline are not flat byte sequences. An embedding is a dense vector with a known dimensionality. A knowledge graph is a set of typed edges. A model checkpoint is a tree of named tensors with shape metadata. Forcing these into flat files means every consumer must deserialize from scratch, and the kernel cannot help with partial access, indexing, or integrity checking.

**Lifecycle policy.** Some data is ephemeral (intermediate computation results that should be garbage collected after an hour). Some data is immutable (a sealed model checkpoint that must never be modified). Some data has regulatory requirements (personal data that must be deleted within 30 days of a request). POSIX has no way to express these constraints. They live in application logic, cron jobs, and operational runbooks — all outside the kernel's awareness.

**Relationships.** A transcript was derived from an audio file. A summary was derived from a transcript. Action items were extracted from the summary. These derivation relationships are invisible to the file system. You cannot ask "what depends on this object?" or "what is the full lineage of this result?" without external tooling.

**Access granularity.** POSIX permissions operate at the file level: read, write, or execute, for owner, group, or other. There is no way to express "this Execution Cell may read the embeddings in this object but not the raw text" or "this object may only be accessed by cells that have been audited for PII handling." Policy is coarse where AI workloads need it to be fine-grained.

### 2.3 The Cost of Convention-Based Solutions

The standard response to these gaps is to build convention-based solutions in userland:

- **Provenance:** MLflow, DVC, W&B lineage tracking
- **Type safety:** File extensions, schema registries, validation layers
- **Lifecycle:** Cron-based cleanup, TTL fields in databases
- **Relationships:** External graph databases, metadata catalogs

These solutions work, but they share a critical weakness: **the kernel does not participate.** This means:

- **No enforcement.** A provenance-tracking convention can be ignored. A lifecycle policy implemented in a cron job can be circumvented by creating files outside the tracked directory. There is no kernel-level guarantee.
- **No optimization.** The kernel cannot place embedding objects near the vector index, or pre-fetch a model's dependency graph before an Execution Cell starts, because it does not know what the data *is*.
- **No composition.** Each convention is a silo. MLflow provenance does not talk to the cron-based lifecycle manager. The schema registry does not inform the storage layer. Integration is application-level glue code, fragile and specific to each pipeline.
- **No introspection.** You cannot ask the operating system "what state exists in this system, what type is it, where did it come from, and when will it expire?" You can only ask individual tools, each with partial knowledge.

### 2.4 The State Object Alternative

The State Object model addresses these gaps by moving semantics *into* the object and *into* the kernel:

| Property       | POSIX File                              | State Object                                     |
|----------------|-----------------------------------------|--------------------------------------------------|
| Identity       | Path in a hierarchical namespace        | Globally unique URN + content-addressable hash    |
| Type           | Extension convention or magic bytes     | Kernel-enforced type tag from a known set         |
| Structure      | Opaque byte sequence                    | Typed payload the kernel can partially interpret   |
| Metadata       | Ownership, permissions, timestamps      | Extensible key-value metadata with system fields  |
| Provenance     | None                                    | Immutable, append-only lineage record             |
| Access policy  | rwx bits for owner/group/other          | Capability-based, field-level, auditable          |
| Lifecycle      | Exists until manually deleted           | Policy-driven: TTL, sealing, tiered retention     |
| Relationships  | None (directory hierarchy only)         | Explicit derivation and dependency edges          |

The goal is **not** to make files more complex. The goal is to make the operating system a *participant* in managing state — so that provenance, policy, and optimization are systemic properties rather than per-application afterthoughts.

### 2.5 Design Constraint: Backward Compatibility

Anunix does not discard the POSIX model. A traditional byte stream is a valid State Object (type: `byte_data`, no provenance, default policies). The POSIX compatibility layer (Section 11) maps `open`/`read`/`write`/`close` onto State Object operations transparently. Existing programs run without modification. The difference is that new programs *can* use richer semantics, and the kernel *can* reason about state — without forcing legacy tools to change.

---

## 3. Design Goals

The State Object model is governed by the following design goals, listed in priority order. When goals conflict, higher-priority goals take precedence.

### DG-1: Universality

Every piece of persistent or semi-persistent state in the system is a State Object. There is no separate concept of "file," "blob," "record," or "artifact" at the kernel level. A raw byte stream is a State Object. A 768-dimensional embedding vector is a State Object. A model checkpoint containing billions of parameters is a State Object. This uniformity is what allows the rest of the system — scheduling, memory management, provenance tracking — to operate on a single abstraction.

**Test:** If a piece of state exists in the system and is not a State Object, the design has failed.

### DG-2: Self-Description

A State Object carries enough information for the kernel to understand what it is without opening or parsing its payload. The object's type, structure, schema version, and semantic annotations are part of the object itself — not encoded in a file extension, not stored in a sidecar, not inferred by heuristic. Any component in the system can inspect an object's metadata and make decisions (routing, placement, validation) without deserializing the data.

**Test:** If a component must parse an object's payload to determine what the object *is*, the design has failed.

### DG-3: Provenance by Default

Every State Object records its origin and transformation history as an intrinsic, immutable property. Provenance is not opt-in. It is not a feature of a particular tool or framework. It is a system guarantee: for any object, you can answer "where did this come from, what produced it, and what was it derived from?" The provenance record is append-only; it cannot be retroactively edited or stripped.

**Test:** If an object exists in the system with no provenance record, and it was not explicitly created as a POSIX-compatibility shim, the design has failed.

### DG-4: Policy as a First-Class Property

Access control, retention rules, and lifecycle behavior are part of the object's definition, not external configuration. An object can declare "I expire after 24 hours," "I am immutable once sealed," "only audited PII-handling cells may read my raw content," or "I must be replicated to at least two storage tiers." The kernel enforces these policies. They travel with the object if it is moved, copied, or transmitted.

**Test:** If a policy can be silently circumvented by accessing the object through a different interface, the design has failed.

### DG-5: Composability

State Objects are designed to be inputs and outputs of transformations. An Execution Cell (RFC-0003) takes State Objects as input and produces State Objects as output. Semantic Streams carry State Objects between cells. The model is explicitly pipeline-friendly: objects flow through chains of transformations, accumulating provenance at each step, without requiring format conversion or wrapper logic between stages.

**Test:** If connecting two Execution Cells requires a format conversion step that is not itself an Execution Cell, the design has failed.

### DG-6: Kernel Participation

The kernel can make informed decisions about State Objects because it understands their metadata. This means:

- **Storage placement.** Embeddings can be placed near the vector index. Frequently accessed model weights can be pinned in GPU-attached memory. Cold archival objects can be demoted to slower tiers.
- **Pre-fetching.** When an Execution Cell declares its input objects, the kernel can begin loading them before the cell starts.
- **Validation.** The kernel can reject a write that violates an object's declared schema, or block access that violates its policy, at the syscall boundary.
- **Garbage collection.** The kernel can reclaim objects whose TTL has expired or whose retention policy is satisfied, without relying on userland cron jobs.

**Test:** If the kernel treats a State Object as an opaque blob and makes no decisions based on its metadata, the object is effectively a POSIX file and the design goal is not met.

### DG-7: Backward Compatibility

Any valid POSIX file operation must produce correct results when applied to the Anunix system through the compatibility layer. Existing binaries that use `open`, `read`, `write`, `close`, `stat`, `readdir`, and related syscalls must work without modification. The compatibility layer may add provenance records and default metadata, but it must never reject an operation that POSIX would accept. Performance of POSIX-mode operations must be within 10% of an equivalent traditional file system for common workloads.

**Test:** If an existing POSIX binary produces different results or fails when run on Anunix (absent bugs), the design has failed.

### DG-8: Minimal Overhead for Simple Cases

The cost of the State Object model must be proportional to the features used. A simple byte-stream object with default metadata and no custom policies should have near-zero overhead compared to a POSIX file. The metadata, provenance, and policy machinery should add cost only when it carries meaningful information. This prevents the abstraction from penalizing workloads that don't need its full capabilities.

**Test:** If creating and reading a simple byte-stream State Object is measurably slower than creating and reading a POSIX file (beyond a constant-time metadata allocation), the implementation must be optimized before the design is considered complete.

---

## 4. State Object Definition

This section defines the canonical structure of a State Object. Every State Object in the system conforms to this structure. Optional fields may be absent, but the structure itself is fixed — there is no concept of a "partial" State Object.

### 4.1 Top-Level Structure

A State Object consists of five sections, each with a distinct role:

```
┌─────────────────────────────────────────────┐
│              STATE OBJECT                    │
├─────────────────────────────────────────────┤
│  1. Identity          (immutable)           │
│     ├── oid            128-bit unique ID    │
│     ├── content_hash   SHA-256 of payload   │
│     └── version        monotonic counter    │
├─────────────────────────────────────────────┤
│  2. Type & Schema     (immutable after set) │
│     ├── object_type    enum (6 types)       │
│     ├── schema_uri     optional reference   │
│     └── schema_version semver               │
├─────────────────────────────────────────────┤
│  3. Payload           (mutable until seal)  │
│     └── data           type-dependent       │
├─────────────────────────────────────────────┤
│  4. Metadata          (mutable)             │
│     ├── system_meta    kernel-managed       │
│     └── user_meta      application-managed  │
├─────────────────────────────────────────────┤
│  5. Governance        (mutable by policy)   │
│     ├── provenance     append-only log      │
│     ├── access_policy  capability rules     │
│     └── retention      lifecycle rules      │
└─────────────────────────────────────────────┘
```

The sections have distinct mutability rules, summarized above and detailed below. These rules are enforced by the kernel — they are not conventions.

### 4.2 Identity

The Identity section uniquely identifies the object and its current version. It is assigned at creation time and managed exclusively by the kernel.

#### 4.2.1 Object ID (`oid`)

- A 128-bit universally unique identifier, generated by the kernel at creation time.
- Format: UUIDv7 (time-ordered, per RFC 9562). The time-ordering property allows efficient range queries ("all objects created between T1 and T2") and ensures IDs are roughly sortable by creation time.
- The `oid` is immutable for the lifetime of the object. It survives moves, copies (which get their own `oid`), and storage tier migrations.
- The `oid` is the primary key for all kernel-internal lookups. It is analogous to an inode number, but globally unique rather than per-filesystem.

#### 4.2.2 Content Hash (`content_hash`)

- A SHA-256 hash of the object's payload (Section 4.4).
- Updated by the kernel each time the payload is modified.
- Enables content-addressable references: two objects with identical payloads have identical `content_hash` values, regardless of their `oid`, metadata, or provenance.
- Used for integrity verification, deduplication, and cache validation.
- For objects with no payload (e.g., a graph node whose value is entirely in its edges), `content_hash` is the hash of the empty byte sequence.

#### 4.2.3 Version (`version`)

- A 64-bit unsigned integer, starting at 1 and incremented by the kernel on every mutation to the payload or governance sections.
- Metadata-only changes (Section 4.5) increment the version only if the change modifies system metadata. User metadata changes do not increment the version.
- The version number is strictly monotonic and never reused. Combined with the `oid`, it forms a unique reference to a specific point-in-time snapshot: `(oid, version)`.
- The kernel may retain historical versions according to the retention policy (Section 4.6.3). If historical versions are not retained, only the current `(oid, version)` is accessible.

### 4.3 Type & Schema

The Type & Schema section tells the kernel and all consumers what kind of data the object holds, without parsing the payload.

#### 4.3.1 Object Type (`object_type`)

A required field set at creation time. One of six canonical types, defined in detail in Section 5:

| Type               | Payload interpretation                        |
|--------------------|-----------------------------------------------|
| `byte_data`        | Uninterpreted byte sequence                   |
| `structured_data`  | Schema-conformant structured record            |
| `embedding`        | Dense numeric vector with fixed dimensionality |
| `graph_node`       | Node in a typed, directed graph                |
| `model_output`     | Result of a model inference operation          |
| `execution_trace`  | Record of an Execution Cell's run              |

The `object_type` is immutable after creation. If a transformation changes the type (e.g., audio bytes become a transcript), it creates a new State Object with the new type and a provenance link back to the original.

#### 4.3.2 Schema URI (`schema_uri`)

- An optional URI identifying the schema that the payload conforms to.
- For `structured_data` objects, this points to a JSON Schema, Protobuf definition, or equivalent.
- For `embedding` objects, this identifies the embedding model and expected dimensionality.
- For `byte_data` objects, this may specify a MIME type or be absent entirely.
- The kernel does not interpret the schema itself, but it stores the reference and can enforce that consumers declare schema compatibility before accessing the payload.

#### 4.3.3 Schema Version (`schema_version`)

- A semantic version string (e.g., `"1.2.0"`) indicating which version of the schema the payload conforms to.
- Enables the kernel to detect schema mismatches: if an Execution Cell expects `schema_version: "2.x"` and the object declares `"1.3.0"`, the kernel can reject the binding or flag a compatibility warning.
- Absent if `schema_uri` is absent.

### 4.4 Payload

The Payload section holds the object's data. Its internal structure depends on the `object_type`.

#### 4.4.1 General Properties

- The payload is the only section that holds application data. All other sections are metadata *about* the data.
- The payload is mutable until the object is **sealed** (Section 10.4). After sealing, any write to the payload is rejected by the kernel.
- The kernel tracks the payload size in bytes as system metadata (`sys.size_bytes`). For structured types, it may also track element counts (e.g., vector dimensionality, number of graph edges).
- The payload is stored separately from the metadata and governance sections. This allows the kernel to load an object's metadata without loading its (potentially large) payload, and to migrate payloads between storage tiers independently.

#### 4.4.2 Type-Specific Layout

Each `object_type` implies a payload layout, detailed in Section 5. Briefly:

- **`byte_data`:** Raw byte sequence. No kernel-imposed structure. This is the POSIX-compatible default.
- **`structured_data`:** A serialized record (e.g., JSON, MessagePack, Protobuf). The kernel stores it opaque but knows the schema and can delegate validation.
- **`embedding`:** A header (dimensionality, element type, normalization flag) followed by a dense numeric array. The kernel understands the header and can perform placement and indexing decisions.
- **`graph_node`:** A node ID, node type, property map, and edge list. The kernel maintains edge integrity and can traverse relationships.
- **`model_output`:** A result payload, a confidence score, the model identifier, and input references. The kernel understands confidence and can route based on it.
- **`execution_trace`:** A structured log of cell inputs, outputs, duration, resource usage, and exit status. The kernel uses this for scheduling optimization and debugging.

### 4.5 Metadata

The Metadata section carries descriptive information about the object. It is divided into two namespaces with different authority.

#### 4.5.1 System Metadata (`system_meta`)

Managed exclusively by the kernel. Applications can read but not write these fields.

| Field              | Type      | Description                                         |
|--------------------|-----------|-----------------------------------------------------|
| `sys.created_at`   | timestamp | Object creation time (set once, immutable)          |
| `sys.modified_at`  | timestamp | Last payload or governance modification             |
| `sys.accessed_at`  | timestamp | Last read access                                    |
| `sys.size_bytes`   | uint64    | Payload size in bytes                               |
| `sys.storage_tier` | enum      | Current storage location (ram, ssd, archive, remote)|
| `sys.sealed`       | bool      | Whether the object is sealed (immutable payload)    |
| `sys.creator_cell` | oid       | OID of the Execution Cell that created this object  |
| `sys.parent_oids`  | oid[]     | OIDs of objects this was directly derived from       |

`sys.parent_oids` is the fast path for provenance queries. The full provenance log (Section 4.6.1) contains richer detail, but `parent_oids` allows the kernel to traverse the derivation graph without loading provenance records.

#### 4.5.2 User Metadata (`user_meta`)

A key-value map managed by applications. The kernel stores it but does not interpret it.

- Keys are UTF-8 strings, maximum 256 bytes.
- Values are typed: string, integer, float, boolean, or byte array, maximum 64 KiB each.
- Total user metadata per object is capped at 1 MiB.
- User metadata is indexed by the kernel for query support. Applications can search for objects by metadata predicates (e.g., "all objects where `user.project == 'alpha'` and `user.stage == 'training'`").
- User metadata changes do not increment the object version or modify `sys.modified_at`. They are considered annotation, not mutation.

### 4.6 Governance

The Governance section controls the object's provenance, access, and lifecycle. It is the mechanism through which design goals DG-3 (Provenance by Default) and DG-4 (Policy as First-Class) are realized.

#### 4.6.1 Provenance Record

An append-only log of events that have affected this object. Each entry is a **Provenance Event**:

```
ProvenanceEvent {
    event_id:       uint64          // monotonic within this object
    timestamp:      datetime        // kernel-assigned wall clock time
    event_type:     enum {
                        CREATED,
                        MUTATED,
                        DERIVED_FROM,
                        SEALED,
                        POLICY_CHANGED,
                        ACCESSED,
                        MIGRATED
                    }
    actor_cell:     oid             // the Execution Cell that caused this event
    actor_model:    string?         // if actor was a model: model ID and version
    input_oids:     oid[]           // objects consumed as input (for DERIVED_FROM)
    confidence:     float32?        // model confidence, if applicable
    description:    string          // human/machine-readable summary
    reproducible:   bool            // can this transformation be exactly replayed?
}
```

Key properties:

- **Append-only.** Events are never modified or deleted. The provenance log is an immutable audit trail.
- **Kernel-enforced.** Applications cannot forge provenance events. The kernel generates them from observed system calls.
- **Inherited on derivation.** When an Execution Cell creates a new object from existing objects, the new object's provenance log begins with a `DERIVED_FROM` event that references the parent objects. The parents' provenance is not copied — it is reachable by following the graph.
- **Storage-efficient.** The `ACCESSED` event type is optional and governed by the object's access policy. For high-traffic objects, access logging can be disabled to prevent provenance log bloat.

#### 4.6.2 Access Policy

A set of capability-based rules that govern who can do what to this object. Unlike POSIX rwx bits, access policies are:

- **Identity-based on Execution Cells**, not users. The unit of access is the Execution Cell (RFC-0003), not the human user. A cell presents a capability token when accessing an object.
- **Operation-granular.** Supported operations: `read_payload`, `read_metadata`, `write_payload`, `write_metadata`, `seal`, `delete`, `derive` (create a child object), `query_provenance`.
- **Field-level for structured types.** For `structured_data` and `graph_node` objects, policies can restrict access to specific fields or properties.
- **Conditional.** Rules can include predicates: "allow `read_payload` if the requesting cell has the `pii_audited` capability."

The default policy for objects created through the POSIX compatibility layer maps to traditional rwx semantics. Objects created through native Anunix system calls must specify a policy or accept the system default (creator-cell has full access, others have `read_metadata` and `query_provenance`).

Access policy changes are logged as `POLICY_CHANGED` provenance events.

#### 4.6.3 Retention Policy

Rules governing the object's lifecycle. The kernel enforces these without relying on userland processes.

```
RetentionPolicy {
    ttl:              duration?     // time-to-live from creation; null = indefinite
    min_versions:     uint32        // minimum historical versions to retain (default: 1)
    max_versions:     uint32        // maximum historical versions (default: 1)
    tier_schedule:    TierRule[]    // automatic storage tier migration rules
    deletion_hold:    bool          // if true, object cannot be deleted (legal hold)
    archive_after:    duration?     // move to archive tier after this duration
    replicas:         uint8         // minimum number of storage replicas (default: 1)
}

TierRule {
    after:            duration      // time since last access
    move_to:          storage_tier  // target tier (ssd, archive, remote)
}
```

Key behaviors:

- **TTL expiration.** When a TTL expires, the kernel marks the object for garbage collection. The object becomes inaccessible immediately and is physically deleted asynchronously.
- **Version retention.** The kernel maintains between `min_versions` and `max_versions` historical snapshots. Older versions beyond `max_versions` are pruned. Fewer than `min_versions` is only possible if the object has fewer total versions.
- **Tier migration.** The `tier_schedule` allows automatic demotion of cold objects. A rule like `{after: "7d", move_to: "archive"}` moves the object to the archive tier if it has not been accessed in 7 days.
- **Legal hold.** The `deletion_hold` flag prevents deletion regardless of TTL or any other rule. This supports regulatory compliance scenarios.
- **Replication.** The `replicas` field ensures durability. The Memory Control Plane (RFC-0004) is responsible for placement; the retention policy declares the requirement.

Retention policy changes are logged as `POLICY_CHANGED` provenance events.
