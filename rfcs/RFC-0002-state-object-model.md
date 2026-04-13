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

---

## 5. Object Types

Anunix defines six canonical object types. Each type determines how the kernel interprets the payload, what system metadata fields are available, and what kernel-level optimizations are possible. This is a closed set: new types require an RFC amendment. The rationale is that the kernel must understand every type it encounters — an open type registry would reintroduce the "opaque bytes" problem that State Objects exist to solve.

### 5.1 `byte_data`

**Purpose:** Uninterpreted byte sequences. This is the type that maps directly to a POSIX file and serves as the backward-compatibility bridge.

**Payload structure:**

```
ByteDataPayload {
    bytes:    uint8[]       // raw byte content
}
```

**When to use:** Any data that has no richer representation in the type system, or any data accessed through the POSIX compatibility layer. Examples: binary executables, images, audio files, compressed archives, plain text files, configuration files.

**Kernel behavior:**

- The kernel treats the payload as opaque. It does not parse, validate, or index the content.
- Storage placement follows default policies (no type-specific optimization).
- The `schema_uri` field, if present, is treated as a MIME type hint (e.g., `"image/png"`, `"text/plain; charset=utf-8"`). The kernel stores it but does not enforce it.
- This is the only type for which the kernel makes *no* assumptions about payload content, honoring DG-8 (minimal overhead for simple cases).

**POSIX mapping:** A POSIX `open()` on a path that does not exist creates a `byte_data` object. A POSIX `open()` on an existing object of any type returns a byte-level view of the serialized payload — Section 11 defines the serialization rules.

### 5.2 `structured_data`

**Purpose:** Schema-conformant structured records. This type represents data that has a known, declared structure — the kernel can validate it, index specific fields, and enforce schema compatibility.

**Payload structure:**

```
StructuredDataPayload {
    encoding:       enum { JSON, MSGPACK, PROTOBUF, CBOR }
    schema_digest:  sha256          // hash of the schema document
    data:           uint8[]         // serialized content in the declared encoding
}
```

**When to use:** Configuration objects, API responses, database records, annotation sets, structured logs, any data where the producer and consumer agree on a schema. Examples: a JSON document representing user preferences, a Protobuf-encoded training record, a CBOR-serialized sensor reading.

**Kernel behavior:**

- The `schema_uri` and `schema_version` fields (Section 4.3) are **required** for this type. Creation without them is rejected.
- The kernel stores a `schema_digest` alongside the payload. If the schema document at `schema_uri` changes without a corresponding `schema_version` bump, the kernel can detect the mismatch.
- **Validation is deferred by default.** The kernel does not parse and validate every write against the schema (that would violate DG-8). Instead, validation occurs:
  - On explicit request via `so_validate()` (Section 12).
  - When an Execution Cell declares a schema requirement in its input contract.
  - When the object is sealed (Section 10.4).
- **Field-level access.** For supported encodings (JSON, MSGPACK), the kernel provides a `so_read_field()` syscall that extracts a specific field without the consumer deserializing the entire payload. This enables field-level access policies.

**Relationship to `byte_data`:** A JSON file opened through the POSIX layer is `byte_data`. A JSON document created through the native API with a declared schema is `structured_data`. The difference is the presence of a schema contract — `structured_data` is data the system can reason about; `byte_data` is data it stores but does not interpret.

### 5.3 `embedding`

**Purpose:** Dense numeric vectors produced by embedding models. These are first-class objects because AI-native workloads generate, store, query, and transform embeddings at massive scale, and the kernel can make significant optimization decisions when it understands the payload structure.

**Payload structure:**

```
EmbeddingPayload {
    dimensions:     uint32          // vector dimensionality (e.g., 768, 1536)
    element_type:   enum { FLOAT16, FLOAT32, FLOAT64, INT8, UINT8 }
    normalized:     bool            // whether the vector is L2-normalized
    vector:         element_type[]  // the dense vector, length == dimensions
}
```

**When to use:** Any vector representation of content produced by an embedding model. Examples: text embeddings from a language model, image embeddings from a vision model, audio embeddings, multi-modal embeddings.

**Kernel behavior:**

- **Placement.** The kernel knows the vector's size and element type, and can place it in memory regions optimized for vector operations (e.g., aligned, contiguous, near the vector index).
- **Indexing.** The Memory Control Plane (RFC-0004) can register embeddings with a semantic index for nearest-neighbor queries. The kernel facilitates this by exposing dimensionality and normalization status.
- **Validation.** On creation or write, the kernel verifies that the vector length matches the declared `dimensions` and that the byte count matches `dimensions * sizeof(element_type)`. Mismatches are rejected.
- **Schema binding.** The `schema_uri` for an embedding identifies the model that produced it (e.g., `"anunix:model/text-embedding-v3"`). This prevents accidental comparison of embeddings from incompatible models — the kernel can warn or reject when two embeddings with different `schema_uri` values are used in the same similarity operation.

**Why not `byte_data`?** Storing embeddings as raw bytes works, but the kernel cannot distinguish them from any other binary blob. It cannot validate dimensionality, optimize placement, or prevent cross-model comparison errors. Making embeddings a distinct type gives the kernel the information it needs to be a useful participant (DG-6).

### 5.4 `graph_node`

**Purpose:** A node in a typed, directed graph. Graph structures appear throughout AI workloads — knowledge graphs, dependency graphs, derivation graphs, ontologies — and representing them as first-class objects enables the kernel to maintain edge integrity, support traversal queries, and enforce graph-level policies.

**Payload structure:**

```
GraphNodePayload {
    node_type:      string          // application-defined type label
    properties:     map<string, Value>  // typed key-value properties
    edges:          Edge[]          // outgoing directed edges
}

Edge {
    label:          string          // relationship type (e.g., "derived_from", "contains")
    target_oid:     oid             // the State Object this edge points to
    weight:         float32?        // optional edge weight
    properties:     map<string, Value>  // optional edge properties
}
```

**When to use:** Any entity that participates in a graph of relationships. Examples: a concept node in a knowledge graph, an entity in an ontology, a step in a workflow DAG, a dependency in a package graph.

**Kernel behavior:**

- **Edge integrity.** The kernel verifies that `target_oid` in each edge references an existing State Object. If the target is deleted, the kernel can either remove the dangling edge, mark it as broken, or block the deletion — configurable via the target's retention policy.
- **Traversal support.** The kernel provides `so_traverse()` (Section 12) for breadth-first and depth-first traversal from a node, with optional filters on edge labels, node types, and depth limits. This is a kernel-level operation because the kernel can optimize it using object placement and pre-fetching.
- **Graph-level policies.** An access policy on a graph node can propagate to reachable nodes: "if you can read node A, you can read any node reachable via `contains` edges up to depth 3." This enables subgraph-level access control without enumerating every node.
- **Index participation.** Graph nodes are indexed by `node_type` for type-filtered queries ("all nodes of type `concept` in namespace X").

**Relationship to provenance:** The provenance graph (Section 4.6.1) is conceptually similar but distinct. Provenance edges are kernel-managed and immutable; `graph_node` edges are application-managed and mutable. An application might build a knowledge graph using `graph_node` objects, and each node would also have its own provenance record tracking how and when it was created.

### 5.5 `model_output`

**Purpose:** The result of a model inference operation. AI workloads produce a high volume of model outputs — predictions, classifications, generations, rankings — and these outputs have properties (confidence, model identity, input references) that no other type captures cleanly.

**Payload structure:**

```
ModelOutputPayload {
    model_id:       string          // identifier of the model (name + version)
    model_hash:     sha256?         // hash of the model weights, if available
    task_type:      string          // e.g., "classification", "generation", "embedding"
    result:         uint8[]         // the output data (encoding per schema_uri)
    confidence:     float32?        // model-reported confidence [0.0, 1.0]
    input_refs:     oid[]           // State Objects that were inputs to inference
    parameters:     map<string, Value>  // inference parameters (temperature, top_k, etc.)
    latency_ms:     uint32?         // inference wall-clock time in milliseconds
}
```

**When to use:** Any output produced by running a model. Examples: a classification label with confidence, a generated text response, a ranked list of recommendations, an anomaly detection result.

**Kernel behavior:**

- **Confidence-based routing.** The kernel can use the `confidence` field to make decisions: route low-confidence outputs to a human review queue, trigger re-inference with different parameters, or flag objects below a confidence threshold in query results.
- **Reproducibility tracking.** The combination of `model_id`, `model_hash`, `parameters`, and `input_refs` provides everything needed to re-run the inference. The provenance layer records whether the transformation was marked `reproducible`.
- **Automatic provenance.** When an Execution Cell produces a `model_output`, the kernel auto-populates the provenance `DERIVED_FROM` event using `input_refs`, and sets `actor_model` from `model_id`. No application intervention is needed.
- **Lineage queries.** "Show me all outputs produced by model X with confidence below 0.8 in the last 24 hours" is a kernel-level query, combining type, metadata, and provenance filters.

**Why not `structured_data`?** A model output *could* be stored as a generic structured record, but the kernel would lose the ability to reason about confidence, model identity, and input lineage as first-class properties. The `model_output` type makes these properties visible to the kernel without requiring schema-specific parsing.

### 5.6 `execution_trace`

**Purpose:** A structured record of an Execution Cell's run. Traces are the audit trail of computation — what ran, on what inputs, with what resources, producing what outputs, in how much time. They are the bridge between the State Object model and the Execution Cell runtime (RFC-0003).

**Payload structure:**

```
ExecutionTracePayload {
    cell_oid:       oid             // the Execution Cell that executed
    cell_type:      enum { CODE, MODEL, HYBRID }
    start_time:     datetime
    end_time:       datetime
    duration_ms:    uint64
    exit_status:    enum { SUCCESS, FAILURE, TIMEOUT, CANCELLED }
    input_oids:     oid[]           // State Objects consumed as input
    output_oids:    oid[]           // State Objects produced as output
    resource_usage: ResourceUsage   // CPU, memory, GPU, network consumed
    error:          string?         // error message if exit_status != SUCCESS
    log_ref:        oid?            // optional reference to a full log object
}

ResourceUsage {
    cpu_ms:         uint64          // CPU time in milliseconds
    memory_peak:    uint64          // peak memory in bytes
    gpu_ms:         uint64?         // GPU time in milliseconds
    gpu_memory_peak: uint64?        // peak GPU memory in bytes
    network_bytes:  uint64          // network I/O in bytes
    storage_bytes:  uint64          // storage I/O in bytes
}
```

**When to use:** Automatically created by the kernel at the end of every Execution Cell run. Applications do not typically create `execution_trace` objects directly — the kernel does.

**Kernel behavior:**

- **Automatic creation.** The kernel creates an `execution_trace` object when an Execution Cell completes. The cell's Execution Cell definition (RFC-0003) determines whether traces are retained, sampled, or discarded.
- **Scheduling optimization.** The scheduler (RFC-0005) uses historical traces to predict resource requirements for future cell executions: expected duration, memory footprint, GPU need. This enables better bin-packing and pre-emption decisions.
- **Debugging support.** Traces link inputs to outputs with full resource accounting. When an output is incorrect, the trace provides the starting point for investigation: what cell ran, what it consumed, and what it produced.
- **Cost accounting.** Resource usage fields enable per-cell, per-pipeline, and per-user cost attribution without external monitoring tools.
- **Retention.** Traces are often high-volume and low-value individually. The default retention policy for traces is short TTL (24 hours) with no version history. Applications can override this for critical pipelines.

### 5.7 Type Extension Process

The six canonical types are designed to cover the essential categories of state in AI-native workloads. However, the system must be able to evolve. Adding a new type requires:

1. An RFC amendment specifying the payload structure, kernel behavior, and interaction with existing types.
2. A demonstration that the new type enables kernel-level optimizations or enforcement that cannot be achieved by using one of the existing types with appropriate metadata.
3. Implementation in the kernel with backward-compatible storage format changes.

The bar for new types is deliberately high. A rich type system is useful; a proliferating type system is a maintenance burden. If a use case can be served by `structured_data` with an appropriate schema, it should be.

---

## 6. Object Identity & Addressing

Every State Object has a unique identity (its `oid`, defined in Section 4.2.1). But identity alone is not sufficient — programs need ways to *find* and *refer to* objects. This section defines the addressing model: how objects are named, how those names are resolved, and how different reference types serve different use cases.

### 6.1 The Three Reference Forms

Anunix supports three ways to refer to a State Object. Each form serves a different purpose, and programs choose the form that matches their intent.

| Form                | Resolves to         | Stable across | Example                                              |
|---------------------|---------------------|---------------|------------------------------------------------------|
| **OID reference**   | Exactly one object  | Lifetime      | `oid:01961f3a-7c00-7000-8000-000000000001`           |
| **Content reference** | Any object(s) with matching content | Content changes | `sha256:e3b0c44298fc1c149afb...` |
| **Path reference**  | One object via namespace lookup | Rebinding | `anunix://default/projects/alpha/config.json` |

#### 6.1.1 OID Reference

The most precise reference. An OID reference points to exactly one object and never changes meaning. It is the analogue of referencing an inode number directly.

Format: `oid:<uuid>`

```
oid:01961f3a-7c00-7000-8000-000000000001
```

- Always resolves to the same object, regardless of whether the object has been moved, renamed, or re-tiered.
- If the object has been deleted (or its TTL has expired), the reference resolves to a tombstone record that confirms the object existed and was removed. Tombstones are retained for a configurable period (default: 30 days).
- Versioned variant: `oid:<uuid>@<version>` references a specific historical version. For example, `oid:01961f3a-...@3` resolves to version 3 of the object, if retained per the retention policy.

**When to use:** Provenance records, edge targets in graph nodes, input/output references in execution traces — anywhere you need an unambiguous, permanent pointer to a specific object.

#### 6.1.2 Content Reference

A reference based on the SHA-256 hash of the payload (the `content_hash` field from Section 4.2.2). Content references answer the question "does an object with this exact content exist?" rather than "which specific object do I want?"

Format: `sha256:<hex-digest>`

```
sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
```

- May resolve to zero, one, or many objects. Multiple objects with identical payloads share the same content hash.
- The kernel maintains a content-hash index for deduplication and lookup. This index is best-effort: it covers objects on local and SSD tiers but may not include objects that have been migrated to archive or remote tiers.
- Content references are not versioned. They refer to payload content, not object identity.
- Content references are used for integrity verification ("does this object still contain what I expect?") and deduplication ("does this content already exist in the system?").

**When to use:** Cache invalidation, content-addressable storage patterns, integrity checks, deduplication queries.

#### 6.1.3 Path Reference

A human-readable name bound to an object through a namespace (Section 6.2). Path references are the closest analogue to POSIX file paths and are the primary interface for human users and the POSIX compatibility layer.

Format: `anunix://<namespace>/<path>`

```
anunix://default/projects/alpha/config.json
anunix://models/text-embedding-v3/weights
anunix://traces/2026/04/13/cell-abc123
```

- A path reference is a **binding**, not an identity. The same path can be rebound to a different object (like a symlink being updated). The object itself does not know or care what paths point to it.
- Path resolution goes through the namespace layer (Section 6.2), which maps hierarchical path segments to OIDs.
- Multiple paths can point to the same object (aliases). An object can exist with no path references at all — it is still addressable by OID.
- Path references are the default for POSIX compatibility: the POSIX path `/home/user/file.txt` maps to `anunix://posix/home/user/file.txt`.

**When to use:** Human-facing interfaces, CLI tools, POSIX compatibility, any context where discoverability and readability matter more than permanence.

### 6.2 Namespaces

A namespace is a hierarchical directory structure that maps path segments to OIDs. Namespaces are themselves State Objects (type: `structured_data`), which means they have provenance, access policies, and retention — but they are given special treatment by the kernel for performance.

#### 6.2.1 Namespace Structure

```
Namespace {
    name:           string          // e.g., "default", "posix", "models"
    root_oid:       oid             // OID of the root directory object
    policy:         NamespacePolicy
}

NamespacePolicy {
    default_access:     AccessPolicy    // default policy for new objects
    default_retention:  RetentionPolicy // default retention for new objects
    naming_rules:       NamingRules     // constraints on path segments
    quota:              Quota?          // optional storage quota
}

NamingRules {
    max_segment_length: uint32      // max bytes per path segment (default: 255)
    max_depth:          uint32      // max directory nesting depth (default: 256)
    allowed_chars:      regex       // permitted characters (default: POSIX portable)
    case_sensitive:     bool        // default: true
}
```

#### 6.2.2 System Namespaces

Anunix defines the following system namespaces. They exist at boot and cannot be deleted.

| Namespace   | Purpose                                                       |
|-------------|---------------------------------------------------------------|
| `posix`     | POSIX compatibility. Maps traditional paths to State Objects. Mounted as the root filesystem for legacy programs. |
| `default`   | The default namespace for native Anunix applications. New objects that do not specify a namespace are created here. |
| `system`    | Kernel-managed objects: scheduler state, memory plane configuration, system policies. Read-only for non-kernel cells. |
| `traces`    | Execution traces. Automatically populated by the kernel. Time-partitioned path structure (`/YYYY/MM/DD/`). |

#### 6.2.3 User-Defined Namespaces

Applications can create additional namespaces for organizational purposes:

```
so_namespace_create("models", {
    default_retention: { ttl: null, replicas: 2 },
    naming_rules: { case_sensitive: true }
})
```

User-defined namespaces are useful for isolation (a namespace per project, per team, or per pipeline), for applying bulk policies (all objects in the `ephemeral` namespace get a 1-hour TTL), and for quota management.

### 6.3 Path Resolution

When a program references an object by path, the kernel resolves it through the following steps:

```
resolve("anunix://models/text-embedding-v3/weights")

1. Look up namespace "models" → Namespace object, get root_oid
2. From root, resolve segment "text-embedding-v3" → directory entry → oid
3. From that oid, resolve segment "weights" → directory entry → oid
4. Return final oid (and object handle)
```

Path resolution is a kernel-internal operation. The namespace directory structure is cached in memory for active namespaces. Resolution cost is O(depth) in the number of path segments.

**Atomicity.** Path bindings are updated atomically. If a path is rebound from object A to object B, no observer will see a partially-updated state. Concurrent readers see either the old binding or the new binding, never an inconsistent state.

**Dangling paths.** If a path references an object that has been garbage-collected (TTL expired, no retention hold), the path entry is marked as **stale**. Accessing a stale path returns an error with the tombstone information for the deleted object, rather than silently succeeding. The kernel periodically sweeps stale entries.

### 6.4 Cross-References Between Objects

Objects frequently reference other objects — a `graph_node` has edges, a `model_output` has `input_refs`, provenance events have `input_oids`. All inter-object references use OID references.

This is a deliberate choice:

- **Paths can be rebound.** If object A references object B by path, and the path is later rebound to object C, A now silently points at C. OID references are stable.
- **Content hashes are ambiguous.** Multiple objects can share the same content hash. An OID reference is unambiguous.
- **OID references survive moves.** If object B is moved to a different namespace or a different storage tier, A's OID reference still resolves correctly.

The one exception is the `schema_uri` field (Section 4.3.2), which uses a URI rather than an OID. Schemas may be external to the Anunix system (e.g., a JSON Schema hosted on a web server), so a URI is more general. For schemas that are themselves State Objects, the URI can include the OID: `anunix:oid:01961f3a-...`.

### 6.5 Addressing and the POSIX Layer

The POSIX compatibility layer presents a traditional file-system view by mapping POSIX paths onto the `posix` namespace:

| POSIX operation          | Anunix equivalent                                     |
|--------------------------|-------------------------------------------------------|
| `open("/etc/config", …)` | Resolve `anunix://posix/etc/config` → OID → open handle |
| `stat("/etc/config")`    | Resolve path → read system metadata                   |
| `readdir("/etc/")`       | List directory entries in `anunix://posix/etc/`       |
| `link("a", "b")`         | Create a second path binding to the same OID          |
| `unlink("a")`            | Remove the path binding (object persists if other refs exist) |
| `rename("a", "b")`       | Atomic rebind: remove old path, create new path       |

POSIX programs see a familiar `/` rooted tree. They are unaware that paths are bindings into a namespace backed by State Objects. This transparency is essential for DG-7 (backward compatibility).

---

## 7. Metadata Model

Section 4.5 introduced the two metadata namespaces — system and user. This section specifies the full metadata schema, the rules for reading and writing metadata, and how metadata participates in queries and kernel decisions.

### 7.1 Principles

Metadata exists to make objects discoverable and classifiable without loading their payloads. Three principles govern its design:

1. **Separation of authority.** System metadata is the kernel's view of the object. User metadata is the application's view. Neither can overwrite the other, and the kernel guarantees consistency of its own fields without trusting application input.
2. **Queryability.** All metadata fields — system and user — are indexed and queryable. The kernel provides a structured query interface (Section 12, `so_query`) that supports filtering, sorting, and aggregation over metadata fields. Metadata that cannot be queried is dead weight.
3. **Proportional cost.** An object with one user metadata field and an object with a hundred must both be efficient to create and access. The metadata storage format uses a compact key-value encoding that avoids per-field overhead beyond the data itself.

### 7.2 System Metadata — Complete Schema

The system metadata fields introduced in Section 4.5.1 are the base set. The full schema includes additional type-specific fields that the kernel populates based on the object's `object_type`.

#### 7.2.1 Universal Fields (all object types)

| Field                | Type        | Set by   | Mutable | Description                                              |
|----------------------|-------------|----------|---------|----------------------------------------------------------|
| `sys.created_at`     | timestamp   | kernel   | no      | UTC creation time, nanosecond precision                  |
| `sys.modified_at`    | timestamp   | kernel   | yes*    | Last payload or governance mutation (* kernel-only)       |
| `sys.accessed_at`    | timestamp   | kernel   | yes*    | Last `read_payload` access (* kernel-only)               |
| `sys.size_bytes`     | uint64      | kernel   | yes*    | Payload size in bytes                                    |
| `sys.storage_tier`   | enum        | kernel   | yes*    | Current tier: `ram`, `ssd`, `archive`, `remote`          |
| `sys.sealed`         | bool        | kernel   | yes*    | Payload immutability flag (one-way: false → true)        |
| `sys.creator_cell`   | oid         | kernel   | no      | OID of the Execution Cell that created this object       |
| `sys.parent_oids`    | oid[]       | kernel   | no      | OIDs of direct parent objects (derivation)               |
| `sys.version`        | uint64      | kernel   | yes*    | Current version number (mirrors Identity.version)        |
| `sys.content_hash`   | sha256      | kernel   | yes*    | SHA-256 of payload (mirrors Identity.content_hash)       |
| `sys.object_type`    | enum        | kernel   | no      | The canonical type (mirrors Type.object_type)            |
| `sys.namespace`      | string      | kernel   | yes*    | Namespace this object resides in (if path-bound)         |
| `sys.path`           | string[]    | kernel   | yes*    | All paths currently bound to this object                 |
| `sys.provenance_len` | uint32      | kernel   | yes*    | Number of provenance events in the log                   |

Fields marked `yes*` are mutable only by the kernel — applications cannot set them directly.

#### 7.2.2 Type-Specific System Fields

The kernel adds additional system metadata fields depending on the `object_type`. These are populated automatically from the payload header and kept in sync on mutation.

**`embedding` objects:**

| Field                   | Type    | Description                              |
|-------------------------|---------|------------------------------------------|
| `sys.embed.dimensions`  | uint32  | Vector dimensionality                    |
| `sys.embed.element_type`| enum    | Element type (float16, float32, etc.)    |
| `sys.embed.normalized`  | bool    | Whether vector is L2-normalized          |
| `sys.embed.model_uri`   | string  | Schema URI identifying the embedding model |

**`graph_node` objects:**

| Field                   | Type    | Description                              |
|-------------------------|---------|------------------------------------------|
| `sys.graph.node_type`   | string  | Application-defined node type label      |
| `sys.graph.edge_count`  | uint32  | Number of outgoing edges                 |
| `sys.graph.edge_labels` | string[]| Distinct edge labels on outgoing edges   |

**`model_output` objects:**

| Field                   | Type    | Description                              |
|-------------------------|---------|------------------------------------------|
| `sys.model.model_id`    | string  | Model identifier and version             |
| `sys.model.task_type`   | string  | Task type (classification, generation, etc.) |
| `sys.model.confidence`  | float32 | Model-reported confidence [0.0, 1.0]     |
| `sys.model.input_count` | uint32  | Number of input objects                  |

**`execution_trace` objects:**

| Field                   | Type    | Description                              |
|-------------------------|---------|------------------------------------------|
| `sys.trace.cell_oid`    | oid     | OID of the Execution Cell                |
| `sys.trace.cell_type`   | enum    | Cell type (code, model, hybrid)          |
| `sys.trace.exit_status` | enum    | Outcome (success, failure, timeout, cancelled) |
| `sys.trace.duration_ms` | uint64  | Wall-clock duration in milliseconds      |
| `sys.trace.cpu_ms`      | uint64  | CPU time consumed                        |
| `sys.trace.memory_peak` | uint64  | Peak memory in bytes                     |

These type-specific fields are indexed alongside the universal fields. This enables efficient kernel-level queries like "all embeddings with dimensions == 768 and model_uri matching `text-embedding-v3`" or "all execution traces with exit_status == FAILURE and duration_ms > 30000".

### 7.3 User Metadata — Schema and Constraints

User metadata is an application-managed key-value map. The kernel stores, indexes, and queries it, but does not interpret its meaning.

#### 7.3.1 Key Format

- UTF-8 strings, 1–256 bytes.
- Must match the pattern `[a-zA-Z_][a-zA-Z0-9_.]*`. This ensures keys are valid identifiers in most programming languages and query syntaxes.
- Keys starting with `sys.` are reserved for system metadata. The kernel rejects any attempt to set a user metadata key with this prefix.
- Keys starting with `anunix.` are reserved for future Anunix extensions. The kernel rejects these as well.
- No other prefixes are reserved. Applications are encouraged to use a namespace prefix (e.g., `myapp.stage`, `pipeline.run_id`) to avoid collisions.

#### 7.3.2 Value Types

| Type    | Size limit  | Description                                      |
|---------|-------------|--------------------------------------------------|
| string  | 64 KiB      | UTF-8 text                                       |
| int64   | 8 bytes     | Signed 64-bit integer                            |
| float64 | 8 bytes     | IEEE 754 double-precision float                  |
| bool    | 1 byte      | True or false                                    |
| bytes   | 64 KiB      | Arbitrary byte sequence (not indexed by content) |
| list    | 64 KiB total| Ordered list of values (single type per list)    |

All value types except `bytes` are indexed for query predicates. `bytes` values are stored but only queryable by existence (`HAS(key)`), not by content.

#### 7.3.3 Limits

| Constraint              | Limit     | Rationale                                      |
|-------------------------|-----------|-------------------------------------------------|
| Max keys per object     | 1,024     | Prevents metadata bloat on individual objects    |
| Max total size per object | 1 MiB  | Keeps metadata loadable in a single page         |
| Max key length          | 256 bytes | Keeps index entries compact                      |
| Max value size          | 64 KiB   | Prevents using metadata as payload bypass        |

If an application needs to store more than 1 MiB of descriptive data, it should create a separate `structured_data` State Object and reference it via a user metadata key (e.g., `myapp.extended_meta_oid`).

### 7.4 Semantic Annotations

Semantic annotations are a specific pattern within user metadata that enables higher-level classification of objects. They are not a separate system — they are conventions built on user metadata — but they are important enough to standardize.

#### 7.4.1 Standard Annotation Keys

The following user metadata keys are recognized by Anunix tooling (CLI, query interfaces, dashboards) as semantic annotations. They are not enforced by the kernel, but applications that use them benefit from interoperability.

| Key                   | Type    | Purpose                                         |
|-----------------------|---------|--------------------------------------------------|
| `anno.tags`           | list    | Free-form classification tags                    |
| `anno.description`    | string  | Human-readable description of the object         |
| `anno.source`         | string  | Logical source system or pipeline name           |
| `anno.stage`          | string  | Pipeline stage (e.g., "raw", "processed", "final") |
| `anno.language`       | string  | Natural language of content (BCP-47 code)        |
| `anno.sensitivity`    | string  | Data sensitivity level ("public", "internal", "confidential", "restricted") |
| `anno.project`        | string  | Project or team identifier                       |
| `anno.expires_reason` | string  | Human-readable reason for the retention TTL      |

#### 7.4.2 Why Conventions, Not Schema

Making these annotations a kernel-enforced schema would violate DG-8 (minimal overhead) — every object would carry the cost of a fixed annotation structure whether it uses it or not. By defining them as conventions on user metadata, applications opt in only when they benefit, and the kernel indexes them just like any other user metadata.

### 7.5 Metadata Queries

The kernel supports structured queries over metadata fields. The query language is defined in Section 12 (`so_query`), but the key semantics are:

#### 7.5.1 Supported Predicates

| Predicate      | Applies to         | Example                                       |
|----------------|--------------------|------------------------------------------------|
| `EQ(k, v)`    | All indexed types   | `EQ("sys.object_type", "embedding")`          |
| `NEQ(k, v)`   | All indexed types   | `NEQ("sys.trace.exit_status", "SUCCESS")`     |
| `GT(k, v)`    | int64, float64, timestamp | `GT("sys.size_bytes", 1048576)`         |
| `LT(k, v)`    | int64, float64, timestamp | `LT("sys.model.confidence", 0.8)`      |
| `GTE(k, v)`   | int64, float64, timestamp | `GTE("sys.created_at", "2026-04-01T00:00:00Z")` |
| `LTE(k, v)`   | int64, float64, timestamp | `LTE("sys.trace.duration_ms", 5000)`   |
| `IN(k, vs)`   | All indexed types   | `IN("anno.stage", ["raw", "processed"])`      |
| `HAS(k)`      | All types           | `HAS("anno.project")`                         |
| `PREFIX(k, v)` | string             | `PREFIX("sys.path", "/projects/alpha/")`       |
| `CONTAINS(k, v)` | list            | `CONTAINS("anno.tags", "training")`            |

#### 7.5.2 Compound Queries

Predicates can be combined with `AND`, `OR`, and `NOT`:

```
AND(
    EQ("sys.object_type", "model_output"),
    LT("sys.model.confidence", 0.8),
    GTE("sys.created_at", "2026-04-12T00:00:00Z"),
    CONTAINS("anno.tags", "production")
)
```

This query returns all model outputs from the last day with confidence below 0.8 that are tagged as production data — a kernel-level operation that requires no application-specific indexing.

#### 7.5.3 Result Ordering and Pagination

Query results can be ordered by any indexed metadata field (ascending or descending) and paginated with cursor-based pagination. The default ordering is `sys.created_at DESC` (newest first). The kernel returns results as a stream of OIDs; the caller decides whether to load metadata, payloads, or both.

### 7.6 Metadata and the POSIX Layer

POSIX `stat()` maps to a subset of system metadata:

| `stat` field   | Mapped from                                              |
|----------------|----------------------------------------------------------|
| `st_size`      | `sys.size_bytes`                                         |
| `st_atime`     | `sys.accessed_at`                                        |
| `st_mtime`     | `sys.modified_at`                                        |
| `st_ctime`     | `sys.created_at` (note: POSIX ctime is "change time," but Anunix maps it to creation for simplicity; `sys.modified_at` covers the change-time role) |
| `st_mode`      | Derived from the object's access policy (Section 4.6.2)  |
| `st_uid/st_gid`| Mapped from the creator cell's associated user/group identity |
| `st_ino`       | Low 64 bits of the `oid` (collision-possible but rare; sufficient for POSIX inode semantics) |
| `st_nlink`     | Count of path bindings to this object                    |

Extended attributes (`xattr`) map to user metadata keys. `getxattr("user.myapp.stage")` reads the user metadata key `myapp.stage`. This provides a natural POSIX-compatible path for applications that want to use metadata without native Anunix APIs.
