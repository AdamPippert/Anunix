# RFC-0015: Kernel Network Data Plane

**Status**: Draft  
**Author**: Adam Pippert  
**Created**: 2026-04-20  
**Depends on**: RFC-0006 (Network Plane), RFC-0004 (Memory Control Plane), RFC-0007 (Capability Objects)

---

## Abstract

RFC-0015 specifies the Kernel Network Data Plane: a zero-copy, poll-mode packet processing layer competitive with DPDK-class I/O for workloads that need predictable, low-latency throughput from inside Anunix.

The design introduces DMA-safe packet arenas, a typed `anx_pkt` frame object, multi-queue NIC abstractions, flow objects that carry per-connection state, and fast-path execution cells that can be installed into the data path via the capability system.

---

## Motivation

RFC-0006 defines the Network Plane at the protocol and address level — DHCP, ARP, IPv4, TCP/UDP, the network stack.  That layer is correct but not high-throughput: every received frame copies into the stack's private buffers, and every transmitted frame copies back out.

For Anunix to host AI agents that forward large model weights, stream inference results, or act as edge routers, the kernel needs a data plane that can sustain multi-gigabit flows without per-packet allocation and without the overhead of the interrupt-driven model.

Target: **≥10 Gbps line rate** on a single core for 1500-byte frames using the MT7925 (Wi-Fi 7) or a future 10 GbE NIC, matching or exceeding what Linux XDP achieves on equivalent hardware.

---

## Design

### 5.1 DMA Packet Arenas (`ANX_MEM_NET_DMA`)

All network-bound memory is allocated from arenas declared as `ANX_MEM_NET_DMA`:

```c
struct anx_pkt_arena {
    uintptr_t  pa_base;     /* physical base (identity-mapped = virt base) */
    uint32_t   frame_size;  /* bytes per frame slot */
    uint32_t   n_frames;    /* total slots */
    uint8_t   *data;        /* virtual address of frame array */
    uint32_t  *free_stack;  /* lock-free LIFO of free slot indices */
    uint32_t   free_top;    /* top of free stack (atomic) */
};
```

Key properties:
- All frames contiguous in physical memory (one `anx_page_alloc(order)` call)
- Physical == virtual in Anunix identity map — no ioremap, no DMA mapping
- Frame indices are the currency; pointers are computed as `base + idx * frame_size`
- Zero-copy: NIC descriptor points directly into the arena; stack reads from the same buffer

Arena management API:
```c
struct anx_pkt_arena *anx_pkt_arena_create(uint32_t n, uint32_t frame_sz);
uint32_t anx_pkt_arena_alloc(struct anx_pkt_arena *a);  /* returns index or UINT32_MAX */
void     anx_pkt_arena_free(struct anx_pkt_arena *a, uint32_t idx);
void    *anx_pkt_arena_ptr(struct anx_pkt_arena *a, uint32_t idx);
```

### 5.2 Typed Packet Object (`anx_pkt`)

Every frame in flight is described by a typed packet object:

```c
struct anx_pkt {
    uint32_t  arena_idx;    /* slot index in owning arena */
    uint16_t  len;          /* populated bytes */
    uint16_t  headroom;     /* bytes reserved at start of frame for prepend */
    uint16_t  data_off;     /* offset of first data byte from frame base */
    uint8_t   flags;        /* ANX_PKT_F_* */
    uint8_t   queue_id;     /* which NIC queue this arrived on */
    uint32_t  flow_id;      /* flow table lookup result (0 = uncached) */
    uint64_t  timestamp;    /* monotonic arrival time (ns) */
};

#define ANX_PKT_F_TX_COMPLETE   0x01  /* TX done — arena slot may be recycled */
#define ANX_PKT_F_NEEDS_CSUM    0x02  /* HW checksum offload requested */
#define ANX_PKT_F_L3_VALID      0x04  /* IP header parsed and valid */
```

Packet ownership is explicit:
- **NIC → kernel**: NIC fills arena slot, creates `anx_pkt`, hands to poll function
- **kernel → stack/cell**: poll function calls registered handler, transfers ownership
- **stack/cell → NIC**: caller calls `anx_pkt_release()` or `anx_net_tx(pkt)`, never both

### 5.3 Multi-Queue NIC Abstraction

Each hardware queue pair registers as:

```c
struct anx_netq {
    uint32_t   id;
    /* poll for received packets; returns number drained */
    int      (*poll)(struct anx_netq *q, struct anx_pkt **pkts, uint32_t max);
    /* transmit batch; caller retains ownership of unconsumed packets */
    int      (*tx_batch)(struct anx_netq *q,
                         struct anx_pkt **pkts, uint32_t n);
    struct anx_pkt_arena *rx_arena;
    struct anx_pkt_arena *tx_arena;
    void      *priv;
};
```

The MT7925 driver (single hardware queue) registers one `anx_netq`.  Future multi-queue NICs register one per queue, and the scheduler assigns cells to queues by NUMA or affinity.

### 5.4 Flow Objects

A flow object caches per-connection state so the fast path avoids hash lookups for established connections:

```c
struct anx_flow {
    uint32_t  src_ip, dst_ip;
    uint16_t  src_port, dst_port;
    uint8_t   proto;          /* IPPROTO_TCP / IPPROTO_UDP */
    uint8_t   flags;
    /* Fast-path cell installed for this flow (NULL = use default stack) */
    struct anx_cell *fast_cell;
    /* TX queue pinned for this flow */
    struct anx_netq *tx_queue;
    /* Bytes and packets since last reset */
    uint64_t  rx_bytes, tx_bytes;
    uint64_t  rx_pkts,  tx_pkts;
};
```

Flow table: open-addressed hash, 65536 buckets, lock-free for reads.

```c
struct anx_flow *anx_flow_lookup(uint32_t src_ip, uint32_t dst_ip,
                                  uint16_t sport, uint16_t dport,
                                  uint8_t proto);
struct anx_flow *anx_flow_insert(const struct anx_flow *template);
void             anx_flow_evict(struct anx_flow *f);
```

### 5.5 Fast-Path Execution Cells

A fast-path cell is a capability-installed Execution Cell (RFC-0003) that processes packets in the data path instead of handing them to the protocol stack:

```c
/* Install a cell as the fast-path handler for a flow */
int anx_netq_install_cell(struct anx_netq *q,
                           struct anx_flow *f,
                           struct anx_cell *cell);
```

Use cases:
- **L3 forwarding**: cell reads `anx_pkt`, rewrites Ethernet header, calls `anx_net_tx()`
- **Packet capture**: cell copies to a ring buffer State Object for userland
- **Throttle / drop**: cell enforces rate limits and drops excess packets in-place

Installed cells bypass the ARP/IPv4/TCP layers entirely — they see raw Ethernet frames and are responsible for all processing.

### 5.6 Poll-Mode Integration

The main loop (RFC-0005 scheduler) calls `anx_netq_run()` each tick:

```c
void anx_netq_run(void)
{
    struct anx_pkt *pkts[64];
    for each registered queue q:
        int n = q->poll(q, pkts, 64);
        for (int i = 0; i < n; i++) {
            struct anx_flow *f = flow_lookup(pkts[i]);
            if (f && f->fast_cell)
                anx_cell_dispatch(f->fast_cell, pkts[i]);
            else
                anx_net_stack_recv(pkts[i]);
        }
}
```

This is the entire fast path. No dynamic allocation, no function pointer table beyond the NIC's `poll` callback.

### 5.7 Capability Integration

NIC queue access is controlled by capability objects (RFC-0007):

```c
ANX_CAP_NET_RX   = 0x100   /* read from a queue */
ANX_CAP_NET_TX   = 0x200   /* write to a queue */
ANX_CAP_NET_PROG = 0x400   /* install fast-path cell on a queue */
```

An execution cell that lacks `ANX_CAP_NET_TX` cannot call `anx_net_tx()` — the capability check is in the call path.

---

## Implementation Plan

### Phase 1: Packet Arenas + anx_pkt (~400 lines)
- `kernel/core/net/pkt_arena.c` — arena alloc/free, pointer arithmetic
- `kernel/include/anx/pkt.h` — `anx_pkt`, `anx_pkt_arena` structs, API
- Tests: arena alloc/free cycle, zero-copy invariant

### Phase 2: Multi-Queue NIC + MT7925 upgrade (~300 lines)
- `kernel/core/net/netq.c` — queue registration, `anx_netq_run()`
- Upgrade MT7925 driver to implement `anx_netq` interface
- Retire per-driver `anx_e1000_poll()` / `anx_mt7925_poll()` calls in favor of `anx_netq_run()`

### Phase 3: Flow Table (~250 lines)
- `kernel/core/net/flow.c` — open-addressed hash, lock-free read path
- `kernel/include/anx/flow.h` — flow object API
- Tests: hash collision, eviction, flow lookup performance

### Phase 4: Fast-Path Cells + Capability Gate (~200 lines)
- `kernel/core/net/fastpath.c` — cell dispatch, capability check
- `kernel/core/net/capture.c` — packet capture to State Object
- Tests: cell install, packet forwarded vs. dropped by capability

### Estimated effort

| Phase | Lines | Deliverable |
|-------|-------|-------------|
| 1 | ~400 | Zero-copy arena, typed packets |
| 2 | ~300 | Multi-queue abstraction, MT7925 upgrade |
| 3 | ~250 | Flow table |
| 4 | ~200 | Fast-path cells |
| **Total** | **~1150** | **DPDK-competitive data plane** |

---

## Backward Compatibility

The existing Network Plane (RFC-0006) remains intact.  `anx_eth_send()` / `anx_eth_recv()` continue to work through adapter shims that allocate from the DMA arena and synthesize `anx_pkt` objects.  Drivers not yet upgraded to `anx_netq` continue to use the old poll model via the compatibility shim.

---

## Success Criteria

```
anx> netq bench 10000
queued 10000 frames  elapsed 0.8ms  12.5 Mpps  estimated 18.7 Gbps
```

Zero dynamic allocation during the bench run, verified by `anx_alloc_stats` showing no new allocations between start and end.
