# RFC-0018: Expert Composition and Multi-Model Hosting

| Field      | Value                                                                  |
|------------|------------------------------------------------------------------------|
| RFC        | 0018                                                                   |
| Title      | Expert Composition and Multi-Model Hosting                             |
| Author     | Adam Pippert                                                           |
| Status     | Draft                                                                  |
| Created    | 2026-05-05                                                             |
| Updated    | 2026-05-05                                                             |
| Depends On | RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0006, RFC-0008, RFC-0013   |

---

## Executive Summary

RFC-0013 made a single model a first-class object: a namespace of tensor objects with shape and dtype the kernel understands, plus a place for LoRA adapters as small delta tensors. That is enough to fine-tune one model and govern its weights. It is not enough to express the dominant pattern emerging in modular AI systems — composing several **independently-trained** models as cooperating experts under a learned router (BAR / "Train Separately, Merge Together", arXiv:2604.18473; classical Mixture-of-Experts; mixture-of-adapters; safety/capability splits).

This is a different architectural problem. Single-model manipulation asks "how do I version and govern the layers of one model." Expert composition asks "how do I host **N** independently-versioned models in the same address space, route inputs across them, and manage memory residency when N grows past what a single device can hold."

RFC-0018 introduces:

1. **Composition Namespace** — a new State Object layout that binds multiple model namespaces (RFC-0013) into a single addressable composite, with its own provenance and version history.
2. **Router Object** — a small tensor-backed gating network treated as a first-class object that can be trained, versioned, and rolled back independently of the experts it routes to.
3. **Expert Residency Model** — a formal vocabulary (`resident`, `staged`, `streamed`, `remote`) for *how* each expert is held, plus a per-composition residency budget the kernel enforces.
4. **Multi-Model Memory Plane** — extensions to RFC-0004 so several models can co-reside, share a backbone, and be paged at tensor granularity rather than as monolithic blobs.
5. **Federated Experts** — extensions to RFC-0006 so an expert can live on another node and be invoked through the network plane while preserving the same composition semantics.
6. **Per-Expert Governance** — credential bindings (RFC-0008) at the expert level, so updating, quarantining, or rolling back one expert never requires authority over the others.

The kernel does not implement gating math. It provides the object model, the residency scheduler, and the routing fabric that make multi-model compositions governable, page-aware, and identical in API from a phone to a cluster.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert
**Depends on:** RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0006, RFC-0008, RFC-0013
**Blocks:** —

---

## 2. Problem Statement

### 2.1 A Model Namespace Is Not Enough

RFC-0013 organizes one model. Its `adapters/` convention covers LoRA and similar parameter-efficient deltas — *transformations of a single base*. It cannot express:

- Two or more **fully independent** models, each with its own training pipeline, dataset provenance, and version history, that must cooperate at inference.
- A learned **router** that gates inputs across those models and is itself trainable.
- A **shared backbone** where several experts reuse the same embedding and head tensors but diverge in the middle layers, without any of them owning the shared layers.

A single model namespace has one manifest, one lineage chain, one credential scope. Forcing a multi-model composition into that shape collapses information the system needs to govern updates and rollbacks correctly.

### 2.2 Memory Residency Is the Hard Part

The naïve interpretation of "host multiple models" is "load them all into memory." On a cluster with sufficient HBM that may be feasible. Everywhere else it is not. A composition of four 8B-parameter experts in bf16 is 64 GB of weights before KV cache. A phone hosting a safety expert plus two task experts cannot keep all weights resident.

The system needs explicit vocabulary for residency, because the choice between "keep this expert pinned in L0" and "fetch its tensors per request from L2" has order-of-magnitude latency consequences and must be a first-class policy decision rather than an emergent property of the page cache. RFC-0004 today places tensors based on access patterns; it has no notion that several tensors *belong to the same expert* and should be admitted or evicted as a group.

### 2.3 Routing Plane Routes Engines, Not Models

RFC-0005 routes a tensor operation to the engine best suited to its dtype, shape, and accelerator. That layer is correct and needed. But it does not answer the question composition adds: *which expert model contributes to this forward pass at all.* That decision is made by a learned gating network whose output drives engine routing for several different parameter sets. The two routing layers must compose without conflating.

### 2.4 Governance Must Be Per-Expert

The principal claim of BAR-style composition is that experts can be updated independently with linear cost. That guarantee evaporates if the system requires monolithic credentials over the whole composition to retrain one expert, or if rollback restores all experts together. The credential and provenance model has to recognize each expert as its own governable unit while still presenting one logical model to callers.

### 2.5 What Existing RFCs Cannot Express

- **RFC-0013** — one namespace, one manifest, one lineage. No cross-model references. No router as object.
- **RFC-0004** — placement is per-tensor based on access pattern. No notion of "this group of tensors is an expert; admit or evict together."
- **RFC-0005** — engine selection per op. No expert-level routing layer above engine routing.
- **RFC-0006** — cells can call remote cells. No notion of an expert hosted off-node that participates in a local forward pass.
- **RFC-0008** — credentials gate cells and objects. No per-expert scope within a composite model.

---

## 3. Goals

### 3.1 Primary Goals

1. **Compositions as first-class objects.** A composition is a namespace that references multiple model namespaces (RFC-0013) and a router, with its own manifest, version history, and provenance.
2. **Independent expert lifecycle.** Any expert can be trained, replaced, rolled back, or quarantined without touching the others or the router.
3. **Independent router lifecycle.** The router is a separately versioned object. A new router can be trained over the same expert set; a new expert can be added by retraining only the router.
4. **Explicit residency.** Every expert has a declared residency mode. The kernel enforces it within a per-composition budget.
5. **Per-tensor streaming.** When an expert is `streamed`, its weights are loaded at tensor granularity using existing RFC-0013 BRIN block access — never as a monolithic blob.
6. **Shared backbones.** A composition may declare shared layers drawn from a base model; those tensors load once and are referenced by all experts that reuse them.
7. **Federated experts.** An expert may live on another node and be invoked transparently through the network plane.
8. **Per-expert credentialing.** Credential bindings can scope writes, reads, and invocations to specific experts within a composition.
9. **Phone to cluster.** The same composition manifest works whether all experts are remote, all are streamed, or all are resident — only the residency declarations change.

### 3.2 Non-Goals

- **Implementing gating math in the kernel.** The router is a small neural network that runs on an engine like any other tensor op.
- **Replacing training frameworks.** Training experts and routers is userland work. The kernel records provenance and gates writes; it does not run optimizers.
- **Defining a specific MoE algorithm.** Top-k softmax, sigmoid mixture, layer-wise gating, request-level routing — all expressible as composition policies. The kernel does not pick one.
- **Auto-discovering compositions.** A composition is created explicitly. The kernel will not infer a composition from observed model usage.

---

## 4. Core Definitions

### 4.1 Composition Namespace

A **Composition Namespace** is a new State Object layout that binds a router and a set of experts:

```
compositions:/<name>/
  manifest                 ANX_OBJ_STRUCTURED_DATA
    {
      "name": "general-bar-4x8b",
      "version": 3,
      "router_oid": "...",
      "experts": [
        { "name": "math",   "model_ref": "models:/llama-3-math-8b@7",   "residency": "staged"   },
        { "name": "code",   "model_ref": "models:/llama-3-code-8b@4",   "residency": "staged"   },
        { "name": "tool",   "model_ref": "models:/llama-3-tool-8b@2",   "residency": "streamed" },
        { "name": "safety", "model_ref": "models:/llama-3-safety-8b@9", "residency": "resident" }
      ],
      "shared_layers": [
        { "name": "embed_tokens", "source": "models:/llama-3-base@1/layers/embed_tokens" },
        { "name": "lm_head",      "source": "models:/llama-3-base@1/layers/lm_head"      }
      ],
      "policy": {
        "granularity": "token",
        "top_k": 2,
        "gating": "softmax",
        "temperature": 1.0,
        "load_balance_alpha": 0.01
      },
      "budget": {
        "max_resident_bytes": 17179869184,
        "max_staged_bytes":   34359738368,
        "max_resident_experts": 2,
        "prefetch_on_router_prediction": true
      }
    }

  router/
    gate                   ANX_OBJ_TENSOR        (router weights)
    arch                   ANX_OBJ_STRUCTURED_DATA  (router architecture)
    meta                   ANX_OBJ_STRUCTURED_DATA  (training provenance)

  experts/
    math/                  ANX_OBJ_MODEL_REF -> models:/llama-3-math-8b@7
    code/                  ANX_OBJ_MODEL_REF -> models:/llama-3-code-8b@4
    tool/                  ANX_OBJ_MODEL_REF -> models:/llama-3-tool-8b@2
    safety/                ANX_OBJ_MODEL_REF -> models:/llama-3-safety-8b@9

  shared/
    embed_tokens           ANX_OBJ_TENSOR_REF -> models:/llama-3-base@1/layers/embed_tokens
    lm_head                ANX_OBJ_TENSOR_REF -> models:/llama-3-base@1/layers/lm_head

  history/                 (composition-level events: expert swaps, router retrains, rollbacks)
```

The composition does **not** own its experts' tensors. It holds references. Each referenced model namespace remains an independent RFC-0013 object with its own version history. The composition's history records how it was assembled — which expert versions are active, when the router was retrained — independently of what happens inside any one expert.

### 4.2 Reference Object Types

Two new object types are introduced to make cross-namespace references first-class:

```c
enum anx_object_type {
    /* ... existing types ... */
    ANX_OBJ_COMPOSITION,   /* composition manifest */
    ANX_OBJ_MODEL_REF,     /* pinned reference to a model namespace at a specific version */
    ANX_OBJ_TENSOR_REF,    /* pinned reference to a tensor object at a specific version */
};
```

References are **pinned to versions**. A composition manifest names `models:/llama-3-math-8b@7`, not `models:/llama-3-math-8b`. New versions of the math model do not silently propagate into the composition — the composition's manifest must be updated to advance the pin. This makes expert upgrades a deliberate, recorded event.

The kernel tracks reference counts across namespaces. A model with live composition references cannot be garbage-collected, and version-7 weights of `llama-3-math-8b` are retained while `general-bar-4x8b@3` references them, even if newer versions exist.

### 4.3 Expert Residency Modes

```c
enum anx_expert_residency {
    ANX_RESIDENCY_RESIDENT,  /* pinned in L0/L1; activation latency: µs */
    ANX_RESIDENCY_STAGED,    /* warm in L1/L2; admitted to L0 on demand; ms */
    ANX_RESIDENCY_STREAMED,  /* cold; tensors loaded individually per request; 100s of ms */
    ANX_RESIDENCY_REMOTE,    /* lives on another node; invoked over network plane */
};
```

| Mode | Memory Tier | Granularity of Load | Activation Latency | Use |
|------|-------------|---------------------|--------------------|---------|
| `resident` | L0/L1 pinned | All tensors at once | µs | Hot experts on every forward pass (safety, default) |
| `staged` | L1/L2 cached | All tensors, paged into L0 on first activation | ms | Likely-active experts on warm hardware |
| `streamed` | L2+ | Per-tensor BRIN block loads | 100s of ms | Long-tail experts; phone-class devices |
| `remote` | Another node | Forward pass executed remotely | 10s–100s of ms | Federated capacity; offload of large experts |

Residency is a **policy declaration**, not a permanent placement. The composition may upgrade or downgrade an expert's residency at runtime within the budget. The kernel guarantees that a `resident` expert's pages cannot be evicted below L1 while the composition is active; `staged` and `streamed` follow standard Memory Plane (RFC-0004) eviction with hints.

### 4.4 Composition Manifest Structure

```c
struct anx_composition_expert {
    char        name[64];                   /* expert name within composition */
    anx_oid_t   model_ref;                  /* pinned ref to model namespace */
    uint32_t    pinned_version;             /* version of referenced model */
    enum anx_expert_residency residency;
    uint64_t    expected_bytes;             /* sum of expert tensor bytes */
    bool        prefetch_eligible;          /* router may prefetch this expert */
};

struct anx_composition_shared_layer {
    char        name[128];                  /* layer name within composition */
    anx_oid_t   source_tensor;              /* pinned tensor ref */
};

enum anx_router_granularity {
    ANX_ROUTE_REQUEST,                      /* one expert per request */
    ANX_ROUTE_TOKEN,                        /* one or more experts per token */
    ANX_ROUTE_LAYER,                        /* one or more experts per layer (full MoE) */
};

struct anx_composition_policy {
    enum anx_router_granularity granularity;
    uint32_t    top_k;                      /* experts activated per gating step */
    char        gating[32];                 /* "softmax", "sigmoid", "topk_renorm" */
    float       temperature;
    float       load_balance_alpha;         /* MoE auxiliary loss weight */
};

struct anx_residency_budget {
    uint64_t    max_resident_bytes;
    uint64_t    max_staged_bytes;
    uint32_t    max_resident_experts;
    bool        prefetch_on_router_prediction;
};

struct anx_composition_manifest {
    char        name[128];
    uint32_t    version;
    anx_oid_t   router_oid;
    struct anx_composition_expert        experts[32];
    uint32_t    expert_count;
    struct anx_composition_shared_layer  shared[16];
    uint32_t    shared_count;
    struct anx_composition_policy        policy;
    struct anx_residency_budget          budget;
};
```

### 4.5 Router Object

A **Router Object** is a small typed model — usually a linear or MLP gating network producing a distribution over experts. It is itself a tensor-backed object with its own manifest:

```c
struct anx_router_meta {
    char        name[64];
    uint32_t    input_dim;                  /* router input feature dimension */
    uint32_t    expert_count;               /* number of output logits */
    char        architecture[32];           /* "linear", "mlp", "hash", "learned" */
    anx_oid_t   weights_tensor;             /* router parameters */
    anx_oid_t   training_data_ref;          /* what the router was trained on */
    uint64_t    training_steps;
    uint32_t    parent_router_version;      /* prior router; 0 if first */
};
```

The router is a ordinary RFC-0013 model in miniature: a manifest, a tensor (or a few), a training history. It is governed by the same credential and provenance machinery as any other model. Crucially, it is **decoupled from the experts**: replacing the router is a single object update, not a retraining of the composition's parameters.

### 4.6 Reference Pinning and Garbage Collection

Cross-namespace references introduce a new GC obligation: a model version with live composition references is **retained**. Concretely:

- Each `ANX_OBJ_MODEL_REF` and `ANX_OBJ_TENSOR_REF` increments a refcount on its target version.
- The object store will not delete a referenced version until the refcount drops to zero.
- Advancing a composition's pin (e.g., `experts/math` from `@7` to `@8`) decrements the refcount on `@7`. If no other composition references `@7`, the store may release it according to its retention policy.

This prevents the dangling-reference class of bugs entirely while keeping each model namespace authoritative over its own version history.

---

## 5. Design Principles

### 5.1 Compositions Reference; Models Own

A composition does not store expert weights. It stores references to model namespaces that own those weights. A model namespace knows nothing about the compositions that reference it. This is the only way per-expert independent updates compose cleanly: the math model has a single linear history regardless of how many compositions consume it.

### 5.2 Routing Has Two Layers

**Expert routing** (the neural router) decides which experts contribute to each forward step. **Engine routing** (RFC-0005) decides which engine executes each per-expert tensor op. The two layers are orthogonal and must not conflate. The router is data; engine routing is policy.

### 5.3 Residency Is an Object Property, Not a Cache Outcome

In a classical OS, "what's in memory" is the result of a thousand cache decisions. In Anunix, a composition declares — per expert — what residency it requires, and the kernel enforces that declaration within budget. Caching still happens, but residency is a stated contract, not an inferred state.

### 5.4 Streamed Experts Are Just BRIN Reads

When an expert is `streamed`, the kernel does not invent a new loading path. It uses RFC-0013's existing BRIN block-level access on the expert's tensors. The unit of load is a tensor block, not a model file. This means a `streamed` expert's first-token latency is bounded by the active layers of the forward pass, not the full weight set.

### 5.5 The Router Is a Tiny Model, Governed Like Any Other

There is no special "router credential" or "router runtime." The router is an `ANX_OBJ_MODEL_REF` to a small model namespace. It trains, versions, rolls back, and audits through the standard RFC-0013 path. The only specialization is its declared role in a composition manifest.

### 5.6 Shared Layers Reduce the Multi-Model Cost

Most expert compositions sharing a base architecture also share input/output projections (token embeddings, output head). Declaring those as `shared/` references means N experts of size B with delta D cost `B + N·D` rather than `N·(B+D)`. This is what makes phone-class compositions tractable.

### 5.7 Per-Expert Rollback Is the Headline Property

The whole point of train-separately-merge-together is that one expert going bad does not poison the others. The kernel must make per-expert rollback a one-call operation that restores the composition's pin without touching any other expert or the router. Section 8.3 specifies this.

---

## 6. Kernel Operations

### 6.1 Composition Lifecycle

```c
/* Create a composition namespace from a manifest.
 * Validates that all expert refs and shared-layer refs resolve. */
int anx_composition_create(const char *ns_path,
                           const struct anx_composition_manifest *manifest,
                           struct anx_state_object **out);

/* Get the current manifest of a composition */
int anx_composition_manifest_get(const anx_oid_t *composition,
                                 struct anx_composition_manifest *out);

/* Update the router pin (router-only retraining produced a new version) */
int anx_composition_set_router(const anx_oid_t *composition,
                               const anx_oid_t *new_router_oid);

/* Add an expert to an existing composition. Forces router retraining
 * before the new expert may participate; the kernel marks the
 * composition as "router-stale" until anx_composition_set_router runs. */
int anx_composition_add_expert(const anx_oid_t *composition,
                               const struct anx_composition_expert *expert);

/* Remove an expert. Refcount on the referenced model is decremented. */
int anx_composition_remove_expert(const anx_oid_t *composition,
                                  const char *expert_name);

/* Advance an expert pin to a new version of its underlying model */
int anx_composition_advance_expert(const anx_oid_t *composition,
                                   const char *expert_name,
                                   uint32_t target_version);
```

### 6.2 Residency Control

```c
/* Change an expert's residency mode. Kernel schedules load/evict
 * within budget; returns -ANX_EBUDGET if the change would exceed it. */
int anx_composition_set_residency(const anx_oid_t *composition,
                                  const char *expert_name,
                                  enum anx_expert_residency target);

/* Update the composition's residency budget */
int anx_composition_set_budget(const anx_oid_t *composition,
                               const struct anx_residency_budget *budget);

/* Report current residency state for all experts */
struct anx_expert_residency_state {
    char     name[64];
    enum anx_expert_residency declared;
    enum anx_expert_residency effective;   /* may differ under budget pressure */
    uint64_t bytes_held_l0;
    uint64_t bytes_held_l1_l2;
    uint64_t last_activation_ns;
    uint64_t activation_count;
};

int anx_composition_residency_report(const anx_oid_t *composition,
                                     struct anx_expert_residency_state *out,
                                     uint32_t max_experts, uint32_t *count);
```

### 6.3 Invocation

```c
struct anx_inference_intent {
    anx_oid_t   input_tensor;               /* model input (token ids, etc.) */
    uint32_t    max_new_tokens;
    bool        record_routing_provenance;  /* default true */
    char        invocation_credential[64];  /* RFC-0008 binding name */
};

struct anx_inference_result {
    anx_oid_t   output_tensor;
    anx_oid_t   provenance_object;          /* per-step routing decisions */
};

/* Invoke a composition. The kernel:
 *  1. Authorizes via invocation_credential
 *  2. Triggers the router (engine call) on the input
 *  3. Resolves top-k experts per gating step
 *  4. Schedules per-expert forward passes through the routing plane
 *  5. Reduces outputs per the composition policy
 *  6. Records routing provenance if requested
 */
int anx_composition_invoke(const anx_oid_t *composition,
                           const struct anx_inference_intent *intent,
                           struct anx_inference_result *out);
```

### 6.4 Per-Expert Rollback

```c
/* Roll one expert's pin back N versions. Other experts and the
 * router are unchanged. The composition's history records the rollback. */
int anx_composition_rollback_expert(const anx_oid_t *composition,
                                    const char *expert_name,
                                    uint32_t versions_back);

/* Quarantine an expert: mark it ineligible for routing without
 * removing it. The router is asked to re-normalize over the remaining
 * experts. Rolled back via clear_quarantine. */
int anx_composition_quarantine_expert(const anx_oid_t *composition,
                                      const char *expert_name);
int anx_composition_clear_quarantine(const anx_oid_t *composition,
                                     const char *expert_name);
```

### 6.5 Reference Tracking

```c
/* List compositions that reference a given model namespace */
int anx_model_referencing_compositions(const anx_oid_t *model_ns,
                                       anx_oid_t *out, uint32_t max,
                                       uint32_t *count);

/* List the experts referenced by a composition (resolves all pins) */
int anx_composition_list_experts(const anx_oid_t *composition,
                                 struct anx_composition_expert *out,
                                 uint32_t max, uint32_t *count);
```

---

## 7. Expert Residency Scheduler

The residency scheduler is the heart of the multi-model story. It runs in the kernel as part of the Memory Control Plane (RFC-0004) and is responsible for honoring per-expert residency declarations under a composition's budget.

### 7.1 Inputs

For each composition the scheduler tracks:

- Declared residency per expert.
- Expert byte size (sum of tensor sizes).
- Last-activation timestamp and activation rate.
- Router prefetch hints (if the router emits a prediction, the scheduler may begin loading the predicted top-k experts before the forward pass requests their tensors).
- Budget (`max_resident_bytes`, `max_staged_bytes`, `max_resident_experts`).

### 7.2 Admission Rules

1. **`resident` is contractual.** If the sum of declared-resident expert bytes exceeds `max_resident_bytes`, `anx_composition_create` fails. Resident experts are pinned at composition activation and remain pinned until residency is changed or the composition is destroyed.
2. **`staged` is best-effort within a soft cap.** The scheduler keeps staged-expert tensors in L1/L2 and admits them to L0 on activation. Under pressure, the least-recently-activated staged expert is the first to drop tensors back to L1.
3. **`streamed` never pins.** Tensors are loaded per request via RFC-0013 BRIN block reads, then released. Cache may retain them briefly subject to L1 policy.
4. **`remote` is invisible to local memory.** No local tier holds the expert's weights. Network plane handles the call.
5. **Effective residency may degrade.** Under sustained pressure, a `staged` expert may behave like `streamed`. The kernel reports the effective residency separately from the declared residency; agents may react to persistent degradation by adjusting the budget or the composition.

### 7.3 Prefetch on Router Prediction

When the policy enables `prefetch_on_router_prediction`, the scheduler observes the router's pre-softmax logits and begins loading the predicted top-k+1 experts before the gating decision is final. This hides expert activation latency for `staged` experts at the cost of occasional wasted loads. The +1 covers the case where the second-place expert is upgraded on close gating margins. This is a kernel-level feature because only the kernel sees both the router output and the page state.

### 7.4 Eviction

Expert eviction is **always at expert granularity**, never per-tensor. When a staged expert must be evicted from L0 to L1, all of its tensors move together. This preserves the property that admitting an expert is a single decision with predictable cost.

---

## 8. Governance and Provenance

### 8.1 Per-Expert Credentials

Credential bindings (RFC-0008) gain expert-name scope:

```
credentials:/builds/math-team/
  scope: write
  target: compositions:/general-bar-4x8b/experts/math
```

A cell holding this binding may advance the math expert's pin or trigger a math-expert retrain that produces a new model version, but cannot touch `experts/code` or the router. The composition's manifest update is itself an authorized event recorded in `history/`.

### 8.2 Router Credential

The router has its own binding:

```
credentials:/builds/router-team/
  scope: write
  target: compositions:/general-bar-4x8b/router
```

Distinct from any expert credential. Retraining the router does not require authority over any expert's weights; conversely, retraining an expert does not authorize router updates.

### 8.3 Per-Expert Rollback

A rollback is the inverse of an advance:

```c
anx_composition_rollback_expert(comp, "math", 1);
```

This:

1. Verifies the calling cell holds write scope on `experts/math`.
2. Reads the composition's `history/` for the prior pin of `experts/math`.
3. Rewrites the manifest's expert entry to that pin.
4. Decrements the refcount on the rolled-out version; increments on the rolled-in one.
5. Marks the composition router-stale only if the policy requires (e.g., when the router was trained against a router-stale invariant). Most rollbacks of small deltas leave the router valid.
6. Appends a rollback event to `history/`.

No other expert is touched. No router update is required by default.

### 8.4 Routing Provenance

When `record_routing_provenance` is set on an invocation, the result includes a provenance object capturing, for each gating step:

- The router version active.
- The top-k expert names and their gating probabilities.
- The pinned model versions of the chosen experts.
- The shared-layer versions used.
- Engine selections per expert tensor op.

This is what allows post-hoc attribution: "the answer came from `math@7` (p=0.78) and `code@4` (p=0.21), routed by `router@12`, with `embed_tokens@1` and `lm_head@1` shared from base." Provenance objects are themselves State Objects with their own retention policy.

---

## 9. Memory Plane Integration (RFC-0004)

Amendments to RFC-0004:

- **New tier policy: expert-pinned pages.** Pages backing tensors of a `resident` expert carry a pin flag; standard eviction skips them.
- **New admission group: composition-bound experts.** Tensors belonging to one expert share an admission group; admission is all-or-nothing.
- **New tier hint: expert prefetch.** When the residency scheduler decides to prefetch a `staged` expert, RFC-0004 receives a hint that promotes the expert's tensors to L0 admission ahead of access.
- **Shared-layer reservation.** Tensors referenced from `shared/` are reserved at L1 minimum while any composition that references them is active, regardless of which expert is currently routed to.

The Memory Plane continues to make its own placement decisions for the working set; the scheduler only constrains them with composition-level groupings and pins.

---

## 10. Routing Plane Integration (RFC-0005)

Amendments to RFC-0005:

- **Expert-aware feasibility.** An invocation feasibility check now includes "all selected experts can be hosted concurrently within budget." If not, the routing plane either degrades (drop top-k from 2 to 1, raising router temperature) or fails the invocation per policy.
- **New engine capability:** `ANX_CAP_TENSOR_MULTI_MODEL_CONTEXT` — set by engines that can hold multiple expert weight sets in a single context without thrashing. Some accelerators (large unified-memory GPUs) can; others cannot. The scheduler considers this when deciding whether `staged` is achievable on the current engine.
- **Routing scope.** The routing plane sees per-expert ops as belonging to the composition's invocation. Engine selection per expert is normal (RFC-0005 unchanged), but the plane records the composition CID alongside engine selection for provenance.

The routing plane does not run the gating network. The router runs as a normal tensor op, scored and routed by RFC-0005 like any other small model.

---

## 11. Network Plane Integration (RFC-0006)

A `remote` expert is hosted on another Anunix node. The network plane handles:

- **Discovery.** Each node advertises which expert model namespaces it hosts. A composition manifest's `model_ref` for a remote expert is resolved through this directory.
- **Credential delegation.** Invoking a remote expert requires the remote node to authorize the call. Standard RFC-0006 federated cells use the credential proxy pattern (RFC-0008): the local credential is presented, and the remote node mints a delegated credential scoped to this invocation.
- **Streaming forward pass.** For token-level routing of a remote expert, the network plane streams partial outputs back rather than buffering full responses. This is the existing federated cell streaming, applied per gating step.
- **Fallback policy.** When a remote expert is unreachable, the composition manifest declares the policy: `fail`, `degrade_to_topk_minus_one`, or `substitute_with_<other-expert>`. The router output is renormalized accordingly.

This makes the phone-plus-server case natural: the phone hosts `safety` resident, `math` and `code` staged, and `tool` remote. When connectivity drops, `tool` falls out of the composition's effective expert set, and the router degrades to top-k over what remains.

---

## 12. Shell Tools

```
composition create <name> --manifest <path>
composition list
composition info <name>
composition history <name>

composition expert add <comp> <name> --model <ns@v> --residency <mode>
composition expert remove <comp> <name>
composition expert advance <comp> <name> --version <v>
composition expert rollback <comp> <name> [--versions N]
composition expert quarantine <comp> <name>
composition expert clear-quarantine <comp> <name>

composition router set <comp> <router-oid>

composition residency set <comp> <expert> <mode>
composition residency report <comp>
composition budget set <comp> --resident-bytes N --staged-bytes M --max-experts K

composition invoke <comp> <input-oid> [--no-provenance]
```

Existing RFC-0013 commands (`tensor stats`, `model info`, etc.) work transparently on referenced experts and the router.

---

## 13. Amendments to Existing RFCs

### 13.1 RFC-0002: State Object Model

- Add `ANX_OBJ_COMPOSITION`, `ANX_OBJ_MODEL_REF`, `ANX_OBJ_TENSOR_REF` to `enum anx_object_type`.
- Object store gains a cross-namespace refcount table maintained by ref-object lifecycle.
- Composition `history/` is a structured event log appended to on every manifest mutation.

### 13.2 RFC-0004: Memory Control Plane

- Expert admission group (Section 9).
- Expert-pinned page flag for `resident` mode.
- Shared-layer reservation when a composition is active.
- Prefetch-on-router-prediction hint.

### 13.3 RFC-0005: Routing Plane

- New engine capability `ANX_CAP_TENSOR_MULTI_MODEL_CONTEXT`.
- Composition CID recorded alongside engine selection.
- Expert-feasibility filter under residency budget.

### 13.4 RFC-0006: Network Plane

- Expert directory advertised per node.
- Federated expert invocation pattern (Section 11).
- Per-composition fallback policy on unreachable experts.

### 13.5 RFC-0008: Credential Objects

- Per-expert credential scope within a composition.
- Distinct router credential.
- Composition-level invocation credential.

### 13.6 RFC-0013: Tensor Objects and Model Representation

- Reference notation `models:/<name>@<version>` standardized for use in composition manifests and shared-layer declarations.
- Model namespaces gain a `referencing_compositions` query for safe deletion checks.
- Adapter trees (`adapters/`) remain a single-model concept; they are not a substitute for compositions.

---

## 14. Implementation Phases

### Phase 1 — Composition Object Model

- `ANX_OBJ_COMPOSITION`, `ANX_OBJ_MODEL_REF`, `ANX_OBJ_TENSOR_REF` types.
- Manifest read/write, history log.
- Cross-namespace refcounting.
- `composition create`, `composition info`, `composition list`, `composition history`.

### Phase 2 — Expert and Router Bindings

- Expert add/remove/advance/rollback with credential gating.
- Router pin update.
- Quarantine and clear-quarantine.

### Phase 3 — Residency Modes (Resident, Staged, Streamed)

- Residency declarations enforced at composition activation.
- Memory Plane amendments: pinned pages, admission groups, shared-layer reservations.
- `composition residency set`, `composition residency report`.
- Streamed mode using existing RFC-0013 BRIN block loads.

### Phase 4 — Invocation and Routing Provenance

- `anx_composition_invoke` end-to-end.
- Router runs as a normal tensor op via RFC-0005.
- Per-step provenance object.
- Engine feasibility filter for multi-expert contexts.

### Phase 5 — Federated Experts (Remote Mode)

- Expert directory in network plane.
- Federated forward pass with credential delegation.
- Fallback policy on unreachable experts.

### Phase 6 — Prefetch and Layer-Granularity Routing

- Router-prediction prefetch in the residency scheduler.
- Layer-granularity routing for full per-layer MoE compositions.

---

## 15. Security Considerations

### 15.1 Expert Poisoning Is Local

A compromised or degraded expert affects only the gating steps where it is selected. Because rollback is per-expert, recovery does not require touching the router or other experts. The router's role here is also defensive: a router trained with load-balancing constraints (`load_balance_alpha` in policy) cannot route arbitrarily large fractions of traffic to a single expert.

### 15.2 Router Poisoning

A compromised router can systematically misroute. Mitigations:

- Router credential is separate; compromising any expert pipeline does not authorize router updates.
- Router updates require validation gates: post-update, a held-out evaluation runs and the new router is retained only if metrics are within tolerance.
- Router rollback is one operation; the prior router version is retained per the standard model retention policy.

### 15.3 Cross-Expert Side Channels

When multiple experts share an engine context (multi-model GPU context), KV cache and activation buffers must be scoped per expert. The Routing Plane records per-expert engine handles; engines that set `ANX_CAP_TENSOR_MULTI_MODEL_CONTEXT` must guarantee buffer isolation. Without that capability, the kernel falls back to context switching between experts at the cost of latency rather than risk side-channel leakage.

### 15.4 Remote Expert Confidentiality

A remote expert's weights remain on the remote node. Only inputs, outputs, and gating decisions cross the network. Credential delegation (RFC-0008) ensures the remote node receives only the scope needed for the invocation. The composition's input tensor is encrypted in transit per standard network plane requirements.

### 15.5 Reference Pinning Prevents Silent Updates

Because every composition reference is version-pinned, no expert can be hot-swapped underneath a running composition. An update is always a recorded manifest mutation gated by the appropriate credential.

---

## 16. Relationship to External Frameworks

Anunix does not replace MoE training stacks. It provides the OS-level objects beneath them:

| External concept | Anunix object |
|------------------|---------------|
| HuggingFace `transformers` MoE block | Composition with `granularity=layer` |
| vLLM expert sharding | Compositions with mixed `staged` and `remote` experts |
| Mixture-of-Adapters | Composition where each expert is a base + LoRA delta |
| BAR independent expert | Model namespace referenced by composition |
| BAR router | Router Object |
| BAR linear-cost update | Per-expert rollback / advance |
| Expert offload to disk | `streamed` residency over BRIN block reads |
| Expert offload to another GPU node | `remote` residency over network plane |

A future Anunix backend for HuggingFace `transformers` or vLLM would map MoE block configuration onto composition manifests and let the kernel's residency scheduler replace the framework's ad-hoc weight management.

---

## 17. Appendix: Mapping the BAR Paper

| Paper concept ("Train Separately, Merge Together") | Anunix construct |
|----------------------------------------------------|------------------|
| Independent expert trained through its own pipeline | Model namespace (RFC-0013) with its own training cells (RFC-0003) and credential scope (RFC-0008) |
| Mid-training / SFT / RL stages per expert | Sequence of training cells writing tensor versions with provenance |
| Lightweight router | Router Object — small model, separately versioned |
| MoE composition | Composition Namespace |
| Linear-cost update of one expert | `anx_composition_advance_expert` + `anx_composition_set_router` |
| No degradation to existing domains | Per-expert rollback; structural isolation via reference pinning |
| Adding a new expert | `anx_composition_add_expert` + router-stale flag + router retrain |

The paper's structural claim — that the cost of update scales linearly with the number of changed experts — is recovered exactly by the kernel's per-expert reference pinning and per-expert credential scope. The kernel does not enforce the paper's training methodology; it provides the object model that makes the paper's economics realizable in practice.
