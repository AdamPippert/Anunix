# RFC-0027: ICM over State Objects — Information Context Management as a Native Convention

| Field      | Value                                                                  |
|------------|------------------------------------------------------------------------|
| RFC        | 0027                                                                   |
| Title      | ICM over State Objects — Information Context Management as a Native Convention |
| Author     | Adam Pippert                                                           |
| Status     | Review                                                                 |
| Created    | 2026-05-31                                                             |
| Updated    | 2026-06-01                                                             |
| Depends On | RFC-0002, RFC-0003, RFC-0007, RFC-0011, RFC-0018                       |
| Blocks     | —                                                                      |

---

## Executive Summary

Information Context Management (ICM) is a methodology for organizing the
artifacts an agent system reads and writes: documents, tools, code, and
generated outputs. Its central rule is **artifact identity is global; artifact
role is local** — the same artifact is an *output* in the workflow that creates
it, an *input* in one that consumes it, a *reference* in one that only needs to
understand it, a *dependency* in a build, and a *constraint* in a compliance
review.

On a POSIX filesystem, ICM must be *emulated*: folders stand in for identity,
hand-maintained manifests (`manifest.yml`, `inputs.yml`) carry role and policy,
a generator script scans the tree to produce a catalog, and a separate document
records the dependency graph. None of it is enforced; all of it can drift.

**Anunix already provides, at the kernel level, every primitive ICM emulates.**
A State Object (RFC-0002) *is* a globally-identified artifact with intrinsic
metadata, an append-only provenance graph, and kernel-enforced access policy. A
Workflow Object (RFC-0018) *is* a stage graph. This RFC therefore does **not**
introduce a new object type or kernel primitive. It defines ICM as a thin
**convention** layered on existing metadata, plus the `icm` agent-native utility
(specified in RFC-0011 §4.10) that renders ICM views (catalog, dependency graph,
role-scoped context) directly from the object store instead of generating and
maintaining files.

The contribution is a mapping and a tool, not new kernel surface.

---

## 1. Status

**Status:** Review
**Author:** Adam Pippert
**Depends on:** RFC-0002 (State Object Model), RFC-0003 (Execution Cell
Runtime), RFC-0007 (Capability Objects), RFC-0011 (Agent-Native Utilities —
hosts the `icm` tool), RFC-0018 (Workflow Objects)
**Blocks:** —

---

## 2. Motivation

### 2.1 What ICM Is

ICM is summarized by four sentences:

> Shared things are referenced. Created things are written.
> Published things are versioned. Consumed things are declared.

And one rule: **identity is global, role is local**. A file is not intrinsically
an "input" or an "output" — its role is assigned by the stage that consumes it.
The same artifact serves many workflows in different roles without being
duplicated into each workflow's folder.

### 2.2 The POSIX Emulation Tax

On a conventional filesystem, expressing ICM requires building, by hand, the
metadata the filesystem cannot carry:

| ICM concern        | POSIX emulation                                  | Failure mode |
|--------------------|--------------------------------------------------|--------------|
| Global identity    | a folder name                                    | renamed/moved → references break |
| Local role         | a `role:` field in a per-stage `inputs.yml`      | advisory only; nothing enforces it |
| Authority / policy | a `write_policy: read-only` field                | honored by convention; nothing blocks a write |
| Catalog            | a generator script that walks the tree           | stale until re-run |
| Dependency graph   | a hand-written `dependency-graph.md`             | drifts from reality |
| Published version  | a `v0.4.2/` directory + a "latest" pointer file  | manual discipline |
| Snapshot           | a frozen copy of files                            | unverifiable; no integrity guarantee |

Every row is something Anunix already does intrinsically.

### 2.3 The Anunix Mapping

| ICM concept                         | Anunix primitive (existing)                                   |
|-------------------------------------|--------------------------------------------------------------|
| Artifact identity is **global**     | State Object `oid` (UUIDv7), RFC-0002 §4.2                    |
| Role is **local** to the consumer   | per-cell **capability token** (RFC-0007) + `anno.*` metadata |
| Manifest (type/authority/stack)     | `system_meta` + `user_meta` / `anno.*` (RFC-0002 §7)         |
| `write_policy: read-only`           | `access_policy` op set — **kernel-enforced** (RFC-0002 §4.6.2) |
| Dependency graph, **one-way**       | provenance graph (`DERIVED_FROM`, append-only DAG, §4.6.1, §8) |
| canonical / published / snapshot    | live OID ref / `oid@version` / **sealed** object + content hash |
| Catalog (generated by scanning)     | `so_query` / `anx_objstore_iterate` — the kernel *is* the index |
| Workflow stage                      | Workflow Object node (RFC-0018) / Execution Cell (RFC-0003)  |
| `knowledge/` (referenced not copied)| objects in a `knowledge` namespace, referenced by OID (§6.2) |
| "consumed things are declared"      | cell input contract (`input_oids`), kernel pre-fetches       |

The conclusion that motivates this RFC: **ICM-on-POSIX is a hand-rolled
emulation of the Anunix metadata layer.** On Anunix we should not recreate the
folders — we should expose the layer that already exists.

---

## 3. Goals

### 3.1 Primary Goals

1. **No new kernel primitive.** ICM is expressed entirely with RFC-0002
   metadata, RFC-0007 capabilities, and RFC-0018 workflows. This RFC adds a
   convention and a userland tool, nothing in the kernel.
2. **Standard ICM annotation keys.** Define a reserved-by-convention set of
   `anno.icm.*` user-metadata keys that carry ICM classification (domain,
   authority, type, status) so tooling interoperates.
3. **Role is a view, not a copy.** A consuming stage declares the role it
   assigns to an artifact in its own context; the artifact is never duplicated.
4. **Catalog and dependency graph as live queries.** `anx icm catalog` and
   `anx icm graph` are rendered from the object store and the provenance graph
   at call time — never generated, never stale.
5. **One-way references preserved.** ICM forbids cycles; the provenance graph is
   acyclic by construction (derivation always flows from existing to new
   objects, RFC-0002 §8.1), so the property is enforced, not merely advised.
6. **POSIX-overlay parity.** The same conceptual workspace can be expressed on a
   POSIX host with the companion tool `icm-scaffold`
   (github.com/AdamPippert/repo-scaffold); RFC-0027 is the native counterpart.

### 3.2 Non-Goals

- **A workflow execution engine.** Stage execution is RFC-0018 / RFC-0003;
  RFC-0027 only annotates and views.
- **A new namespace mechanism.** Uses RFC-0002 §6.2 namespaces as-is.
- **A visual editor.** Catalog/graph rendering is textual in Phase 1; visual
  surfaces are deferred to the Interface Plane (RFC-0012).
- **Replacing manifests on POSIX.** The POSIX overlay tool stands on its own;
  this RFC does not change it.

---

## 4. The ICM Annotation Convention

RFC-0002 §7.4 establishes that semantic annotations live in `user_meta` under
reserved-by-convention key prefixes, indexed like any other metadata and never
kernel-enforced (preserving DG-8, minimal overhead). RFC-0027 reserves the
`anno.icm.*` sub-namespace.

### 4.1 Standard Keys

| Key                  | Type   | Meaning                                                       |
|----------------------|--------|--------------------------------------------------------------|
| `anno.icm.domain`    | list   | Domains the artifact belongs to (tags, not folders).         |
| `anno.icm.kind`      | string | Artifact kind: `app`, `lib`, `tool`, `workshop`, `config`, `experiment`, `sandbox`, `external`, `data`, `media`. |
| `anno.icm.authority` | string | `own` \| `own-local` \| `external` — provenance of authorship. |
| `anno.icm.status`    | string | `active` \| `stale` \| `scaffold` \| `empty`.                |
| `anno.icm.stack`     | string | Primary implementation stack (free text).                    |
| `anno.icm.entry`     | string | How to run/use the artifact.                                 |
| `anno.icm.published` | string | Release marker for a published artifact, e.g. `anx:pkg/foo@1.2.0`. Present only on published releases; absent on working artifacts. |

`anno.icm.domain` is a **tag list**, not a namespace placement (§9.1): an
artifact may carry many domains at once, modeling the n-to-m relation between
artifacts and the contexts they serve. `anno.icm.published` marks the ICM
lifecycle state "published things are versioned" — it is set on a *sealed*
object (RFC-0002 §10.4) and carries the release URI so a release is discoverable
by query rather than inferred from seal state (§9.3).

These mirror the fields a POSIX `manifest.yml` carries, but here they are
queryable kernel metadata on the artifact's State Object, not a sidecar file.

### 4.2 Role Is Not Stored on the Artifact

Critically, **role is not an `anno.icm.*` key on the artifact.** Role is local
to the consumer. A consuming stage records the role it assigns in *its own*
context — in Anunix terms, in the Workflow Object node that declares the
artifact as an input (RFC-0018), or in the requesting cell's capability scope
(RFC-0007). The same artifact, viewed from two stages, has two roles and one
identity. This is the direct realization of "identity is global, role is local."

### 4.3 Reserved Prefix

`anno.icm.` is reserved by this RFC. Per RFC-0002 §7.3.1, the kernel does not
interpret these keys; it stores and indexes them. Tools that understand ICM
(notably `anx icm`) interpret them. Applications must not use `anno.icm.*` for
unrelated purposes.

---

## 5. Views

`anx icm` renders three views, all from live state. None writes a catalog file.

### 5.1 Catalog View

Equivalent to the POSIX `catalog.md`, but a query result. The tool iterates the
object store (`anx_objstore_iterate`), reads each object's `anno.icm.*`
metadata, groups by `anno.icm.domain`, and prints a table. Because it reads the
store directly, it is never stale: there is no generation step to forget.

### 5.2 Dependency-Graph View

Equivalent to the POSIX `dependency-graph.md`, but read from the **provenance
graph**. For each artifact, the tool walks `DERIVED_FROM` edges (RFC-0002 §8.2
ancestry/descendant queries) and prints the one-way edges. Cycles cannot appear
because the provenance graph is acyclic by construction — the ICM "one-way
reference" rule is a kernel invariant here, not a linting concern.

### 5.3 Role-Scoped Context View

Given a workflow/stage (RFC-0018) or a capability scope (RFC-0007), the tool
lists the artifacts that stage consumes, with the **role that stage assigns**.
The same artifact appears with role `dependency` under one stage and `reference`
under another — surfaced from the consumers, never duplicated onto the artifact.

---

## 6. The `icm` Userland Tool

`icm` is an **agent-native utility in the RFC-0011 catalog (§4.10)**, not a
standalone binary (§9.2). It lives at `userland/icm/icm.c`, is written in C11 per
the Anunix language policy, and is a thin renderer over the object-store and
metadata APIs declared in `anx/icm.h`. Phase 1 is read-only except for `tag`
and `published`, which write `user_meta` only.

### 6.1 Subcommands (Phase 1)

```
icm tag <oid> --domain D[,D] --kind K --authority A --status S [--stack ..] [--entry ..]
        Set the anno.icm.* metadata on an existing State Object.

icm catalog [--domain D]
        Render the catalog view, optionally filtered to one domain tag.

icm graph [<oid>]
        Render the provenance-derived dependency graph (whole store, or rooted
        at one artifact).

icm show <oid>
        Render one artifact's ICM annotations + a one-hop provenance summary.

icm tag <oid> --published <release-uri>
        Mark a sealed artifact as a published release (anno.icm.published).

icm published
        List artifacts that carry an anno.icm.published marker.
```

### 6.2 Dependencies on Existing APIs

| Operation                | API (existing)                                     |
|--------------------------|----------------------------------------------------|
| Enumerate artifacts      | `anx_objstore_iterate()` (RFC-0002 §12)            |
| Read annotations         | `anx_meta_get()` (anx/meta.h)                       |
| Write annotations (`tag`)| `anx_meta_set_str()` / `anx_meta_set_i64()`         |
| Walk dependencies        | provenance ancestry/descendant queries (RFC-0002 §8) |
| Resolve role per stage   | Workflow Object node inputs (RFC-0018)             |

The tool introduces **no kernel changes**: every primitive it needs already
exists. This is the point of the RFC — Anunix's substrate already carries ICM.

### 6.3 Phasing

- **Phase 1 (this RFC):** `catalog` / `graph` / `show` / `published` views and
  the `tag` writer (including `--published`), operating on the object store.
  Host-testable.
- **Phase 2:** role-scoped context from live Workflow Objects (RFC-0018), and
  capability-gated views (RFC-0007).
- **Phase 3:** Interface Plane (RFC-0012) canvas rendering of the catalog and
  dependency graph as interactive surfaces.

---

## 7. Relationship to Other RFCs

- **RFC-0002 (State Objects):** supplies identity, metadata, provenance, and
  policy. ICM is a reading of these. RFC-0027 reserves the `anno.icm.*` key
  prefix under §7.4.
- **RFC-0018 (Workflow Objects):** supplies the stage graph and per-node input
  declarations from which *role* is read. RFC-0027 does not duplicate workflow
  structure; it annotates and surfaces it.
- **RFC-0007 (Capabilities):** supplies the per-consumer scope that, with the
  Workflow node, defines local role and enforces `write_policy`.
- **RFC-0003 (Execution Cells):** stages are cells; "consumed things are
  declared" is the cell input contract.
- **RFC-0011 (Agent-Native Utilities):** hosts the `icm` tool in its utility
  catalog (§4.10) and ships it in Wave 2. RFC-0027 supplies the model and the
  `anx/icm.h` API; RFC-0011 is the tool's userland home.

---

## 8. Security Considerations

- `icm tag` writes only `user_meta`; it cannot alter identity, provenance,
  or access policy (RFC-0002 §4.5.1 forbids application writes to system
  metadata). Mis-tagging is therefore a labeling error, never a policy bypass.
- Catalog and graph views inherit object access policy: a caller sees only the
  artifacts and provenance its capabilities permit (RFC-0007). The ICM catalog
  is not a side channel around access control.
- Because role lives with the consumer and authority is kernel-enforced, the
  POSIX failure mode "read-only honored by convention" does not exist here.

---

## 9. Resolved Decisions

The questions raised in the first draft are resolved as follows. Where a
filesystem habit and an ICM principle conflicted, the ICM principle won.

1. **Domains are multi-membership tags, not namespaces.** `anno.icm.domain` is a
   tag *list* (§4.1), not a single namespace placement. ICM's core rule is that
   one artifact serves many contexts at once; domains are one expression of that.
   A namespace forces a 1-to-1 "where does this live" choice, which is exactly
   the folder-placement constraint ICM exists to escape. Tags model the n-to-m
   relation between artifacts and domains directly: an artifact can be `web` *and*
   `product` *and* `ai-agents` simultaneously, with no primary. (RFC-0002 §6.2
   namespaces remain available for isolation and bulk policy where a hard 1-to-1
   boundary is genuinely wanted — e.g. an `ephemeral` namespace with a blanket
   TTL — but they are orthogonal to ICM classification, not its mechanism.)
2. **The `icm` tool folds into RFC-0011.** It is an agent-native utility — it
   exists only because the system has first-class provenance and object metadata
   — so it belongs in the RFC-0011 utility catalog (§4.10) alongside
   `objls`/`objprov`, not as a standalone binary. This RFC supplies the model
   and the `anx/icm.h` API; RFC-0011 is its userland home and ships it in Wave 2.
3. **A published artifact carries an explicit `anno.icm.published` marker.**
   Sealing + `oid@version` (RFC-0002) is the *mechanism* of immutability, but it
   is not *discoverable* as a release: a sealed object looks like any other
   sealed object. ICM distinguishes "published things are versioned" as a
   first-class lifecycle state, so a published release is tagged
   `anno.icm.published = <release URI>` (e.g. `anx:pkg/foo@1.2.0`). This makes
   "show me the published releases" a metadata query (`anx_icm_count` /
   `icm published`) rather than an inference over seal state, and lets consumers
   pin to a release marker without resolving raw oids. See §4.1.
