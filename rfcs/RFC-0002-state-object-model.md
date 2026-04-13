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
