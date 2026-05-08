# Anunix 2026.5.8 Release Notes

Milestone: **Topological organization of State Objects** — Anunix now has
a deterministic, content-derived coordinate system (UOR) for every object,
woven into the disk store's locality index. Editor renamed to **amacs**.

## Highlights

- **UOR — Universal Object Reference** (`kernel/core/uor/`,
  `kernel/include/anx/uor.h`). State Objects gain a topological coordinate
  alongside their OID and content hash. Projection is a pure function of
  the canonical manifest `(oid, version, content_hash, type, parents,
  schema)`; mutating content yields a new projection while OID stays
  stable. The UOR-derived `boundary_key` is the engine that the disk
  store's sorted index uses for locality-ordered range scans.
- **Topology rebuild** — `anx_uor_rebuild_topology_index()` walks the
  in-memory object store, recomputes every projection, and reattaches
  metadata. Non-destructive: payloads, OIDs, and provenance are
  untouched. Topology epoch bumps so callers can invalidate region
  caches deterministically.
- **`anunixmacs` → `amacs`** — directory, header, RFC, test name, and
  the user dotfile (`~/.amacs.el`) all renamed. Public `anx_ed_*` API is
  unchanged.
- **RFC-0002 §14** — new section *UOR Projection and Topological
  Identity* documents the design boundary, manifest schema, projection
  formula, lifecycle hooks, and forward-looking phases (memory plane,
  routing, network plane, verification).
- **Anunix-tools UOR helpers** — Python reference oracle, an
  HTTP-API-driven `anx-uor-inspect` script, and a region-scan helper
  for external developers cross-validating kernel-emitted UOR refs.

## What UOR is and is not

UOR is a **secondary** identity surface. The OID, content hash,
provenance graph, access policy, and memory-plane tier model continue
to be authoritative. UOR adds:

```
oid           which object is this?
content_hash  is the payload intact?
uor_address   where does this object live in the universal topology?
```

Projection is bound to `(oid, version, content_hash)`, not just `oid`,
so topological coordinates track content faithfully across mutation.
UOR coordinates do **not** recreate missing payloads, replace
embeddings, or bypass access policy.

## On-disk shape

`anx_disk_write_obj_bk` already accepted a caller-supplied
`boundary_key`. The State Object lifecycle now feeds the UOR-derived
key in automatically:

```text
canonical_manifest
  → SHA-256
  → 32-byte uor.address
  → boundary_key = (object_type<<56) | BE56(address[1..7])
  → region [boundary_key & ~MASK, ... | MASK]
  → anx_disk_write_obj_bk(...)
```

Range scan over `[type<<56, type<<56 | (1<<56)-1]` returns every
on-disk object of a given type in deterministic order; finer scans by
region address one locality cluster at a time.

## New API

```c
/* anx/uor.h */
int  anx_uor_build_manifest(const struct anx_state_object *obj,
                            struct anx_uor_manifest *out);
int  anx_uor_project_manifest(const struct anx_uor_manifest *m,
                              uint16_t object_type,
                              struct anx_uor_ref *out);
int  anx_uor_project_object(const struct anx_state_object *obj,
                            struct anx_uor_ref *out);
int  anx_uor_attach_metadata(struct anx_state_object *obj,
                             const struct anx_uor_ref *ref);
int  anx_uor_rebuild_topology_index(uint32_t *count_out);
uint64_t anx_uor_topology_epoch(void);
uint64_t anx_uor_boundary_key(const struct anx_uor_ref *ref);
```

## New metadata keys (system_meta)

```
uor.address              hex string, 64 chars
uor.boundary_key         int64 (sortable)
uor.region.lo / .hi      int64
uor.locality_metric      int64 (reserved)
uor.manifest_version     int64
uor.topology_epoch       int64 (bumped on rebuild)
```

## Lifecycle hooks

UOR projection runs on:

- `anx_so_create()` — once `(oid, version, content_hash)` are set.
- `anx_so_seal()` — re-projects under the now-immutable content hash.
- `anx_uor_rebuild_topology_index()` — explicit rebuild, bumps epoch.

It does **not** run on read.

## Tests

`tests/test_uor.c` covers manifest determinism, content-hash
sensitivity, version sensitivity, type bucketing, region bounds,
metadata attachment on create and seal, topology rebuild, and disk
range scans driven by UOR-derived boundary keys.

```
=== Results: 50 passed, 0 failed ===
```

## amacs rename

The `anunixmacs` editor is now `amacs`:

| Before                                 | After                              |
|----------------------------------------|------------------------------------|
| `kernel/core/apps/anunixmacs/`         | `kernel/core/apps/amacs/`          |
| `kernel/include/anx/anunixmacs.h`      | `kernel/include/anx/amacs.h`       |
| `tests/test_anunixmacs.c`              | `tests/test_amacs.c`               |
| `docs/rfcs/RFC-0023-anunixmacs-editor` | `docs/rfcs/RFC-0023-amacs-editor`  |
| `~/.anunixmacs.el`                     | `~/.amacs.el`                      |
| `anx:app/anunixmacs`                   | `anx:app/amacs`                    |

The user-facing public API (`anx_ed_*`) is unchanged. The eLISP surface
is unchanged.

## Breaking changes

- `~/.anunixmacs.el` is no longer loaded; rename to `~/.amacs.el`.
- The workflow template `anx:app/anunixmacs` is now `anx:app/amacs`.

## Forward-looking

The UOR layer shipped here is the minimum viable integration: projection
+ boundary-key path + topology rebuild. Phases queued for subsequent
releases (per RFC-0002 §14.8):

- **Memory plane** integration: region-aware admission, prefetch, decay.
- **Routing** locality scoring against UOR neighborhoods.
- **Network plane** federated `uor://` lookup, with policy enforcement.
- **Verification certificates**: `verification.uor_certificate` State
  Objects bind a projection to an external trust anchor.
