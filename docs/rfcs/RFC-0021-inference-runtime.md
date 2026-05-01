# RFC-0021: Inference Runtime (anxml)

| Field      | Value                                                     |
|------------|-----------------------------------------------------------|
| RFC        | 0021                                                      |
| Title      | Inference Runtime (anxml)                                 |
| Author     | Adam Pippert                                              |
| Status     | Draft                                                     |
| Created    | 2026-05-01                                                |
| Updated    | 2026-05-01                                                |
| Depends On | RFC-0002, RFC-0003, RFC-0005, RFC-0007, RFC-0013, RFC-0014, RFC-0022 |

---

## Executive Summary

RFC-0013 makes tensors first-class kernel objects. RFC-0014 enumerates the hardware Anunix runs on. Neither specifies how a model actually executes. This RFC defines the **inference runtime** — `anxml` — the C subsystem that turns a model namespace plus an input prompt into a stream of output tokens.

`anxml` is Anunix's MLX-equivalent. It is not a port of MLX (which is C++ and Metal-bound). It is a from-scratch C implementation built on Anunix primitives: Tensor Objects (RFC-0013) for weights and activations, Execution Cells (RFC-0003) for inference sessions, the Routing Plane (RFC-0005) for backend selection, and the GPU Compute Plane (RFC-0022) for accelerator dispatch.

The runtime is structured as three layers: a **operator interface** that defines the typed primitives a backend must implement; a set of **backends** (CPU/NEON, AGX, XDNA, ANE) that each implement that interface against their hardware; and a **graph executor** that walks a model's computation graph, dispatches operators through the routing plane, manages KV cache lifetime, and runs the generation loop. Userland touches the runtime through `ansh model run` and the existing exec HTTP endpoint.

The CPU/NEON backend is the reference implementation. It is expected to ship before any GPU support and remain the correctness oracle for every other backend.

---

## 1. Problem Statement

### 1.1 RFC-0013 Stops Short of Execution

RFC-0013 specifies Tensor Objects with shape, dtype, BRIN summaries, and a model namespace layout. It explicitly states *"the kernel does not implement matmul"* and defers compute to engines. But it does not define:

- What a model's **forward pass** looks like as Anunix code.
- What **operators** a backend must implement.
- How **KV cache** lives across tokens within a session.
- How **token generation** (sampling) interacts with the object model.
- How a **GGUF or safetensors file** becomes a populated model namespace.
- How the **routing plane** chooses between CPU, AGX, XDNA, and ANE for a given operator on a given dtype.

Without these, RFC-0013 is a storage layer with no consumer.

### 1.2 MLX Is Not Portable

MLX is C++ and tightly bound to Metal. Its lazy-evaluation graph, unified-memory assumptions, and Metal kernel library cannot be lifted into Anunix without violating the no-C++ rule and without an AGX driver to run on. A C reimplementation is required, and once it is required, it should be designed to fit Anunix's primitives rather than mimic MLX's API.

### 1.3 The Runtime Must Be Backend-Agnostic

Anunix targets multiple ML accelerators (RFC-0014 Section 7): AMD XDNA NPU on Framework Desktop, Apple AGX GPU and ANE on M1, eventually Radeon and RTX. The same model must run on all of them with the same code path above the operator interface. The runtime cannot be designed around any one accelerator's quirks.

---

## 2. Goals

### 2.1 Primary Goals

1. **Pure C, no exceptions.** Kernel-style C11 throughout. SIMD intrinsics are acceptable; C++ and Rust are not.
2. **Backend-agnostic operator interface.** A single typed vtable that CPU, AGX, XDNA, and ANE all implement.
3. **Model loading from GGUF.** GGUF is the lingua franca of quantized open-weight models. Day-one support means Anunix can run any LLM the community ships.
4. **Correct-by-construction CPU reference.** Every backend's output is validated against the CPU/NEON path on every operator.
5. **KV cache as Tensor Objects.** Cache tensors live in the State Object store with explicit lifetime, allowing inspection, reuse across sessions, and tier-aware placement (RFC-0004).
6. **Inference Cell type.** A new Execution Cell type (`ANX_CELL_INFERENCE`) carries the session, dispatches operators, and surfaces tokens to userland.
7. **Streaming generation.** Tokens stream as they are produced, not after the full completion.

### 2.2 Non-Goals

- **Training.** This RFC covers inference only. Training (gradient computation, optimizer state) is a future RFC and a much larger undertaking.
- **A general tensor compiler.** No NIR, no SPIR-V, no autograd. The operator set is fixed and curated; new operators are added by hand.
- **Replacing PyTorch.** Anunix is not a Python ML environment. PyTorch interop, if it ever happens, is a separate userland project.
- **Multi-modal models.** Text-only generation in the initial scope. Vision and audio operators are future extensions.

---

## 3. Architecture

```
                 ┌────────────────────────────────────────┐
                 │              ansh / HTTP API           │
                 │   model load · model run · stream      │
                 └───────────────────┬────────────────────┘
                                     │
                 ┌───────────────────▼────────────────────┐
                 │         Inference Cell (RFC-0003)      │
                 │  session · KV cache · sampler · loop   │
                 └───────────────────┬────────────────────┘
                                     │
                 ┌───────────────────▼────────────────────┐
                 │          Graph Executor (anxml)        │
                 │  walk computation graph · dispatch ops │
                 └───────────────────┬────────────────────┘
                                     │
                 ┌───────────────────▼────────────────────┐
                 │          Operator Interface            │
                 │     anx_op_vtable (typed dispatch)     │
                 └───┬─────────┬──────────┬──────────┬────┘
                     │         │          │          │
              ┌──────▼──┐ ┌────▼─────┐ ┌──▼─────┐ ┌──▼─────┐
              │ CPU/    │ │ AGX      │ │ XDNA   │ │ ANE    │
              │ NEON    │ │ (RFC-22) │ │ (0014) │ │ (0014) │
              └─────────┘ └──────────┘ └────────┘ └────────┘
```

The graph executor is hardware-agnostic. Each backend implements `struct anx_op_vtable` against its hardware. Backend selection per operator is decided by the Routing Plane (RFC-0005) using the engine capabilities registered by each backend.

---

## 4. Core Definitions

### 4.1 Operators

The operator set is fixed. Adding a new operator requires an RFC amendment. The initial set covers transformer LLM inference:

```c
enum anx_op_kind {
    /* tensor ops */
    ANX_OP_MATMUL,        /* C = A @ B (with optional bias) */
    ANX_OP_GEMV,          /* y = A @ x (matrix-vector) */
    ANX_OP_ADD,           /* C = A + B (broadcasted) */
    ANX_OP_MUL,           /* C = A * B (elementwise) */
    ANX_OP_SCALE,         /* C = A * s (scalar) */

    /* normalization */
    ANX_OP_RMSNORM,       /* RMS norm with learned scale */
    ANX_OP_LAYERNORM,     /* layer norm with learned scale + bias */

    /* activations */
    ANX_OP_SILU,          /* x * sigmoid(x) */
    ANX_OP_GELU,          /* Gaussian error linear unit */
    ANX_OP_SOFTMAX,       /* row-wise softmax */

    /* attention */
    ANX_OP_ROPE,          /* rotary position embedding */
    ANX_OP_SDPA,          /* scaled dot-product attention (causal/non) */

    /* embedding */
    ANX_OP_EMBED_LOOKUP,  /* token id → embedding row */

    /* sampling — special: input is logits, output is token id */
    ANX_OP_SAMPLE,        /* dispatches to sampler config */
};
```

Operators that decompose into others (e.g., transformer block) are *not* operators. They are graph templates the executor walks.

### 4.2 Operator Vtable

```c
struct anx_op_args {
    enum anx_op_kind kind;
    const struct anx_state_object *inputs[8];
    uint32_t input_count;
    struct anx_state_object *output;       /* pre-allocated by executor */
    union {
        struct { bool transpose_a, transpose_b; float bias_scale; } matmul;
        struct { float epsilon; } norm;
        struct { float theta_base; uint32_t pos; } rope;
        struct { bool causal; float scale; } sdpa;
        struct { float temperature; uint32_t top_k; float top_p; } sample;
    } u;
};

struct anx_op_vtable {
    const char *name;                       /* "cpu_neon", "agx", "xdna", "ane" */
    uint32_t supported_dtypes;              /* bitmask of anx_tensor_dtype */
    uint32_t supported_ops;                 /* bitmask of anx_op_kind */

    int  (*init)(struct anx_op_backend *self);
    void (*fini)(struct anx_op_backend *self);

    /* dispatch a single operator. Synchronous; backends may queue internally. */
    int  (*dispatch)(struct anx_op_backend *self, struct anx_op_args *args);

    /* for async backends (GPU): enqueue, get fence, wait */
    int  (*dispatch_async)(struct anx_op_backend *self,
                           struct anx_op_args *args,
                           anx_fence_t *fence_out);
    int  (*fence_wait)(struct anx_op_backend *self, anx_fence_t fence);
};
```

A backend implements either `dispatch` or `dispatch_async`. The graph executor adapts.

### 4.3 Inference Session

```c
struct anx_inference_session {
    anx_oid_t   model_namespace;     /* RFC-0013 model namespace OID */
    anx_oid_t   tokenizer;           /* tokenizer state object */

    /* per-session state */
    uint32_t    pos;                 /* current sequence position */
    uint32_t    max_context;         /* model's context window */
    anx_oid_t   kv_cache;            /* tensor object: [layers, 2, heads, ctx, head_dim] */

    /* sampler config */
    float       temperature;
    uint32_t    top_k;
    float       top_p;
    uint64_t    rng_seed;

    /* backend selection (set by routing plane on first dispatch) */
    struct anx_op_backend *primary_backend;
    struct anx_op_backend *fallback_backend; /* always CPU/NEON */
};
```

KV cache is a Tensor Object, not a backend-private allocation. This allows inspection, snapshotting, transfer between backends, and tier placement under the Memory Control Plane.

### 4.4 Inference Cell

A new Execution Cell type:

```c
#define ANX_CELL_INFERENCE  (ANX_CELL_TYPE_BASE + 21)

struct anx_inference_intent {
    anx_oid_t   model_namespace;
    char        prompt[8192];        /* input text */
    uint32_t    max_tokens;
    float       temperature;
    uint32_t    top_k;
    float       top_p;
    uint64_t    rng_seed;
    bool        stream;              /* emit tokens incrementally */
};
```

The cell holds a credential binding (RFC-0008) authorizing read access to the model namespace. Streaming is implemented through the cell's standard output channel.

### 4.5 Computation Graph

A model's computation graph is a structured object in its namespace (RFC-0013 §4.5):

```
models:/llama-3-8b/graph
```

Encoded as a flat list of operator invocations referencing tensor OIDs by namespace path. The graph executor reads this once at session start, resolves OIDs, allocates output tensors, and produces a dispatch plan:

```c
struct anx_dispatch_plan {
    struct anx_op_args *steps;       /* ordered operator invocations */
    uint32_t step_count;
    anx_oid_t *intermediate_tensors; /* allocated activations */
    uint32_t intermediate_count;
};
```

For an 8B-parameter Llama-style model, a forward pass is roughly 32 layers × ~10 operators = 320 dispatch steps.

---

## 5. CPU/NEON Backend (Reference)

The CPU backend is the correctness oracle. Every operator must produce bit-exact (or bounded-error for non-associative float ops) output equal to the CPU implementation.

### 5.1 Layout

```
lib/anxml/backend/cpu/
    cpu_backend.c          /* vtable registration, dispatch */
    cpu_matmul.c           /* GEMM kernels (NEON for aarch64, AVX2 for x86_64) */
    cpu_attn.c             /* RoPE, SDPA */
    cpu_norm.c             /* RMSNorm, LayerNorm */
    cpu_activation.c       /* SiLU, GELU, Softmax */
    cpu_sample.c           /* sampling (top-k/top-p/temp) */
    cpu_quant.c            /* int8/int4 dequantization fast paths */
```

### 5.2 SIMD Strategy

- **aarch64**: NEON intrinsics (`<arm_neon.h>`). 128-bit vectors, FMLA fused multiply-add, `vdotq_s32` for int8 dot products on Apple cores.
- **x86_64**: AVX2 intrinsics (`<immintrin.h>`). 256-bit vectors. AVX-512 path optional, gated on CPU detection.
- **Fallback**: scalar C path for correctness. Always compiled, used on architectures without SIMD support and as the bit-reference for SIMD validation.

### 5.3 Dtype Support

CPU backend supports all dtypes in RFC-0013 §4.2: f32, f16 (storage only on most CPUs, computed in f32), bf16, int8, int4. f16/bf16 paths use scalar conversion for systems without hardware support, NEON FP16 instructions on Apple cores.

### 5.4 Quantization

GGUF k-quants (Q4_K, Q5_K, Q6_K, Q8_0) are decoded inline during matmul. The quantized weight stays in memory; dequantized blocks are produced in registers. This matches `llama.cpp`'s strategy and is the basis of CPU inference performance.

---

## 6. GGUF Loader

### 6.1 Why GGUF First

GGUF is the dominant format for quantized open-weight models. Supporting it day-one means Anunix runs anything on Hugging Face that has been converted (which is, in practice, everything popular). Safetensors comes second.

### 6.2 Loader Pipeline

```c
/* Read a GGUF file and populate a model namespace */
int anx_gguf_import(const char *path, const char *namespace_dest);
```

The loader:

1. Parses GGUF header and metadata into a `manifest` structured object.
2. For each tensor in the GGUF, creates an `ANX_OBJ_TENSOR` with the correct shape and dtype.
3. Maps the GGUF tensor data into the tensor object's payload (mmap if file-backed, copy if memory-backed).
4. Synthesizes the model graph from the metadata's architecture description (Llama-style models share a structure that can be templated).
5. Seals all tensors, triggering BRIN summary computation.

### 6.3 Tokenizer

Tokenizers ship inside GGUF. The loader extracts the tokenizer (vocabulary, merges, special tokens) into a structured object at `models:/<name>/tokenizer`. The runtime uses a pure-C BPE implementation (~500 lines).

---

## 7. Backend Selection (Routing Plane)

Each backend registers as an Execution Engine (RFC-0005) with capabilities:

```c
struct anx_op_engine_registration {
    const char *name;              /* "anxml-cpu", "anxml-agx", etc. */
    uint32_t    supported_ops;     /* bitmask of anx_op_kind */
    uint32_t    supported_dtypes;
    uint64_t    max_tensor_bytes;
    char        accelerator[64];   /* matches RFC-0013 §10.1 */
    void       *vtable;            /* struct anx_op_vtable * */
};
```

For each operator in the dispatch plan, the routing plane:

1. Filters engines that support the operator and the input dtypes.
2. Scores by accelerator preference (GPU > NPU > CPU for compute-bound; reverse for tiny tensors).
3. Routes to the winner; falls back to CPU if the winner errors.

The `primary_backend` cached in the session is the dominant winner across operators; outliers (one CPU-only op in an otherwise-GPU plan) are dispatched per-op.

---

## 8. Userland Surface

### 8.1 ansh Commands

```
model load <ns:path> <gguf-file>      Import a model from GGUF
model run  <ns:path> <prompt> [opts]  Generate a completion
model chat <ns:path>                  Interactive chat session
model info <ns:path>                  Show manifest, layer count, dtype
```

Options for `model run`:

```
  --max-tokens N        max output tokens (default 512)
  --temp F              sampling temperature (default 0.8)
  --top-k N             top-k sampling (default 40)
  --top-p F             top-p / nucleus (default 0.95)
  --seed N              RNG seed (default: random)
  --backend NAME        force a specific backend
```

### 8.2 HTTP API

The existing exec endpoint gains a `model.generate` route:

```
POST /v1/model/generate
{
  "model": "models:/llama-3-8b",
  "prompt": "...",
  "max_tokens": 512,
  "temperature": 0.8,
  "stream": true
}
```

Streaming responses use server-sent events with a token-per-event format.

---

## 9. Implementation Phases

### Phase 1 — CPU Reference, Tiny Model

- Operator vtable, CPU/NEON backend skeleton.
- Operators: matmul, gemv, add, mul, scale, rmsnorm, silu, softmax, rope, sdpa, embed_lookup, sample.
- Dtype: f32 only.
- GGUF loader for the f32 path; safetensors loader as a fallback.
- Run TinyLlama-1.1B at f32 end-to-end on aarch64 host (Hyde) and on a QEMU VM.
- Validation: output matches `llama.cpp` reference within numerical tolerance.

**Exit criterion:** `ansh model run models:/tinyllama "Hello"` produces a coherent completion in QEMU.

### Phase 2 — Quantization and SIMD

- Q4_K, Q5_K, Q6_K, Q8_0 GGUF decoders.
- NEON-accelerated matmul/gemv kernels for f32 and quantized paths.
- AVX2 path for x86_64.
- Run Llama-3-8B Q4_K_M on Hyde and on Jekyll under QEMU.

**Exit criterion:** ≥10 tokens/sec on M1 cores at Q4_K_M for an 8B model.

### Phase 3 — KV Cache and Sessions

- KV cache as Tensor Object with layered shape `[layers, 2, heads, ctx, head_dim]`.
- Session lifecycle: create, append tokens, persist, reuse, destroy.
- Streaming generation path.
- `model chat` interactive command.

**Exit criterion:** Multi-turn conversation maintains context coherently across 10+ turns.

### Phase 4 — Backend Abstraction

- Routing plane integration: per-operator engine selection.
- Async dispatch path with fences (lays groundwork for GPU).
- CPU backend as the validated reference; second backend stub registered for testing dispatch logic.

**Exit criterion:** CPU dispatch works through the routing plane with no measurable overhead (<2%) vs direct dispatch.

### Phase 5 — AGX Integration

Depends on RFC-0022 reaching its Phase 5 (compute kernels working). Wires the AGX backend into the operator vtable, validates parity against CPU output for every operator, then activates it as primary backend on M1.

### Phase 6 — XDNA and ANE

XDNA backend on Framework Desktop (drives RFC-0014 §3.2 work). ANE backend on M1 once register documentation is sufficient.

---

## 10. Testing

### 10.1 Operator Parity

Every operator has a parity test: random inputs, run on every backend, compare outputs. Float ops use absolute and relative tolerance (1e-3 / 1e-2 for f16 paths). Reference is always the scalar CPU implementation.

### 10.2 Model Smoke Tests

- TinyLlama-1.1B at f32: deterministic output for fixed prompt + seed.
- Llama-3-8B Q4_K_M: perplexity within 1% of `llama.cpp` baseline on a fixed corpus.

### 10.3 Performance Floors

A backend that passes parity but runs slower than the CPU reference is rejected. Performance floors per backend are documented in `docs/perf-floors.md` and enforced in CI.

---

## 11. Security

- **Model namespace access**: governed by RFC-0008 credential bindings on the model namespace OID. An inference cell without read access to `models:/llama-3-8b` cannot run the model.
- **Sandbox isolation**: the GPU compute plane (RFC-0022) provides per-context isolation. One inference session cannot read another's KV cache through the GPU.
- **Determinism**: sampling RNG is seeded explicitly. f32 paths are deterministic; reduced-precision paths are deterministic per-backend but may differ across backends. This is documented, not papered over.
- **Resource caps**: max context length, max generated tokens, and max concurrent sessions are governed by the cell's admission record (RFC-0003).

---

## 12. Open Questions

1. **Speculative decoding**: cross-backend? (Draft model on CPU, target on GPU.) Promising for latency; deferred until single-backend baseline is solid.
2. **Continuous batching**: serve multiple sessions in parallel through a single GPU dispatch queue. Requires Phase 3 + 4 to land first.
3. **Tokenizer formats beyond BPE**: SentencePiece, Tiktoken, etc. The loader currently assumes BPE-style vocabularies; broader support is a Phase 2 stretch goal.

---

## 13. Relationship to RFC-0013

RFC-0013 says "engines provide the math." RFC-0021 is the engine. Specifically:

| RFC-0013 Concept | RFC-0021 Use |
|---|---|
| Tensor Object | Inputs, outputs, weights, KV cache |
| Model Namespace | Model identity, weight resolution, graph |
| BRIN Summary | Tier placement decisions, validation gating |
| Delta Versioning | LoRA composition at load time |
| Self-Edit Cell | A future training RFC will use this; inference does not modify weights |
| Tensor Engine Capabilities | Backend registration via routing plane |

RFC-0013 is the storage and identity layer. RFC-0021 is the compute layer. They compose without overlap.
