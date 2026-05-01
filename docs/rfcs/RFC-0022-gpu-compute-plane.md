# RFC-0022: GPU Compute Plane and AGX Driver

| Field      | Value                                                     |
|------------|-----------------------------------------------------------|
| RFC        | 0022                                                      |
| Title      | GPU Compute Plane and AGX Driver                          |
| Author     | Adam Pippert                                              |
| Status     | Draft                                                     |
| Created    | 2026-05-01                                                |
| Updated    | 2026-05-01                                                |
| Depends On | RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0007, RFC-0014, RFC-0021 |

---

## Executive Summary

RFC-0014 catalogues the hardware Anunix targets and lists Apple AGX, AMD Radeon, and NVIDIA RTX as future GPU work. This RFC defines the **GPU Compute Plane** — the kernel subsystem that provides a uniform interface to GPU compute hardware — and specifies the **AGX driver** as its first implementation.

The plane is compute-only by intent. Anunix has a framebuffer (RFC-0014 §2.1) and a future Display Coprocessor (DCP) RFC will handle accelerated display. The GPU compute plane handles the other half: running shader programs on tensor data. ML inference (RFC-0021) is the primary consumer; classical GPGPU workloads are a secondary one.

The plane defines a driver vtable, GPU contexts (per-cell isolated address spaces), GPU buffers (memory allocations visible to a context), command streams (the submission interface), compute kernels (loaded shader binaries), and fences (completion events). Every GPU driver implements this interface.

The AGX driver is reimplemented from first principles in C and aarch64 assembly. The Asahi Linux project's reverse-engineering work and the `applegpu` ISA documentation are reference material — every line is rewritten to comply with Anunix's no-C++/no-Rust rule and to fit the plane's interface. The driver targets compute only; graphics is out of scope for this RFC.

---

## 1. Problem Statement

### 1.1 No Subsystem Owns GPU Compute

Anunix has drivers for the framebuffer, virtio devices, NVMe, and the AMD XDNA NPU. None of these patterns extends cleanly to GPUs. GPUs require:

- A second MMU (the GPU's own page tables) managed in lockstep with the CPU's.
- A firmware lifecycle (most modern GPUs run firmware on internal cores).
- An asynchronous command submission model with deep queues and fence-based synchronization.
- Per-process address space isolation enforced by the GPU MMU.

These do not fit into "yet another PCI driver." They need a subsystem.

### 1.2 RFC-0021 Needs a GPU Backend

RFC-0021 §3 shows the GPU backend as a peer of CPU/NEON. For that backend to exist, the kernel needs an interface to talk to GPUs. Building one ad-hoc inside the AGX driver locks the design to AGX. Building one as a plane lets Radeon, RTX, and Mali backends slot in cleanly later.

### 1.3 AGX Is Not Like Other GPUs

Apple's GPU is integrated, uses a unified-memory model, has its own MMU (UAT), runs firmware on a coprocessor, and has no public documentation. The Asahi Linux project has reverse-engineered enough to write a working DRM driver, but their code is GPL Linux kernel C plus userspace components in C++ (Mesa) and Rust (recent driver work). Anunix needs a C reimplementation that fits Anunix's primitives.

---

## 2. Goals

### 2.1 Primary Goals

1. **Define a GPU compute plane.** A driver vtable, a context model, a buffer model, a command submission model, and a fence model. Every GPU driver targets this interface.
2. **Compute-only scope.** No graphics pipeline state, no rasterizer, no texture sampling beyond what compute kernels need. Display is RFC-0014's framebuffer or a future DCP RFC.
3. **Per-cell isolation.** GPU contexts are scoped to Execution Cells (RFC-0003). One cell's GPU memory is unreachable from another cell. Enforced by the GPU's MMU.
4. **AGX driver in pure C/ASM.** Reimplemented from first principles using Asahi/applegpu/m1n1 as reference material. No C++, no Rust, no GPL code copied verbatim.
5. **Engine registration.** GPU drivers register through the Routing Plane (RFC-0005) with tensor capabilities matching RFC-0013 §10.1.
6. **Deterministic teardown.** A cell exiting (cleanly or otherwise) frees all of its GPU resources. No leaks, no zombie command streams, no orphaned firmware queues.

### 2.2 Non-Goals

- **Graphics rendering.** No vertex shaders, no rasterization, no display pipeline. RFC-0021 needs compute, period.
- **Cross-GPU memory sharing.** Buffers belong to one driver; transfer between drivers goes through CPU memory.
- **GPU-driven scheduling.** The kernel scheduler stays authoritative. GPU work is dispatched, not autonomous.
- **A shading language compiler.** Hand-written shaders are sufficient for the initial operator set. A compiler is a separate, future RFC.

---

## 3. Architecture

```
                  ┌───────────────────────────────────────┐
                  │       RFC-0021 Inference Runtime      │
                  │       (GPU backend uses this plane)   │
                  └─────────────────┬─────────────────────┘
                                    │
                  ┌─────────────────▼─────────────────────┐
                  │         GPU Compute Plane             │
                  │   anx_gpu_device · anx_gpu_context    │
                  │   anx_gpu_buffer · anx_gpu_kernel     │
                  │   anx_gpu_cmdstream · anx_gpu_fence   │
                  └────┬────────────┬────────────┬────────┘
                       │            │            │
              ┌────────▼─┐    ┌─────▼────┐  ┌────▼─────┐
              │ AGX      │    │ Radeon   │  │ RTX      │
              │ (M1)     │    │ (later)  │  │ (later)  │
              └──────────┘    └──────────┘  └──────────┘
```

The plane defines opaque types and a driver vtable. Drivers implement the vtable against their hardware. RFC-0021 (and any other compute consumer) talks only to the plane.

---

## 4. Plane Definitions

### 4.1 Device

```c
struct anx_gpu_device {
    const struct anx_gpu_driver *drv;
    void *drv_state;                     /* driver-private */

    /* identity */
    char        name[64];                /* "Apple M1 GPU (8 cores)" */
    uint32_t    vendor_id;
    uint32_t    device_id;

    /* capabilities */
    uint64_t    total_memory;            /* bytes addressable */
    uint32_t    compute_units;           /* SMs / cores / clusters */
    uint32_t    supported_dtypes;        /* bitmask of anx_tensor_dtype */
    uint64_t    max_buffer_bytes;
    uint32_t    max_concurrent_contexts;
};
```

A GPU device is created at driver probe and lives for the life of the kernel. Cells do not own devices; they own contexts on a device.

### 4.2 Context

```c
struct anx_gpu_context {
    struct anx_gpu_device *dev;
    void *drv_state;
    anx_cid_t   owner_cell;              /* RFC-0003 cell id */
    uint64_t    memory_used;
    uint32_t    active_cmdstreams;
};
```

A context is an isolated GPU address space. Buffers and kernels are allocated within a context; nothing crosses contexts. The driver enforces this through the GPU MMU.

### 4.3 Buffer

```c
enum anx_gpu_buffer_kind {
    ANX_GPU_BUF_DEVICE,    /* GPU-only memory */
    ANX_GPU_BUF_SHARED,    /* CPU+GPU mapped, unified memory */
    ANX_GPU_BUF_HOST,      /* CPU memory, GPU-readable via DMA */
};

struct anx_gpu_buffer {
    struct anx_gpu_context *ctx;
    void *drv_state;
    uint64_t    size;
    enum anx_gpu_buffer_kind kind;
    void       *host_ptr;                /* valid for SHARED and HOST */
    uint64_t    gpu_addr;                /* GPU virtual address */
};
```

On AGX (and other unified-memory architectures), `ANX_GPU_BUF_SHARED` is the cheap path — CPU and GPU see the same physical memory through different page tables. On discrete GPUs (Radeon, RTX), `SHARED` falls back to BAR-mapped or coherent memory; `DEVICE` is preferred for hot data.

### 4.4 Kernel

```c
struct anx_gpu_kernel {
    struct anx_gpu_context *ctx;
    void *drv_state;
    char        name[64];
    uint32_t    arg_count;
    /* binary code is driver-private; this is just a handle */
};
```

A GPU kernel is a loaded shader binary. The plane does not specify the binary format — that's per-driver. AGX kernels are AGX bytecode (see §6.5).

### 4.5 Command Stream

```c
struct anx_gpu_cmdstream {
    struct anx_gpu_context *ctx;
    void *drv_state;
};

enum anx_gpu_cmd_kind {
    ANX_GPU_CMD_DISPATCH,        /* run a kernel */
    ANX_GPU_CMD_COPY_BUFFER,     /* device-side memcpy */
    ANX_GPU_CMD_FILL_BUFFER,     /* device-side memset */
    ANX_GPU_CMD_BARRIER,         /* memory barrier */
};

struct anx_gpu_dispatch_args {
    struct anx_gpu_kernel *kernel;
    struct anx_gpu_buffer *args[16];
    uint32_t arg_count;
    uint32_t grid[3];                    /* threadgroups */
    uint32_t block[3];                   /* threads per group */
};
```

Command streams are built host-side, then submitted. Building is cheap; submission triggers the doorbell to the GPU firmware.

### 4.6 Fence

```c
typedef uint64_t anx_gpu_fence_t;
```

Fences are monotonically-increasing 64-bit values per context. Submission returns the fence value the GPU will write on completion. Waiting is busy-poll for short waits, IRQ-driven for long ones.

### 4.7 Driver Vtable

```c
struct anx_gpu_driver {
    const char *name;

    /* lifecycle */
    int  (*probe)(void);
    int  (*device_init)(struct anx_gpu_device *dev);
    void (*device_fini)(struct anx_gpu_device *dev);

    /* contexts */
    int  (*context_create)(struct anx_gpu_device *dev, anx_cid_t owner,
                           struct anx_gpu_context **out);
    void (*context_destroy)(struct anx_gpu_context *ctx);

    /* buffers */
    int  (*buffer_alloc)(struct anx_gpu_context *ctx, uint64_t size,
                         enum anx_gpu_buffer_kind kind,
                         struct anx_gpu_buffer **out);
    void (*buffer_free)(struct anx_gpu_buffer *buf);
    int  (*buffer_sync)(struct anx_gpu_buffer *buf, bool to_gpu);

    /* kernels */
    int  (*kernel_load)(struct anx_gpu_context *ctx, const void *binary,
                        uint64_t binary_size, const char *entrypoint,
                        struct anx_gpu_kernel **out);
    void (*kernel_unload)(struct anx_gpu_kernel *k);

    /* command streams */
    int  (*cmdstream_create)(struct anx_gpu_context *ctx,
                             struct anx_gpu_cmdstream **out);
    int  (*cmdstream_dispatch)(struct anx_gpu_cmdstream *cs,
                               const struct anx_gpu_dispatch_args *args);
    int  (*cmdstream_copy)(struct anx_gpu_cmdstream *cs,
                           struct anx_gpu_buffer *dst,
                           struct anx_gpu_buffer *src,
                           uint64_t bytes);
    int  (*cmdstream_barrier)(struct anx_gpu_cmdstream *cs);
    int  (*cmdstream_submit)(struct anx_gpu_cmdstream *cs,
                             anx_gpu_fence_t *fence_out);
    void (*cmdstream_destroy)(struct anx_gpu_cmdstream *cs);

    /* fences */
    int  (*fence_wait)(struct anx_gpu_context *ctx, anx_gpu_fence_t fence,
                       uint64_t timeout_ns);
    int  (*fence_check)(struct anx_gpu_context *ctx, anx_gpu_fence_t fence,
                        bool *signaled_out);
};
```

---

## 5. Plane Operations

### 5.1 Driver Registration

```c
int anx_gpu_register_driver(const struct anx_gpu_driver *drv);
```

Drivers register at module init. The plane calls `probe` to discover devices and `device_init` per device.

### 5.2 Cell-Scoped Context Lifecycle

```c
/* Get or create a GPU context for the calling cell. */
int anx_gpu_context_for_cell(struct anx_gpu_device *dev,
                             struct anx_gpu_context **out);
```

The plane tracks contexts per cell. When a cell exits, the plane calls `context_destroy` for every context it holds. This is the deterministic teardown contract.

### 5.3 Engine Registration

GPU drivers register an inference engine with the Routing Plane (RFC-0005) using RFC-0013 §10.1 tensor capabilities. The engine vtable internally translates RFC-0021 operator dispatches into GPU command streams.

---

## 6. AGX Driver

### 6.1 Hardware Surface

AGX is the GPU on Apple Silicon SoCs. Public reference material:

- **m1n1**: bootloader and reverse-engineering tooling. Source: `https://github.com/AsahiLinux/m1n1`. Definitive for boot-time hardware state, AIC, PMGR, and early MMIO.
- **applegpu**: AGX ISA documentation. Source: `https://github.com/dougallj/applegpu`. Primary reference for shader bytecode encoding.
- **Asahi DRM driver**: working Linux driver. Source: `drivers/gpu/drm/asahi/` in the Asahi kernel tree. Reference for firmware queues, UAT, command submission protocol.
- **Mesa AGX backend**: userspace shader compiler. Source: `mesa/src/asahi/`. Reference for the kernel ABI and shader binary layout.

These are read for understanding; nothing is copied. Anunix's AGX driver is independently authored C/ASM.

### 6.2 Components

```
kernel/drivers/gpu/agx/
    agx_main.c            /* driver registration, probe */
    agx_device.c          /* device_init, AGX MMIO mapping */
    agx_firmware.c        /* firmware extraction, load, handshake */
    agx_uat.c             /* Unified Address Translation (GPU MMU) */
    agx_context.c         /* per-cell GPU contexts */
    agx_buffer.c          /* allocation, mapping, sync */
    agx_kernel.c          /* shader binary load, validation */
    agx_cmdstream.c       /* command buffer build, submit */
    agx_fwq.c             /* firmware queue protocol */
    agx_irq.c             /* interrupt handling */
    agx_isa.h             /* AGX ISA opcode definitions */
    agx_asm.c             /* AGX assembler (text → bytecode) */

kernel/drivers/gpu/agx/shaders/
    matmul_f16.agx        /* hand-written shader source */
    matmul_q4k.agx
    rmsnorm_f16.agx
    softmax_f16.agx
    rope_f16.agx
    sdpa_f16.agx
    ...
```

### 6.3 Boot-Time Dependencies

The AGX driver needs platform infrastructure that does not yet exist on bare-metal Apple Silicon:

- **AIC (Apple Interrupt Controller)**: required for IRQ delivery. Currently RFC-0014 §7 future work. **Blocker for AGX on bare metal; not blocking under UTM.**
- **PMGR (Power Management)**: GPU clock and power domains must be enabled before AGX MMIO is accessible. Currently RFC-0014 §4.2 future work. **Blocker.**
- **Apple Device Tree (ADT)**: AGX MMIO addresses come from the ADT, not the standard FDT. Asahi has a parser; we write our own. **Blocker.**

These three items are tracked as RFC-0014 amendments and must land before AGX driver Phase 2.

### 6.4 UAT (GPU MMU)

AGX uses a 4-level page table walked by the GPU. Layout is similar to ARMv8 stage-1 with quirks. The driver:

- Allocates a TTBR (translation table base) per context.
- Maintains parallel page tables for CPU and GPU views of `ANX_GPU_BUF_SHARED` buffers, with the same physical backing.
- Invalidates GPU TLBs on unmap via the firmware queue.
- Pins kernel-side memory as needed; no swapping under the GPU.

### 6.5 Firmware

AGX runs firmware on an internal coprocessor. The firmware is provided by Apple and is not redistributable. Two paths:

1. **Bootstrap path (Phase 1)**: extract the firmware from a macOS installation, store it as a blob in `/lib/firmware/agx/`, load it during `device_init`. The user provides the blob; Anunix loads it.
2. **Long-term path**: investigate whether a stub firmware can drive a useful subset of AGX. Asahi has not pursued this seriously; it is a research item, not a Phase 1 dependency.

Firmware is loaded into a designated physical region, the GPU is brought out of reset, a handshake message is exchanged via the firmware queue, and the firmware reports ready.

### 6.6 Shader Toolchain

The AGX assembler in `agx_asm.c` translates a textual AGX assembly format into AGX bytecode. The format mirrors `applegpu`'s disassembler output. Shaders for the operator set are hand-written in this format and assembled at build time, producing `.agxbin` files in `build/agx/shaders/`.

A general shader compiler (NIR → AGX) is explicitly out of scope. Hand-written shaders for ~20 operators is tractable and yields better performance than naive compilation. New operators added later require new hand-written shaders, not a compiler.

### 6.7 Command Submission

The driver builds a command buffer (a structured byte stream the GPU firmware parses), writes it to a per-context ring, and rings the doorbell (an MMIO write). The firmware processes commands in order, updates the fence value when each completes, and raises an IRQ if the host has requested it.

### 6.8 Engine Registration

AGX registers as `anxml-agx` with capabilities:

```c
struct anx_op_engine_registration agx_engine = {
    .name = "anxml-agx",
    .supported_ops = /* every operator with an AGX shader */,
    .supported_dtypes = ANX_CAP_TENSOR_F16 | ANX_CAP_TENSOR_BF16
                      | ANX_CAP_TENSOR_INT8 | ANX_CAP_TENSOR_INT4,
    .max_tensor_bytes = (1ULL << 32),
    .accelerator = "agx",
    .vtable = &agx_op_vtable,
};
```

The AGX op vtable wraps GPU plane primitives: dispatch a kernel, copy buffers, fence wait.

---

## 7. Implementation Phases

### Phase 1 — Plane Skeleton

- Plane types, vtable, opaque structs (`kernel/core/gpu/`).
- A null driver that registers, supports context create/destroy, and rejects all real operations. Used as a smoke test for the plane interface.
- Engine registration through the routing plane (RFC-0005).

**Exit criterion:** A cell can create a GPU context against the null driver and tear it down cleanly.

### Phase 2 — AGX Bringup (UTM)

Depends on UTM presenting an AGX device through Apple Virtualization.framework. Initial validation under UTM avoids blocking on bare-metal AIC/PMGR.

- Apple Device Tree parser (shared with platform code).
- AGX MMIO mapping.
- Firmware blob loader and handshake.
- Sanity test: read GPU identity registers and confirm firmware ready state.

**Exit criterion:** AGX driver successfully completes firmware handshake under UTM.

### Phase 3 — UAT and Memory

- UAT page table management.
- `buffer_alloc` for `ANX_GPU_BUF_SHARED` (the unified-memory hot path).
- `buffer_alloc` for `ANX_GPU_BUF_DEVICE` (GPU-only).
- Cross-context isolation test: cell A cannot see cell B's buffers.

**Exit criterion:** Allocate a 1 GB buffer, write from CPU, read back from GPU through a no-op shader.

### Phase 4 — Command Submission

- Command buffer builder.
- Firmware queue and doorbell.
- Fence path including IRQ handler (depends on AIC for bare metal; UTM uses GICv2 shim).
- Trivial compute kernel: write a constant to a buffer.

**Exit criterion:** Submit a no-op kernel, fence-wait for completion, verify result.

### Phase 5 — First Real Kernel

- AGX assembler.
- Hand-written matmul shader (f16, 16x16 tile baseline).
- Validate against RFC-0021 CPU reference.

**Exit criterion:** GPU matmul matches CPU output within tolerance, on tensors up to 4096×4096.

### Phase 6 — Operator Coverage

Hand-written shaders for the remaining RFC-0021 operators: gemv, rmsnorm, silu, softmax, rope, sdpa, embed_lookup. Each lands with a parity test against the CPU backend.

**Exit criterion:** RFC-0021 Phase 5 exit — Llama-3-8B inference runs on AGX with parity vs CPU.

### Phase 7 — Bare Metal

Once AIC, PMGR, ADT parsing, and AGX driver are working under UTM, repeat the bringup on bare metal Apple Silicon. The expected delta is small but non-trivial (real IRQ wiring, real clock enables, real firmware extraction).

**Exit criterion:** M1 Mac Studio boots Anunix from local storage and runs `ansh model run` on AGX backend.

---

## 8. Testing

### 8.1 Plane Tests

Plane tests use the null driver:

- Context create/destroy lifecycle.
- Buffer alloc respects size.
- Per-cell isolation enforced.
- Resources freed on cell exit.

### 8.2 AGX Conformance

- Firmware handshake completes within timeout.
- UAT round-trip: map a buffer, walk the page tables, confirm GPU view matches CPU view.
- Fence ordering: submit N command streams, verify fence values monotonically increase.
- Operator parity: every shader matches CPU output within tolerance (per RFC-0021 §10.1).

### 8.3 Stress

- 1000 cells, each with a context, each running a small dispatch. Verify no leaks, no fence collisions.
- Long-running session (8B model, 100k tokens) — no firmware queue stalls, no UAT TLB drift.

---

## 9. Security

- **Per-cell isolation**: enforced by UAT. A cell cannot map another cell's GPU memory. This is the same guarantee as the CPU MMU provides for processes.
- **Firmware blob trust**: the AGX firmware is opaque and Apple-signed. Loaded firmware is treated as trusted code with full GPU access. This is a known risk inherited from the architecture; it is not unique to Anunix.
- **Resource caps**: max GPU memory per cell, max concurrent contexts per device. Enforced at `buffer_alloc` and `context_create`.
- **No GPU-driven IO**: the GPU cannot initiate DMA to arbitrary host memory. All buffers are pre-mapped through the plane.

---

## 10. Open Questions

1. **DCP (Display Coprocessor)**: separate RFC. Compute and display share the GPU silicon but the firmware paths are distinct. Keeping them in separate RFCs avoids coupling.
2. **ANE as a GPU plane peer?** ANE is fixed-function, not programmable like AGX. It probably belongs in a sibling "Accelerator Plane" rather than the GPU plane. To be decided when ANE driver work begins.
3. **Multi-GPU**: phone vs. cluster (RFC-0013 §3.1.8) implies eventually scaling to multiple GPUs. The plane supports multiple devices but the AGX driver does not (M1 has one GPU). Cross-device coordination is deferred until Radeon/RTX work makes it relevant.
4. **Firmware reverse engineering**: writing our own firmware would be an enormous research project and may not be feasible. Treated as exploratory; not a Phase 1 commitment.

---

## 11. Relationship to Other RFCs

| RFC | Relationship |
|---|---|
| RFC-0014 | Defines hardware targets; this RFC is the architecture for one class (GPU compute) |
| RFC-0013 | GPU buffers back tensor objects when placed on GPU; tier placement integrates with Memory Plane |
| RFC-0021 | Primary consumer; the AGX backend is an inference engine running on this plane |
| RFC-0005 | Engine registration and per-operator dispatch |
| RFC-0003 | Cells own GPU contexts; cell teardown drives plane teardown |
| RFC-0007 | Capability objects gate access to GPU devices for restricted cells |
