# RFC-0013: Tensor Objects and Model Representation

| Field      | Value                                                     |
|------------|-----------------------------------------------------------|
| RFC        | 0013                                                      |
| Title      | Tensor Objects and Model Representation                    |
| Author     | Adam Pippert                                              |
| Status     | Draft                                                     |
| Created    | 2026-04-17                                                |
| Updated    | 2026-04-17                                                |
| Depends On | RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0007, RFC-0008, RFC-0009 |

---

## Executive Summary

Anunix exists to be an AI-native operating system. An operating system that cannot natively represent, version, inspect, slice, and transform the fundamental data structures of AI — tensors, weight matrices, computation graphs — is not AI-native. It is a general-purpose OS with an inference API bolted on.

RFC-0013 introduces the **Tensor Object**, a new State Object type that makes multi-dimensional arrays first-class kernel citizens. A Tensor Object is not a file containing binary data that happens to be a matrix. It is a semantically typed object with shape, dtype, and statistical metadata (BRIN-style summaries) that the kernel understands, indexes, and can operate on without loading the full payload. When a model's weights are stored as Tensor Objects in a namespace, the kernel can answer questions like "which layers drifted most from the base model?" or "find all tensors with sparsity above 0.5" — without reading a single weight.

This RFC also specifies the **Model Namespace Convention**: a standard layout for organizing model weights, architecture descriptions, computation graphs, and training metadata as a composition of State Objects. A model is not one object — it is a namespace of objects. This decomposition enables layer-level versioning, selective fine-tuning, LoRA adapter composition, cross-model layer sharing, and self-editing (SEAL-style weight modification with full provenance tracking).

Finally, this RFC defines the **Tensor Compute Protocol**: how Execution Cells request tensor operations through the Routing Plane, how engines register tensor capabilities, and how the kernel mediates access to weight objects during training, fine-tuning, and inference. The kernel does not implement matmul. It provides the object model, access control, provenance, and routing that make tensor operations governable, auditable, and composable.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert / public collaborators
**Depends on:** RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0007, RFC-0008, RFC-0009

---

## 2. Problem Statement

### 2.1 Models Are Not Files

In every existing operating system, a model is a file. A 70-billion-parameter language model is a 140GB blob on a filesystem. The OS knows nothing about its contents. To find out which layers changed between two checkpoints, you must load both files entirely into memory, deserialize them, and compare every tensor. To fine-tune three layers out of eighty, you must load and save the entire model. To share an embedding layer between two models, you must duplicate the data.

This is not a storage efficiency problem — it is a semantic gap. The operating system cannot participate in model operations because it does not know what a model is.

### 2.2 Self-Editing Requires Governance

The SEAL paper (2506.10943) demonstrates that models can learn to modify their own weights — performing targeted edits to internal representations to encode new knowledge or correct errors. This is powerful but dangerous. An uncontrolled self-edit could degrade model quality, introduce biases, or corrupt critical capabilities.

In a classical OS, there is no mechanism to govern weight modifications. A process can overwrite a model file freely. There is no access control at the tensor level, no audit trail of which weights changed, no automatic rollback if metrics degrade, and no way to restrict which layers a fine-tuning job can touch.

Anunix's existing primitives — access-controlled State Objects with provenance tracking, Execution Cells with capability-scoped authorization, credential-gated engine access — are precisely what self-editing models need. But they require tensor-level granularity to be useful.

### 2.3 What Existing RFCs Cannot Express

- **RFC-0002 (State Objects)** can store tensor data as `ANX_OBJ_BYTE_DATA`, but the kernel cannot distinguish a weight matrix from a JPEG. No shape, no dtype, no statistical metadata.

- **RFC-0004 (Memory Control Plane)** manages memory tiers, but placement decisions are based on access patterns, not tensor semantics. A frequently-accessed but largely-zero sparse tensor wastes L0 cache that a dense active tensor needs.

- **RFC-0005 (Routing Plane)** routes work to engines, but has no concept of tensor-specific capabilities. It cannot express "route this matmul to a GPU engine that supports bf16" or "this tensor is int8 quantized, select an engine that handles quantized inference."

- **RFC-0009 (Agent Memory)** provides episodic and semantic memory for agents, but model weights are not agent memories — they are structured mathematical objects that require different operations (linear algebra, not similarity search).

---

## 3. Goals

### 3.1 Primary Goals

1. **Tensors as typed objects.** A new `ANX_OBJ_TENSOR` type carries shape, dtype, and statistical summaries as kernel-visible metadata. The kernel can index, query, and route operations based on tensor properties.

2. **BRIN-style indexing.** Each Tensor Object stores block-range summary statistics (mean, variance, norm, sparsity, min, max) computed on seal. Metadata queries ("find sparse tensors", "find layers that drifted from baseline") work without loading tensor payloads.

3. **Sub-tensor access.** Slice operations create new Tensor Objects that reference ranges of a parent tensor's payload. Read a 1000-row slice of a 32000-row embedding without loading 250MB.

4. **Models as namespaces.** A model is a namespace containing tensor objects (weights), structured objects (architecture, hyperparameters), and graph objects (computation graph). Standard layout enables tooling, versioning, and composition.

5. **Delta versioning.** Fine-tuned tensors store the delta from their parent version. A LoRA adapter is a set of small delta tensor objects referencing the base model's OIDs.

6. **Self-edit governance.** Cells that modify tensor objects are subject to access control (which layers can they write?), provenance (what changed and why?), and rollback (revert if metrics degrade). The kernel enforces this at the object level.

7. **Tensor-aware routing.** The Routing Plane considers tensor dtype, shape, and target hardware when selecting engines. A bf16 tensor routes to a GPU engine; an int8 tensor routes to a CPU VNNI engine.

8. **Phone to cluster.** The tensor object model is the same on a phone (one small int4 model, CPU inference) and a cluster (sharded fp32 model across 8 GPUs). The kernel abstracts the hardware; the object model is uniform.

### 3.2 Non-Goals

- **Implementing linear algebra in the kernel.** Matmul, convolutions, attention — these are engine operations, not kernel operations. The kernel provides the objects and routing; engines provide the math.

- **Training frameworks.** PyTorch, JAX, MLX — these are userspace frameworks that run as Cells and operate on Tensor Objects. The kernel does not replace them.

- **Model architecture search.** The kernel stores and versions architectures as structured objects. Searching the architecture space is an agent task.

---

## 4. Core Definitions

### 4.1 Tensor Object

A **Tensor Object** is a State Object of type `ANX_OBJ_TENSOR` containing a dense multi-dimensional array of typed elements. Its metadata includes:

- **Shape**: dimensions of the array (up to 8 dimensions)
- **Dtype**: element data type (float16, bfloat16, float32, float64, int8, uint8, int4)
- **BRIN summary**: statistical properties computed on seal (mean, variance, L2 norm, sparsity, min, max)
- **Block index**: optional sub-tensor access map for range queries

### 4.2 Tensor Dtype

```c
enum anx_tensor_dtype {
    ANX_DTYPE_FLOAT16,    /* IEEE 754 half-precision */
    ANX_DTYPE_BFLOAT16,   /* Brain floating point */
    ANX_DTYPE_FLOAT32,    /* IEEE 754 single-precision */
    ANX_DTYPE_FLOAT64,    /* IEEE 754 double-precision */
    ANX_DTYPE_INT8,       /* signed 8-bit integer */
    ANX_DTYPE_UINT8,      /* unsigned 8-bit integer */
    ANX_DTYPE_INT4,       /* packed 4-bit integer (2 per byte) */
    ANX_DTYPE_INT32,      /* signed 32-bit integer */
    ANX_DTYPE_BOOL,       /* 1-bit packed boolean */
};
```

### 4.3 Tensor Metadata

```c
struct anx_tensor_meta {
    uint32_t ndim;               /* number of dimensions (1-8) */
    uint64_t shape[8];           /* size of each dimension */
    enum anx_tensor_dtype dtype; /* element type */
    uint64_t elem_count;         /* product of shape */
    uint64_t byte_size;          /* total payload bytes */

    /* BRIN summary — computed on seal, never stale */
    float    stat_mean;
    float    stat_variance;
    float    stat_l2_norm;
    float    stat_sparsity;      /* fraction of elements < epsilon */
    float    stat_min;
    float    stat_max;

    /* Lineage */
    anx_oid_t parent_tensor;     /* base tensor for delta versioning */
    bool      is_delta;          /* payload is diff from parent */
};
```

### 4.4 BRIN Block Index

For sub-tensor access without loading the full payload:

```c
struct anx_tensor_brin_block {
    uint64_t byte_offset;        /* offset into tensor payload */
    uint64_t byte_size;          /* bytes in this block */
    uint64_t dim_start[8];       /* start index per dimension */
    uint64_t dim_end[8];         /* end index (exclusive) per dimension */
    float    block_mean;
    float    block_max;
    float    block_min;
    float    block_sparsity;
};
```

A BRIN index divides the tensor into blocks along the first dimension and stores summary statistics per block. A 32000-row embedding matrix with 128-row blocks has 250 BRIN entries. Finding "rows where the mean activation exceeds threshold" scans 250 summaries instead of 32000 rows.

### 4.5 Model Namespace

A **Model Namespace** is a standard layout for organizing a model's components as State Objects:

```
models:/<model-name>/
  manifest             ANX_OBJ_STRUCTURED_DATA
    {
      "name": "llama-3-8b",
      "architecture": "transformer",
      "parameters": 8000000000,
      "layers": 32,
      "hidden_dim": 4096,
      "vocab_size": 32000,
      "dtype": "bfloat16",
      "parent_model": null,
      "training": { "dataset": "...", "steps": 1000000 }
    }

  graph                ANX_OBJ_STRUCTURED_DATA
    (computation graph: layer connectivity, attention patterns)

  layers/
    embed_tokens       ANX_OBJ_TENSOR  [32000, 4096] bf16
    blocks/
      0/
        attn/
          q_proj       ANX_OBJ_TENSOR  [4096, 4096] bf16
          k_proj       ANX_OBJ_TENSOR  [4096, 1024] bf16
          v_proj       ANX_OBJ_TENSOR  [4096, 1024] bf16
          o_proj       ANX_OBJ_TENSOR  [4096, 4096] bf16
        mlp/
          gate_proj    ANX_OBJ_TENSOR  [4096, 11008] bf16
          up_proj      ANX_OBJ_TENSOR  [4096, 11008] bf16
          down_proj    ANX_OBJ_TENSOR  [11008, 4096] bf16
        norm           ANX_OBJ_TENSOR  [4096] bf16
      1/
        ...
    lm_head            ANX_OBJ_TENSOR  [4096, 32000] bf16

  adapters/            (LoRA adapters — small delta tensors)
    medical-lora/
      blocks/0/attn/q_proj_A  ANX_OBJ_TENSOR [4096, 16] bf16
      blocks/0/attn/q_proj_B  ANX_OBJ_TENSOR [16, 4096] bf16
      ...
```

### 4.6 Delta Versioning

When a tensor is fine-tuned, the new version stores only the **delta** (difference) from the parent:

- Parent tensor: `blocks/0/attn/q_proj` version 1 (base weights)
- Fine-tuned tensor: `blocks/0/attn/q_proj` version 2 (delta from v1)
- `is_delta = true`, `parent_tensor = OID of v1`
- Payload contains `v2 - v1` (compressed, often highly sparse)
- Reconstruction: `v2_full = v1_full + delta`

This means a LoRA fine-tune that touches 10% of layers stores only ~10% of the model size, with full provenance connecting every delta to its base.

---

## 5. Design Principles

### 5.1 The Kernel Understands Shape, Not Math

The kernel knows that a Tensor Object has shape `[4096, 4096]`, dtype `bfloat16`, and sparsity `0.03`. It does NOT know how to multiply two tensors. Shape and dtype are metadata for routing and indexing. Math is engine work.

### 5.2 BRIN Summaries Are Computed Once

When a Tensor Object is **sealed** (made immutable), the kernel computes its BRIN summary statistics. These are stored in the tensor metadata and never recomputed. Since sealed objects are immutable, the statistics are always consistent. This is the same principle as PostgreSQL's BRIN indexes on append-only tables.

### 5.3 Models Are Compositions, Not Monoliths

A model is not one object. It is a namespace of objects. This is not a storage optimization — it is a governance requirement. Different layers require different access controls, different versioning policies, different tier placement, and different provenance histories. A monolithic model file makes all of this impossible.

### 5.4 Self-Edit Is Just Object Mutation

A model modifying its own weights is an Execution Cell writing new versions of Tensor Objects. The kernel does not need special "self-edit" support. It needs tensor-level access control (RFC-0008 credential bindings can restrict which tensor objects a Cell may write), tensor-level provenance (RFC-0002 records every mutation), and tensor-level versioning (the object store keeps previous versions for rollback).

### 5.5 Hardware Abstraction Through Routing

A `matmul` on an int8 tensor routes differently than on a bf16 tensor. The Routing Plane's engine registry carries tensor-specific capabilities:

```c
#define ANX_CAP_TENSOR_FP32     (1 << 16)
#define ANX_CAP_TENSOR_BF16     (1 << 17)
#define ANX_CAP_TENSOR_INT8     (1 << 18)
#define ANX_CAP_TENSOR_INT4     (1 << 19)
#define ANX_CAP_TENSOR_GPU      (1 << 20)
```

The kernel selects the right engine based on the tensor's dtype and available hardware. Application code says "multiply these two tensors" — the kernel figures out where and how.

---

## 6. Kernel Operations

### 6.1 Tensor Object Creation

```c
/* Create a tensor object with metadata */
int anx_tensor_create(const struct anx_tensor_meta *meta,
                       const void *data, uint64_t data_size,
                       struct anx_state_object **out);

/* Seal a tensor (computes BRIN summary, makes immutable) */
int anx_tensor_seal(const anx_oid_t *oid);
```

### 6.2 Sub-Tensor Access (Slicing)

```c
/* Create a view (new tensor object) from a slice of an existing tensor.
 * The view shares payload storage with the parent (copy-on-write). */
int anx_tensor_slice(const anx_oid_t *src,
                      const uint64_t *start, const uint64_t *end,
                      uint32_t ndim,
                      struct anx_state_object **out);
```

### 6.3 Metadata Query

```c
/* Get tensor metadata without loading payload */
int anx_tensor_meta(const anx_oid_t *oid,
                     struct anx_tensor_meta *meta_out);

/* Search tensors by statistical properties */
int anx_tensor_search(float min_sparsity, float max_sparsity,
                       enum anx_tensor_dtype dtype_filter,
                       anx_oid_t *results, uint32_t max_results,
                       uint32_t *count);
```

### 6.4 Delta Operations

```c
/* Compute delta between two tensor versions */
int anx_tensor_diff(const anx_oid_t *base, const anx_oid_t *modified,
                     struct anx_state_object **delta_out);

/* Reconstruct full tensor from base + delta */
int anx_tensor_apply_delta(const anx_oid_t *base,
                            const anx_oid_t *delta,
                            struct anx_state_object **full_out);
```

### 6.5 Quantization

```c
/* Create a quantized copy of a tensor */
int anx_tensor_quantize(const anx_oid_t *src,
                         enum anx_tensor_dtype target_dtype,
                         struct anx_state_object **out);
```

---

## 7. Shell Tools

```
tensor create <ns:path> <shape> <dtype>    Create an empty tensor
tensor stats <ns:path>                      Show BRIN summary
tensor slice <ns:path> <dim:start:end>     Create a slice view
tensor diff <path-a> <path-b>              Show delta between versions
tensor quantize <ns:path> <dtype>          Create quantized copy
tensor search sparsity>0.5                  Find tensors by property

model load <ns:path> <safetensors-file>    Import model from file
model info <ns:path>                        Show model manifest
model layers <ns:path>                      List layers with stats
model diff <model-a> <model-b>             Compare two models layer-by-layer
model lora <base> <adapter> <output>       Merge LoRA adapter
```

---

## 8. Self-Edit Protocol

### 8.1 Edit Cell

A self-edit is an Execution Cell of type `ANX_CELL_MODEL_EDIT`:

```c
struct anx_model_edit_intent {
    anx_oid_t model_namespace;      /* which model to edit */
    char target_layers[16][256];    /* which layers to modify */
    uint32_t target_layer_count;
    char edit_description[1024];    /* what the edit achieves */
    float max_delta_norm;           /* bound on weight change magnitude */
};
```

### 8.2 Edit Governance

1. **Admission**: the Cell must hold credential bindings for each target layer. A Cell authorized to edit `blocks/0-5` cannot touch `blocks/6+`.

2. **Execution**: the Cell reads current tensor objects, computes new weights (via engine), writes new tensor versions.

3. **Validation**: after the edit, a validation Cell runs inference on a held-out set. If metrics degrade beyond threshold, the edit is automatically rolled back (previous tensor versions restored).

4. **Provenance**: every edited tensor records: the edit Cell's CID, the edit description, the input data that motivated the edit, the delta norm, and the validation result.

### 8.3 Rollback

```c
/* Rollback a model edit — restore tensors to their pre-edit versions */
int anx_model_rollback(const anx_oid_t *model_namespace,
                        const anx_cid_t *edit_cell_cid);
```

---

## 9. Memory Plane Integration

### 9.1 Tensor-Aware Tier Placement

The Memory Control Plane (RFC-0004) uses tensor metadata for placement decisions:

| Tier | Tensor Placement Rule |
|------|-----------------------|
| L0 (active) | Tensors currently loaded in an active inference engine |
| L1 (cache) | Recently-used tensors, especially attention KV cache |
| L2 (local store) | Full model weights on local storage |
| L3 (retrieval) | Embedding layers used for semantic search (RFC-0009) |
| L4 (long-term) | Historical model versions, training checkpoints |
| L5 (remote) | Distributed model shards on remote nodes |

### 9.2 Sparsity-Aware Compression

Tensors with sparsity > 0.5 (from BRIN metadata) are automatically stored in compressed sparse format at L2+. Dense tensors use dense storage. The kernel handles format conversion transparently.

---

## 10. Routing Plane Integration

### 10.1 Tensor Engine Capabilities

Engines register tensor-specific capabilities:

```c
struct anx_tensor_engine_caps {
    uint32_t supported_dtypes;   /* bitmask of anx_tensor_dtype */
    uint32_t max_tensor_dims;    /* max dimensions supported */
    uint64_t max_tensor_bytes;   /* max single tensor size */
    bool     supports_sparse;    /* can handle sparse format */
    bool     supports_quantized; /* handles int8/int4 natively */
    char     accelerator[64];    /* "cpu", "cuda", "metal", "npu" */
};
```

### 10.2 Tensor Operation Routing

When a Cell requests a tensor operation (matmul, attention, etc.), the routing plane:

1. Inspects the input tensors' dtype and shape from metadata (no data load needed)
2. Filters engines by dtype support and accelerator availability
3. Scores engines by expected throughput for this tensor size and dtype
4. Routes to the best engine

This means the same model code works on CPU (int8 engine), GPU (bf16 engine), and phone (int4 engine) — the routing plane adapts automatically.

---

## 11. Amendments to Existing RFCs

### 11.1 RFC-0002: State Object Model

- Add `ANX_OBJ_TENSOR` to `enum anx_object_type`
- Tensor Objects carry `struct anx_tensor_meta` in system metadata
- Sealing a Tensor Object triggers BRIN summary computation

### 11.2 RFC-0004: Memory Control Plane

- Tier placement considers tensor metadata (dtype, sparsity, access frequency)
- Sparse tensors may be stored in compressed format at L2+
- Embedding tensors used for RFC-0009 semantic search placed in L3

### 11.3 RFC-0005: Routing Plane

- Engine capabilities extended with `struct anx_tensor_engine_caps`
- Route scoring considers tensor dtype compatibility and accelerator match
- New feasibility filter: `tensor_dtype_supported`

### 11.4 RFC-0008: Credential Objects

- Tensor-level access control: credential bindings can restrict which tensor objects (by namespace path pattern) a Cell may read or write
- Self-edit Cells require explicit tensor write authorization

### 11.5 RFC-0009: Agent Memory

- Agent memories can reference tensor objects (e.g., "this embedding was important for task X")
- Embedding layers in L3 tier provide the vector store for semantic memory search

---

## 12. Implementation Phases

### Phase 1 — Tensor Object Type

- `ANX_OBJ_TENSOR` in State Object enum
- `struct anx_tensor_meta` in tensor header
- `tensor create`, `tensor stats` shell commands
- BRIN summary computation on seal

### Phase 2 — Model Namespace

- Standard model layout convention
- `model load` (import from safetensors/GGUF)
- `model info`, `model layers` shell commands
- Delta versioning (store diffs between tensor versions)

### Phase 3 — Tensor Operations

- `tensor slice` (sub-tensor views)
- `tensor diff` (compare versions)
- `tensor quantize` (dtype conversion)
- `tensor search` (BRIN metadata queries)

### Phase 4 — Compute Routing

- Tensor engine capability registration
- Dtype-aware route scoring
- CPU tensor math engine (reference implementation)
- GPU engine interface (CUDA/Metal abstraction)

### Phase 5 — Self-Edit Protocol

- `ANX_CELL_MODEL_EDIT` cell type
- Tensor-level access control enforcement
- Automatic validation after edit
- Rollback on metric degradation
- Full provenance for every weight change

---

## 13. Relationship to External Frameworks

Anunix does not replace PyTorch, JAX, or MLX. It provides the operating system layer beneath them:

| Framework Operation | Anunix Layer |
|--------------------|--------------|
| `torch.load()` | Read Tensor Objects from namespace |
| `model.parameters()` | Iterate tensor objects in model namespace |
| `optimizer.step()` | Cell writes new tensor versions with provenance |
| `torch.save()` | Seal modified tensors, compute BRIN summaries |
| `model.to(device)` | Routing plane selects engine for target accelerator |
| `torch.quantize()` | `anx_tensor_quantize()` creates new typed tensor |

A future "Anunix PyTorch" would be a PyTorch backend that stores tensors as Anunix Tensor Objects instead of files, routes operations through the Anunix Routing Plane instead of dispatching directly to CUDA, and records training provenance automatically through the object store.

---

## 14. POSIX Compatibility

In the POSIX compatibility layer, tensor objects appear as files under a virtual `/models/` namespace:

```
/models/llama-3-8b/manifest.json
/models/llama-3-8b/layers/embed_tokens.tensor
/models/llama-3-8b/layers/blocks/0/attn/q_proj.tensor
```

The `.tensor` extension signals the POSIX layer to include shape and dtype in `stat()` output. `read()` returns the raw tensor payload. `mmap()` provides direct memory-mapped access to tensor data for zero-copy engine consumption.

---

## 15. Security Considerations

### 15.1 Model Poisoning

Self-editing creates a vector for model poisoning. Mitigations:

- **Access control**: only authorized Cells can write to tensor objects
- **Delta bounds**: `max_delta_norm` in edit intent limits magnitude of weight changes
- **Validation gates**: automatic post-edit validation before the edit is committed
- **Provenance audit**: every edit traces to the Cell, the input data, and the motivation
- **Rollback**: any edit can be reverted by restoring previous tensor versions

### 15.2 Weight Exfiltration

Model weights are valuable intellectual property. Tensor Objects use the same access control as all State Objects (RFC-0002 Section 6). Additionally:

- Tensor Objects can be marked as non-migratable (like credentials, RFC-0008) to prevent network distribution
- Remote inference uses the credential proxy pattern: the model stays local, only inference results cross network boundaries
