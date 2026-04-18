# AMD XDNA NPU Driver Guide

**Audience**: Kernel developers extending the driver; AI researchers using the NPU from Anunix.  
**File**: `kernel/drivers/xdna.c`  
**Date**: 2026-04-18  

---

## What Is XDNA?

XDNA is AMD's AI Engine (AIE) architecture embedded as a PCI device in Ryzen AI SoCs. It provides dedicated matrix-multiply hardware for ML inference workloads. It is NOT a GPU — it is a separate NPU (Neural Processing Unit) on the same die, with its own register space, firmware, and DMA rings.

The AIE architecture uses a 2D array of "columns," each containing multiple processing elements. A column handles a slice of the computation. More columns = more parallelism = higher throughput for large matrix operations.

From Anunix's perspective, XDNA is an execution engine registered with the Routing Plane. When a task requires `ANX_CAP_TENSOR_NPU`, the routing plane selects the XDNA engine and dispatches the job through the DMA command ring.

---

## Supported Hardware

| Generation | PCI ID | AIE Columns | Performance | SoC |
|-----------|--------|-------------|-------------|-----|
| NPU1 Phoenix / Hawk Point | 1022:1502 | 4 | 16 TOPS INT8 | Ryzen 7040 / 8040 |
| NPU1 Phoenix2 | 1022:1506 | 2 | 8 TOPS INT8 | Ryzen 7040U (low power) |
| NPU2 Rembrandt | 1022:1638 | 4 | 12 TOPS INT8 | Ryzen 6000 |
| NPU4/5 Strix Point | **1022:17f0** | 8 | **50 TOPS INT8** | **Ryzen AI HX 370** |

The Framework Desktop uses the AMD Ryzen AI HX 370 (Strix Point). This is the primary development and target machine for XDNA work. 50 TOPS INT8 is sufficient for running quantized 7B-class language models and smaller vision models entirely on the NPU.

All four device IDs are enumerated in `xdna_devices[]` in `xdna.c`. Adding support for future XDNA generations means adding a new entry to that table (see Section: Extending the Driver).

---

## Driver Architecture

### PCI BAR0

XDNA exposes a 4 MiB MMIO register space via BAR0. The kernel maps this at its physical address (identity-mapped in Anunix's MMIO region). All register access goes through BAR0 offsets.

```c
struct anx_xdna_dev {
    void *bar0;              /* mapped BAR0 base */
    uint64_t bar0_phys;      /* physical address */
    /* ... DMA rings, state, etc. */
};

#define XDNA_REG(dev, off)  ((volatile uint32_t *)((dev)->bar0 + (off)))
#define xdna_read(dev, off)  (*XDNA_REG(dev, off))
#define xdna_write(dev, off, val)  (*XDNA_REG(dev, off) = (val))
```

### PSP Mailbox

The AMD Security Processor (PSP) co-manages the NPU. Firmware loading goes through the PSP mailbox rather than being written directly to NPU registers. This is intentional — AMD uses the PSP to authenticate firmware before execution.

The mailbox protocol:

1. Write firmware command and payload into BAR0 at `XDNA_MBOX_BASE` (512 bytes).
2. Write `1` to `XDNA_MBOX_IPC_TRIGGER` to signal the PSP.
3. Poll `XDNA_MBOX_IPC_STATUS` bit 0. When it clears, the PSP is done.
4. Read response from `XDNA_MBOX_RESP_BASE` (512 bytes).

```c
static int xdna_psp_send(struct anx_xdna_dev *dev,
                          const void *cmd, size_t cmd_len,
                          void *resp, size_t resp_len)
{
    /* Write command */
    memcpy(dev->bar0 + XDNA_MBOX_BASE, cmd, cmd_len);

    /* Trigger PSP */
    xdna_write(dev, XDNA_MBOX_IPC_TRIGGER, 1);

    /* Poll for completion (bit 0 clears when PSP is done) */
    int timeout = 100000;
    while ((xdna_read(dev, XDNA_MBOX_IPC_STATUS) & 1) && --timeout)
        /* spin */;
    if (!timeout)
        return -ANX_ETIMEOUT;

    /* Read response */
    if (resp)
        memcpy(resp, dev->bar0 + XDNA_MBOX_RESP_BASE, resp_len);

    return 0;
}
```

### DMA Rings

Jobs are submitted via a command ring and completed via a completion ring. Each ring entry is 64 bytes.

```
cmd_ring:   [ entry 0 ][ entry 1 ] ... [ entry 63 ]
comp_ring:  [ entry 0 ][ entry 1 ] ... [ entry 63 ]
```

Ring layout is set by writing the physical addresses and entry counts into the DMA registers (see Register Map). The kernel maintains `cmd_head`, `cmd_tail`, `comp_head`:

- **Submit**: write descriptor to `cmd_ring[tail]`, increment tail mod ring_size, write new tail to `XDNA_DMA_CMD_TAIL`.
- **Completion**: poll `comp_ring[comp_head]`. When a completion entry appears (job_id matches), read status, increment comp_head.

Each job descriptor includes:
- `job_id`: 32-bit identifier, echoed back in the completion entry.
- Input/output buffer physical addresses and sizes.
- Opcode identifying the operation type.

### Engine Registration

On successful probe, the driver registers with the Anunix engine subsystem:

```c
anx_engine_register(&(struct anx_engine_desc){
    .name        = "amd-xdna",
    .type        = ANX_ENGINE_DEVICE_SERVICE,
    .caps        = ANX_CAP_TENSOR_NPU |
                   ANX_CAP_TENSOR_INT8 |
                   ANX_CAP_TENSOR_FP32,
    .quality     = 90,
    .submit      = anx_xdna_submit,
    .priv        = dev,
});
```

`quality_score = 90` is higher than the CPU tensor path, so the routing plane prefers XDNA for NPU-capable tensor jobs when firmware is loaded and the engine is AVAILABLE.

---

## Firmware

### Why Firmware Is Required

The AIE cores are not directly programmable from the host side without firmware. The PSP loads and authenticates the firmware image, then the firmware sets up the AIE column microcontrollers, DMA configuration, and the command ring protocol. Without it, the hardware is present but cannot accept jobs.

### Getting the Firmware

AMD distributes NPU firmware through the `linux-firmware` repository:

```sh
# On a Linux host (or macOS with git)
git clone https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git
ls linux-firmware/amdnpu/
# You will see files like: 1502_00.sbin, 17f0_10.sbin, etc.
# The prefix matches the PCI device ID (17f0 = Strix Point / Ryzen AI HX 370)
```

The `.sbin` files are signed binary blobs. They are redistributable under AMD's firmware license. Do not modify them — the PSP validates the signature.

### Loading Firmware into Anunix

**Method 1 — Credential store (preferred)**:

```sh
# On the Anunix shell or via HTTP API
secret set xdna-firmware $(xxd -p -c 0 linux-firmware/amdnpu/17f0_10.sbin)
xdna load
```

**Method 2 — Boot command line**:

```
-append "cred:xdna-firmware=<hex-encoded blob>"
```

The hex-encoded blob is the `.sbin` file contents as a lowercase hex string. This is useful for automated testing where you want firmware loaded before the shell starts.

### What Happens on `xdna load`

1. `anx_xdna_load_firmware()` is called.
2. Reads firmware bytes from the credential store (`secret get xdna-firmware`).
3. Builds a firmware load command in the PSP mailbox format.
4. Calls `xdna_psp_send()` to transfer the firmware via PSP mailbox.
5. PSP authenticates the signature, loads firmware into NPU SRAM.
6. On success, engine state transitions: `REGISTERED` → `AVAILABLE`.
7. The routing plane can now dispatch tensor jobs to XDNA.

Until `xdna load` succeeds, the engine is `REGISTERED` but not `AVAILABLE`. Submitted jobs fail with `ANX_EUNAVAIL`.

---

## Programming Model

### Submitting a Job

```c
int anx_xdna_submit(struct anx_engine *eng, struct anx_tensor_job *job)
{
    struct anx_xdna_dev *dev = eng->priv;
    struct xdna_cmd_entry *ent;
    uint32_t tail;

    tail = dev->cmd_tail;
    ent  = &dev->cmd_ring[tail];

    ent->job_id  = job->id;
    ent->opcode  = XDNA_OP_MATMUL;
    ent->src_a   = job->buf_a_phys;
    ent->src_b   = job->buf_b_phys;
    ent->dst     = job->buf_out_phys;
    ent->len_m   = job->m;
    ent->len_n   = job->n;
    ent->len_k   = job->k;
    ent->dtype   = job->dtype;
    memset(ent->_pad, 0, sizeof(ent->_pad));

    dev->cmd_tail = (tail + 1) % dev->ring_size;
    xdna_write(dev, XDNA_DMA_CMD_TAIL, dev->cmd_tail);

    return 0;
}
```

### Polling for Completion

```c
int anx_xdna_poll(struct anx_xdna_dev *dev, uint32_t job_id, int *status_out)
{
    struct xdna_comp_entry *ent = &dev->comp_ring[dev->comp_head];

    /* Completion ring entries are zeroed; job_id != 0 means a result is ready */
    if (ent->job_id != job_id)
        return -ANX_EAGAIN;

    *status_out = ent->status;
    ent->job_id = 0;  /* consume */
    dev->comp_head = (dev->comp_head + 1) % dev->ring_size;
    return 0;
}
```

The routing plane calls `anx_xdna_submit()` and then polls `anx_xdna_poll()` in its completion loop. A future IRQ-driven path will replace polling when XDNA interrupt routing is confirmed working on real hardware.

---

## Shell Commands

```
xdna
```

Displays NPU information:

```
XDNA NPU — Strix Point (1022:17f0)
  BAR0:     0xf0000000  (4 MiB)
  AIE cols: 8
  TOPS:     50 (INT8)
  Firmware: LOADED
  Engine:   AVAILABLE
```

```
xdna load
```

Loads firmware from the credential store via PSP mailbox. Prints the result:

```
xdna: loading firmware from credential store...
xdna: PSP mailbox command sent
xdna: firmware loaded OK — engine AVAILABLE
```

Or on failure:

```
xdna: no firmware found in credential store (key: xdna-firmware)
xdna: use: secret set xdna-firmware <hex>
```

---

## Register Map

All offsets are from BAR0 base. Registers are 32-bit unless noted.

| Offset | Name | Description |
|--------|------|-------------|
| 0x000 | `XDNA_VERSION` | Hardware version register |
| 0x004 | `XDNA_STATUS` | Device status bits (bit 0 = ready) |
| 0x100 | `XDNA_MBOX_BASE` | PSP mailbox command buffer (512 bytes) |
| 0x300 | `XDNA_MBOX_RESP_BASE` | PSP mailbox response buffer (512 bytes) |
| 0x500 | `XDNA_MBOX_IPC_TRIGGER` | Write 1 to send command to PSP |
| 0x504 | `XDNA_MBOX_IPC_STATUS` | Bit 0: PSP busy (1 = busy, 0 = done) |
| 0x600 | `XDNA_AIE_COLS` | Number of AIE columns (read-only) |
| 0x1000 | `XDNA_DMA_CMD_BASE_LO` | Command ring physical address [31:0] |
| 0x1004 | `XDNA_DMA_CMD_BASE_HI` | Command ring physical address [63:32] |
| 0x1008 | `XDNA_DMA_CMD_SIZE` | Command ring entry count |
| 0x100C | `XDNA_DMA_CMD_HEAD` | Command ring head pointer (hardware advances) |
| 0x1010 | `XDNA_DMA_CMD_TAIL` | Command ring tail pointer (kernel writes) |
| 0x1020 | `XDNA_DMA_COMP_BASE_LO` | Completion ring physical address [31:0] |
| 0x1024 | `XDNA_DMA_COMP_BASE_HI` | Completion ring physical address [63:32] |
| 0x1028 | `XDNA_DMA_COMP_SIZE` | Completion ring entry count |
| 0x102C | `XDNA_DMA_COMP_HEAD` | Completion ring head (kernel advances) |
| 0x1030 | `XDNA_DMA_COMP_TAIL` | Completion ring tail (hardware advances) |

These offsets are from the XDNA driver implementation and linux-firmware source analysis. If a future XDNA generation changes the layout, the offsets need to be updated per device ID in `xdna_devices[]`.

---

## Extending the Driver

### Adding a New Device ID

Add an entry to `xdna_devices[]` in `xdna.c`:

```c
static const struct xdna_device_info xdna_devices[] = {
    { 0x1022, 0x1502, "Phoenix/Hawk Point", 4, 16 },
    { 0x1022, 0x1506, "Phoenix2",           2,  8 },
    { 0x1022, 0x1638, "Rembrandt",          4, 12 },
    { 0x1022, 0x17f0, "Strix Point",        8, 50 },
    /* Add new entry here: { vendor, device, name, aie_cols, tops } */
    { 0, 0, NULL, 0, 0 }  /* terminator */
};
```

Also add the PCI ID to `RFC-0014-hardware-platform.md` and the hardware support matrix.

### Adding GPU Support

The GPU is a separate device with a completely different register layout. It should be a separate driver (`radeon.c` or `nvrtx.c`), not an extension of `xdna.c`. The XDNA and GPU register spaces do not overlap. The GPU registers as a different capability:

```c
.caps = ANX_CAP_RENDER | ANX_CAP_TENSOR_GPU | ANX_CAP_TENSOR_FP16,
```

### NUMA / Partition Support

The XDNA hardware supports partitioning the AIE array into multiple logical partitions, each handled independently. This allows concurrent jobs to run on disjoint sets of columns. Current Anunix implementation uses the full array as a single partition. To add partition support:

1. Probe `XDNA_AIE_COLS` and divide into N partitions (configurable).
2. Allocate one command ring and completion ring per partition.
3. Register N engine instances, each with a subset of the AIE columns.
4. The routing plane will load-balance across partitions.

This is particularly relevant for long-running background inference workloads that should not block interactive tensor jobs.
