# RFC-0026: Clustered World Graph Runtime

| Field      | Value                                                                  |
|------------|------------------------------------------------------------------------|
| RFC        | 0026                                                                   |
| Title      | Clustered World Graph Runtime                                          |
| Author     | Adam Pippert                                                           |
| Status     | Draft                                                                  |
| Created    | 2026-06-05                                                             |
| Updated    | 2026-06-05                                                             |
| Depends On | RFC-0002, RFC-0003, RFC-0005, RFC-0006, RFC-0020                       |
| Blocks     | —                                                                      |

---

## Executive Summary

Anunix already has the bones of a world-system: State Objects with provenance
and policy (RFC-0002), Execution Cells (RFC-0003), the routing and scheduler
planes (RFC-0005), a Network Plane with node identity and trust zones
(RFC-0006), and the iterative belief-action loop with JEPA/EBM hooks (RFC-0020).
What is missing is the **connective tissue** that lets many models, processes,
and nodes cooperate on one evolving model of the world without trampling each
other.

This RFC defines the **world graph runtime**: a first-class graph of world
state, a patch protocol by which models propose changes instead of mutating
state, a speculative branch manager that forks the graph for evaluation, a
commit gate that admits or rejects patches, a provider registry that gives each
model a declared semantic authority, and a trust-zoned replication gate that
carries committed state to peers.

The governing rule is simple: **models observe and propose; the kernel
validates and commits.** No model writes world state directly. Every committed
node, edge, constraint, and prediction is a State Object, so provenance, policy,
and replication apply to the world graph exactly as they do to every other
artifact in the system.

```
observe -> propose patch -> validate -> commit / reject -> provenance -> replicate
```

## Motivation

The natural next step for Anunix is not more model demos; it is the substrate
that makes a *clustered* world-system possible. Without it, every new model
(physics, chemistry, spatial reasoning) becomes a hard-coded special case that
mutates shared state and races every other model. With it, a model is just a
**provider** with a manifest, scheduled like a process but reasoned about by its
semantic authority, and its claims flow through a uniform commit gate.

The critical path is: **world graph → patch protocol → branch manager →
provider registry → trust-zoned replication.** Downstream model integrations
(LNN/HNN/MuJoCo and friends) are plugins that ride on this substrate; they are
explicitly *out of scope* here and must not be built first.

## Object Model (Section 3)

The runtime adds eight canonical object types to RFC-0002's `enum
anx_object_type`, making the world graph native rather than implied by memory
metadata:

| Type                  | Meaning                                            |
|-----------------------|----------------------------------------------------|
| `ANX_OBJ_WORLD_NODE`  | A canonical node in the world graph                |
| `ANX_OBJ_WORLD_EDGE`  | A relation between two nodes                        |
| `ANX_OBJ_WORLD_PATCH` | A committed patch, kept as an audit record         |
| `ANX_OBJ_WORLD_BRANCH`| A speculative branch marker                        |
| `ANX_OBJ_MODEL_CLAIM` | A claim authored by a model                        |
| `ANX_OBJ_CONSTRAINT`  | A constraint attached to a node                    |
| `ANX_OBJ_OBSERVATION` | An observation snapshot that feeds a patch         |
| `ANX_OBJ_PREDICTION`  | A prediction attached to a node                    |

A node's **identity is its oid**. Inside an uncommitted branch a new node holds
a *provisional* oid used only for wiring edges and claims; at commit the node is
materialized as a `WORLD_NODE` State Object and its identity becomes that
object's oid. Edges and claims are retargeted onto the committed oids during the
same merge, so the canonical graph is always keyed by real object identity.

## Patch Protocol (Section 4)

A model never mutates world state. It authors a **patch** — an ordered set of
operations — and proposes it onto a branch. The supported operations are:

| Operation            | Effect                                              |
|----------------------|-----------------------------------------------------|
| `add_node`           | Introduce a node in a world slice (domain)          |
| `add_edge`           | Relate two nodes                                     |
| `update_property`    | Set a key/value on a node                           |
| `attach_constraint`  | Attach a constraint expression to a node            |
| `attach_prediction`  | Attach a prediction (with fixed-point confidence)   |
| `mark_conflict`      | Flag a node as conflicting                          |
| `resolve_conflict`   | Clear a node's conflict flag                        |

Operations reference nodes either by a **patch-local index** (for nodes the same
patch adds) or by **oid** (for existing canonical nodes). Forward references are
rejected: a node must be added before it is referenced. Proposing a patch
applies it to the branch snapshot — so providers can read the speculative result
— while leaving the canonical graph untouched until commit.

## Speculative Branch Manager (Section 6)

The branch manager forks the canonical graph into a **speculative branch** at a
specific version. Patches mutate only the branch. Evaluation (by plugins or
models) happens against the branch. A **commit gate** then decides whether the
branch merges back:

```
canonical graph
  └─ fork ─▶ speculative branch
              └─ propose patch(es)
                  └─ commit gate ─▶ commit ─▶ canonical update
                                 └─ reject ─▶ branch left intact for revision
```

Concurrency is **optimistic**: a branch records the canonical version it forked
from, and commit refuses a branch whose base has since advanced
(`ANX_EBUSY`) — the branch must be re-forked (rebased). This gives last-writer-
wins per committed branch with no lost updates, and it is the seed of multi-node
conflict handling.

## Provider Registry and Commit Gate (Section 5)

Every model or process that touches the world graph registers a **manifest**:

```
id                 = "physics.lnn"
domain             = "physics"
reads              = "world.physical,world.spatial"
writes             = "world.physical,world.spatial"
can_execute_actions = false
network_zone       = "physics.zone"
```

This lets Anunix schedule providers like processes while reasoning about their
**semantic authority**. The commit gate runs in three stages, in order:

1. **Staleness** — reject a branch whose canonical base advanced.
2. **Authority** — reject any patch whose provider is unregistered or whose
   manifest does not grant write authority over a slice the patch creates.
3. **Validators** — run every registered validator against every staged patch.
   A validator (for example, a constraint checker or JEPA scorer) returns a
   rejection to veto the merge, with a human-readable reason.

Only when all three pass does the branch merge. On merge, each new node, edge,
constraint, and prediction is materialized as a State Object with provenance
naming the authoring provider, and the canonical version is bumped.

## Trust-Zoned Replication (Section 7)

Local writes are authoritative; replication is policy-bound, matching RFC-0006's
local-first direction (remote reads are advisory unless local state is absent;
writes are local-first with optional async replication). The replication gate
resolves a peer through the Network Plane node registry and refuses untrusted
peers (`ANX_EPERM`) and unknown peers (`ANX_ENOENT`); on success it records a
`PROV_MIGRATED` provenance event naming the peer.

The gate decides *whether* a committed object may cross to a peer and leaves the
audit trail. The byte transfer itself belongs to the network data plane
(RFC-0006 / RFC-0015); this runtime defines *what* crosses it, not the wire.

## Acceptance Criteria

Two Anunix nodes can:

1. establish signed peer identity (RFC-0006 node registry);
2. advertise model capabilities (provider manifests);
3. fork a world graph branch;
4. accept a model-generated patch;
5. validate it through a commit gate;
6. replicate the committed State Object to a trusted peer;
7. preserve provenance and policy throughout.

Phase 1 (this RFC's implementation) covers 2–5 and 7 in-kernel and host-tested,
plus the policy decision and provenance for 6. Phase 2 wires the signed wire
protocol for 1 and the byte transfer for 6 onto the Network Plane.

## Scope and Non-Goals

**In scope (Phase 1):** the object model, patch protocol, branch manager,
provider/commit-gate registry, and the trust-zoned replication gate, all
implemented in `kernel/core/worldgraph/` with host-native tests.

**Out of scope:** LNN/HNN/MuJoCo or any specific physics model — those are
providers that ride on this substrate and must not become special cases baked
into the runtime; the cross-node signed transport and overlay sync, which extend
RFC-0006; and re-materialization of mutated (as opposed to newly added) canonical
nodes, whose changes are captured in the `WORLD_PATCH` audit record for now.

## Implementation Notes

The Phase 1 caps (`ANX_WORLD_MAX_NODES`, etc. in `anx/worldgraph.h`) are
deliberately conservative so that the canonical graph and every branch snapshot
remain single, page-cheap allocations on the shared kernel heap. They are
`#define`s and are expected to grow once the runtime is backed by a dedicated
arena. No floating point is used; prediction confidence is a fixed-point value
in `[0, 1000]`.
