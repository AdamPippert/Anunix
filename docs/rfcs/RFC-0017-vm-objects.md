# RFC-0017: VM Objects — Dual-Nature Virtual Machine Primitives

| Field      | Value                                                              |
|------------|--------------------------------------------------------------------|
| RFC        | 0017                                                               |
| Title      | VM Objects — Dual-Nature Virtual Machine Primitives                |
| Author     | Adam Pippert                                                       |
| Status     | Draft                                                              |
| Created    | 2026-04-20                                                         |
| Updated    | 2026-04-20                                                         |
| Depends On | RFC-0001, RFC-0002, RFC-0003, RFC-0005, RFC-0007, RFC-0008         |

---

## Executive Summary

A virtual machine is two things simultaneously: a **persistent configuration** (CPU topology, memory layout, disk images, firmware, network interfaces) that must survive across reboots, be versioned, be snapshotted, and be inspectable by agents — and a **running unit of execution** (a guest OS consuming CPU cycles, producing network traffic, emitting console output) that obeys the same admission, routing, and policy model as every other cell in the system.

Existing Anunix primitives address each half in isolation. RFC-0002 State Objects can store VM configuration and disk images as structured and byte data. RFC-0003 Execution Cells can model the running VM instance. But neither primitive alone captures the dual nature: State Objects have no concept of "start this thing," and Execution Cells have no concept of "this thing has a persistent structured configuration that agents may inspect and modify safely."

RFC-0017 introduces the **VM Object** — a new `ANX_OBJ_VM` State Object type — and the **VM Cell** — a new `ANX_CELL_VM` Execution Cell subtype — that together model a virtual machine as a first-class dual-nature entity. The VM Object is the at-rest configuration and persistent state tree. The VM Cell is the running instance. The relationship is permanent and bidirectional: the cell is always backed by an object, and the object records the cell's current state. Credential Objects (RFC-0008) gate every lifecycle operation. Capability Objects (RFC-0007) give agents scoped access to specific VM configuration fields, enabling an agent to inspect or modify CPU count, memory size, or network topology without being able to destroy the disk or start an unauthorized instance.

The result is a hypervisor that Anunix's routing plane, credential manager, and agent layer treat as a native subsystem rather than an external tool.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert
**Depends on:** RFC-0001, RFC-0002, RFC-0003, RFC-0005, RFC-0007, RFC-0008
**Blocks:** —

---

## 2. Problem Statement

### 2.1 VMs Are Not Files

The conventional approach — KVM/QEMU with disk images as files, configuration as XML or TOML, management via `virsh` or `qemu-system` command lines — treats a VM as an artifact of the host filesystem. Disk images are files. Configurations are config files. Running VMs are processes. This model has three consequences that are unacceptable in an AI-native OS:

**Agents cannot safely edit VM configuration.** A running agent that needs to add a network interface or increase a VM's memory has no primitive for "modify field X of this VM's config while preserving everything else and without touching the disk image." It must parse XML, validate, regenerate, reload — a fragile procedure that bypasses all provenance tracking and policy enforcement.

**Credentials are ambient.** Starting a VM requires root or the `kvm` group. Every cell that can start one VM can start any VM. There is no mechanism for "this agent may start VM `build-worker-1` but not `production-db`."

**Snapshots have no lineage.** A VM snapshot created by `qemu-img snapshot` has no relationship to the credential that authorized it, the agent that requested it, or the state of the system at creation time. It is an opaque file on disk with a name.

### 2.2 VMs Are Not Just Cells

An Execution Cell is a unit of work with a finite lifetime: it starts, runs, produces outputs, and terminates. A VM is different. Its defining property is not a computation that completes — it is a persistent system that runs indefinitely, may be paused and resumed, accumulates state across runs, and has a durable identity independent of whether it is currently powered on. A VM that is stopped is not finished; it is dormant. Its disk, NVRAM, and configuration remain meaningful objects even when no cell is running.

The RFC-0003 cell model has no concept of a dormant-but-persistent execution unit. Modeling a VM purely as a cell would mean losing the object identity — there would be no addressable thing to snapshot, inspect, or credential-gate when the VM is off.

### 2.3 What Is Needed

A clean VM primitive for Anunix requires:

1. A persistent **object** that exists and is addressable regardless of whether the VM is running.
2. A structured **configuration subtree** that agents can read and write under fine-grained capability control.
3. An **execution cell** that is the running instance, with standard cell admission, tracing, and policy.
4. **Credential bindings** that gate lifecycle operations (start, stop, config change, snapshot) per VM identity.
5. **Snapshots as immutable objects** with full provenance, linked to the VM object that produced them.
6. A **hypervisor backend abstraction** so the same API works on KVM (jekyll), QEMU usermode, and Apple Virtualization.framework.

---

## 3. Goals

### 3.1 Primary Goals

1. **VM Object as State Object.** `ANX_OBJ_VM` is a first-class State Object type with OID, provenance, access policy, and lifecycle.

2. **VM Cell as Execution Cell.** A running VM is an `ANX_CELL_VM` with standard admission, tracing, and cell policy. The cell and object share identity.

3. **Structured configuration.** VM configuration is a structured State Object subtree that the kernel understands semantically. Agents can query and modify specific fields through the object API, not string manipulation.

4. **Credential-gated lifecycle.** Every lifecycle operation (create, start, pause, resume, stop, destroy, snapshot, config-edit) requires a named credential binding, checked at the kernel boundary.

5. **Capability-scoped agent access.** Capability Objects define the exact set of VM configuration fields an agent may read or write. An agent with `ANX_VM_CAP_NET_EDIT` can add/remove network interfaces; it cannot touch the disk configuration.

6. **Snapshots as immutable VM Objects.** A snapshot is a sealed, immutable `ANX_OBJ_VM` with provenance linking it to its parent and the credential that authorized the snapshot.

7. **Hypervisor backend abstraction.** The kernel VM subsystem defines a backend interface. KVM, QEMU, and Apple Virtualization.framework are pluggable implementations.

8. **Provenance on every mutation.** Every config change, snapshot, and lifecycle transition generates a provenance record linking the event to its authorizing credential and executing agent.

### 3.2 Non-Goals

- **Full hypervisor implementation.** This RFC defines the kernel interface and object model. KVM and QEMU backends delegate to kernel KVM driver or fork a QEMU process.
- **Live migration.** Cross-host VM migration is deferred to a later RFC (depends on RFC-0006 network plane extensions).
- **GPU passthrough.** Hardware passthrough is a backend concern; the VM Object model is agnostic.
- **Nested virtualization.** Out of scope.
- **Container support.** Containers (namespaces + cgroups) are a separate primitive; they may share the cell model but not the VM Object type.

---

## 4. Core Definitions

### 4.1 VM Object (`ANX_OBJ_VM`)

A **VM Object** is a State Object of type `ANX_OBJ_VM`. It is the durable, addressable identity of a virtual machine. It exists regardless of whether the VM is currently running. It owns references to child State Objects: the VM configuration, disk images, NVRAM, and snapshots.

A VM Object has exactly one **power state** at any moment: `DEFINED`, `RUNNING`, `PAUSED`, or `SAVED`. These states govern which operations are permitted and which cell, if any, is currently bound to the object.

### 4.2 VM Cell (`ANX_CELL_VM`)

A **VM Cell** is an Execution Cell of type `ANX_CELL_VM`. It is created when a VM Object transitions from `DEFINED` or `SAVED` to `RUNNING`. It is destroyed when the VM transitions back to `DEFINED` or `SAVED`. A VM Object may have at most one active VM Cell at any time.

The VM Cell is admitted, traced, and policy-checked like every other cell. Its input is the VM Object's OID. Its output is the VM's exit state (shutdown, crash, or suspend-to-disk). Its trace records every lifecycle event that occurred during the run.

### 4.3 VM Configuration Object

A VM Configuration Object is an `ANX_OBJ_STRUCTURED_DATA` child of the VM Object. Its schema is defined in Section 6. It is the structured representation of everything QEMU or KVM needs to instantiate the VM — CPU topology, memory sizing, firmware path, disk attachments, network interfaces, display, serial ports, and agent access policy.

The configuration object is mutable (subject to capability checks) when the VM is `DEFINED` or `SAVED`. When the VM is `RUNNING` or `PAUSED`, only hotplug-safe fields may be modified.

### 4.4 VM Disk Object

A **VM Disk Object** is an `ANX_OBJ_BYTE_DATA` child of the VM Object with subtype `ANX_BLOB_VM_DISK`. It holds a raw disk image. Disk objects are large; they are stored via the disk-store backend (RFC-0015 block layer). Disk objects may be shared between VM Objects when the sharing policy permits (e.g., a base image shared by multiple clone VMs), but the sharing relationship is tracked in provenance.

### 4.5 VM Snapshot

A **VM Snapshot** is a sealed, immutable `ANX_OBJ_VM` whose `parent_vm_oid` field references the VM Object it was derived from. A snapshot captures the full object tree at the moment of creation: the configuration, disk state (as a copy-on-write overlay reference), and NVRAM. It cannot be started directly; it must be cloned into a new VM Object first.

### 4.6 VM Credential Binding

A **VM Credential Binding** is a Credential Object (RFC-0008) named by convention `vm/<vm-name>/<operation>`, where `<operation>` is one of: `lifecycle`, `config`, `snapshot`, `console`, `exec`. A cell must hold the appropriate binding for the operation it intends to perform. The kernel checks the binding at every VM API call.

---

## 5. VM Object Type Addition

This RFC adds `ANX_OBJ_VM` to the State Object type enum defined in RFC-0002 Section 5. Per RFC-0002 Section 5.7, adding a new object type requires an RFC because it affects every subsystem that switches on object type.

```c
/* kernel/include/anx/state_object.h — addition to enum anx_object_type */
ANX_OBJ_VM = 9,		/* Virtual Machine Object (RFC-0017) */
```

VM Objects participate in the full State Object lifecycle (NASCENT → ACTIVE → SEALED/ARCHIVED/DELETED). The SEALED state for a VM Object means the VM is a snapshot: immutable, read-only, cannot be started without cloning.

---

## 6. VM Configuration Schema

The VM configuration is a structured State Object with the following schema. All fields are optional unless marked required.

```
vm_config {
    /* Identity */
    name:           string (required) — human-readable name, unique in namespace
    uuid:           uuid   — stable identity; generated on create if absent
    description:    string

    /* CPU */
    cpu {
        count:      uint32 (required, default 1)
        model:      string ("host", "qemu64", "cortex-a72", ...)
        topology {
            sockets: uint32
            cores:   uint32
            threads: uint32
        }
        features:   [string]   — CPU feature flags to expose/hide
    }

    /* Memory */
    memory {
        size_mb:    uint64 (required, default 512)
        balloon:    bool   — enable virtio-balloon for dynamic sizing
        hugepages:  bool
    }

    /* Firmware / Boot */
    boot {
        firmware:   enum (bios | uefi | direct_kernel)
        kernel_ref: oid    — State Object OID of kernel image (direct_kernel only)
        initrd_ref: oid    — State Object OID of initrd (direct_kernel only)
        cmdline:    string — kernel command line (direct_kernel only)
        order:      [enum] — (disk | cdrom | net | none)
        uefi_nvram_ref: oid — State Object OID of NVRAM (uefi only)
    }

    /* Disk attachments */
    disks: [
        {
            disk_ref:   oid (required) — State Object OID of ANX_BLOB_VM_DISK
            bus:        enum (virtio | ide | scsi)
            index:      uint32
            read_only:  bool
            cache:      enum (writeback | writethrough | none | unsafe)
            format:     enum (raw | qcow2)
        }
    ]

    /* Network interfaces */
    network: [
        {
            model:      enum (virtio | e1000 | rtl8139)
            mac:        string — 00:00:00:00:00:00 format; generated if absent
            net_ref:    oid    — Anunix Network Object OID (RFC-0006)
            bridge:     string — fallback: host bridge name (e.g. "virbr0")
        }
    ]

    /* Display */
    display {
        type:       enum (none | vnc | spice | framebuffer)
        width:      uint32
        height:     uint32
        vnc_port:   uint32 (type=vnc only)
    }

    /* Serial / Console */
    serial: [
        {
            index:  uint32
            target: enum (stdio | pty | file | tcp | none)
            path:   string  — file path or TCP address
        }
    ]

    /* Agent access policy */
    agent_policy {
        /* Fields agents may read without ANX_VM_CAP_CONFIG_READ */
        public_fields:  [string]  — e.g. ["name", "cpu.count", "memory.size_mb"]

        /* Hotplug-safe fields (may be modified while RUNNING) */
        hotplug_fields: [string]  — e.g. ["memory.size_mb", "network"]

        /* Maximum resource limits for agent modifications */
        max_cpu_count:  uint32
        max_memory_mb:  uint64
        max_disk_count: uint32
        max_net_count:  uint32
    }
}
```

---

## 7. VM Object Hierarchy

A VM Object owns a tree of child State Objects tracked in its `children` provenance relation. The canonical structure:

```
ANX_OBJ_VM  [oid: vm/<name>]
├── config         ANX_OBJ_STRUCTURED_DATA  [oid: vm/<name>/config]
├── disks/
│   ├── disk0      ANX_OBJ_BYTE_DATA (ANX_BLOB_VM_DISK)
│   └── disk1      ANX_OBJ_BYTE_DATA (ANX_BLOB_VM_DISK)
├── nvram          ANX_OBJ_BYTE_DATA        [oid: vm/<name>/nvram]
└── snapshots/
    ├── snap-<ts>  ANX_OBJ_VM (sealed)
    └── snap-<ts>  ANX_OBJ_VM (sealed)
```

Child object lifetimes are bound to the parent VM Object. Deleting a VM Object cascades to all children unless a child has external references (a disk shared by another VM, a snapshot that a second VM was cloned from).

---

## 8. VM Cell Type Addition

This RFC adds `ANX_CELL_VM` to the cell type enum in RFC-0003.

```c
/* kernel/include/anx/cell.h — addition to enum anx_cell_type */
ANX_CELL_VM = 8,	/* Virtual Machine execution cell (RFC-0017) */
```

### 8.1 VM Cell Admission

When a VM Object transitions to `RUNNING`, the kernel creates an `ANX_CELL_VM` and runs the standard cell admission sequence (RFC-0003 Section 7):

1. **Credential check** — the requesting cell must hold `vm/<name>/lifecycle` credential binding.
2. **Resource check** — the scheduler verifies CPU and memory availability.
3. **Policy check** — the VM cell inherits the requesting cell's trust domain and network policy, intersected with the VM object's agent policy.
4. **Backend check** — the hypervisor backend reports whether KVM is available; if not, falls back to QEMU usermode.

Admission failure leaves the VM Object in `DEFINED` state. No cell is created.

### 8.2 VM Cell Inputs and Outputs

A VM Cell takes one implicit input: the OID of its backing VM Object. All configuration is read from the config child object at start time.

The cell's output State Object (committed on exit) contains:
- `exit_reason`: enum (shutdown | reboot | crash | kill | suspend)
- `exit_code`: uint32
- `runtime_seconds`: uint64
- `final_state_ref`: oid of a snapshot, if `exit_reason == suspend`

### 8.3 Tracing

Every lifecycle event during a VM Cell's run is appended to the cell's execution trace (RFC-0003 Section 11). Traced events include: start, pause, resume, config hotplug, console attach/detach, snapshot creation, and stop. Each event records the authorizing credential binding's name (never its payload) and the timestamp.

---

## 9. Lifecycle and State Machine

```
                    anx_vm_create()
                         │
                    ┌────▼────┐
                    │ DEFINED │ ◄────────────────────────────────┐
                    └────┬────┘                                  │
          anx_vm_start() │  (requires vm/<n>/lifecycle cred)     │
                    ┌────▼────┐         anx_vm_stop()            │
                    │ RUNNING │ ─────────────────────────────────┤
                    └────┬────┘                                  │
         anx_vm_pause()  │                                       │
                    ┌────▼────┐       anx_vm_restore()           │
                    │ PAUSED  │ ──────────────────────────────── ┤
                    └────┬────┘                                  │
     anx_vm_suspend()    │          (snapshot → new DEFINED VM)  │
                    ┌────▼────┐                                  │
                    │  SAVED  │ ─────────────────────────────────┘
                    └─────────┘
                         │
              anx_vm_destroy() ──► DELETED (object + all children gc'd)
```

**State transition rules:**

| From      | To      | Operation             | Required Credential       |
|-----------|---------|-----------------------|---------------------------|
| DEFINED   | RUNNING | `anx_vm_start()`      | `vm/<name>/lifecycle`     |
| RUNNING   | PAUSED  | `anx_vm_pause()`      | `vm/<name>/lifecycle`     |
| PAUSED    | RUNNING | `anx_vm_resume()`     | `vm/<name>/lifecycle`     |
| RUNNING   | DEFINED | `anx_vm_stop()`       | `vm/<name>/lifecycle`     |
| PAUSED    | DEFINED | `anx_vm_stop()`       | `vm/<name>/lifecycle`     |
| RUNNING   | SAVED   | `anx_vm_suspend()`    | `vm/<name>/lifecycle`     |
| SAVED     | RUNNING | `anx_vm_restore()`    | `vm/<name>/lifecycle`     |
| any       | DELETED | `anx_vm_destroy()`    | `vm/<name>/lifecycle`     |
| any       | —       | `anx_vm_snapshot()`   | `vm/<name>/snapshot`      |
| DEFINED   | —       | `anx_vm_config_set()` | `vm/<name>/config`        |
| RUNNING   | —       | `anx_vm_hotplug()`    | `vm/<name>/config`        |
| any       | —       | `anx_vm_console()`    | `vm/<name>/console`       |
| RUNNING   | —       | `anx_vm_exec()`       | `vm/<name>/exec`          |

---

## 10. Credential and Capability Model

### 10.1 Credential Naming Convention

VM credentials follow the naming convention established in RFC-0008 Section 5:

```
vm/<vm-name>/lifecycle   — start, stop, pause, resume, suspend, restore, destroy
vm/<vm-name>/config      — config reads/writes and hotplug
vm/<vm-name>/snapshot    — snapshot creation and restoration
vm/<vm-name>/console     — attach to serial/VGA console
vm/<vm-name>/exec        — inject commands via guest agent
```

A credential with a broader scope may delegate narrower scopes to child cells using the RFC-0008 delegation mechanism.

### 10.2 Capability Objects for Agent Access

When an agent cell needs access to a VM's configuration — to inspect its topology, recommend resource changes, or apply a hardware profile — it receives a scoped Capability Object (RFC-0007) rather than a raw credential binding. The Capability Object specifies exactly which configuration fields the agent may access:

```c
struct anx_vm_capability {
    anx_oid_t	vm_oid;			/* which VM */
    uint32_t	field_mask;		/* bitmask of ANX_VM_FIELD_* */
    bool		read_only;		/* if true, no writes */
    bool		hotplug_only;		/* if true, only hotplug-safe fields */
    uint32_t	max_cpu_count;		/* ceiling on cpu.count writes */
    uint64_t	max_memory_mb;		/* ceiling on memory.size_mb writes */
};

/* ANX_VM_FIELD_* bitmask values */
#define ANX_VM_FIELD_CPU		(1u << 0)
#define ANX_VM_FIELD_MEMORY		(1u << 1)
#define ANX_VM_FIELD_BOOT		(1u << 2)
#define ANX_VM_FIELD_DISKS		(1u << 3)
#define ANX_VM_FIELD_NETWORK		(1u << 4)
#define ANX_VM_FIELD_DISPLAY		(1u << 5)
#define ANX_VM_FIELD_SERIAL		(1u << 6)
#define ANX_VM_FIELD_AGENT_POLICY	(1u << 7)
#define ANX_VM_FIELD_ALL		(0xFFu)
```

An agent with `ANX_VM_FIELD_NETWORK | ANX_VM_FIELD_CPU` and `read_only = false` may add a network interface or change the CPU count, but cannot touch the disk configuration or firmware settings.

### 10.3 Policy Enforcement

The kernel enforces capability scope at every `anx_vm_config_set()` call:

1. Resolve the calling cell's VM Capability Object for this VM OID.
2. Determine which top-level config key the write targets.
3. Check the field mask. Reject with `ANX_EPERM` if the field bit is not set.
4. Check `read_only`. Reject writes if set.
5. Check `hotplug_only` if VM is `RUNNING`. Reject non-hotplug-safe fields.
6. Check numeric ceilings (`max_cpu_count`, `max_memory_mb`). Reject if exceeded.
7. Apply the write, record provenance.

---

## 11. Agent Primitive Access

Agents interact with VM Objects through the standard `anx_so_*` object API for reads, and through the VM-specific config API for writes. The design principle is: **agents see VM configuration as structured data, not as a configuration language.**

### 11.1 Reading VM State

An agent with any VM Capability Object (even `ANX_VM_FIELD_CPU` read-only) may call:

```c
/* Read a single config field by JSON path */
int anx_vm_config_get(const anx_oid_t *vm_oid,
                      const char *field_path,
                      struct anx_state_object **value_out);

/* Read the full config subtree (respects field_mask) */
int anx_vm_config_dump(const anx_oid_t *vm_oid,
                       struct anx_state_object **config_out);

/* Query VM power state */
int anx_vm_state_get(const anx_oid_t *vm_oid,
                     enum anx_vm_state *state_out);

/* List all VMs in the system visible to this cell */
int anx_vm_list(anx_oid_t *results, uint32_t max,
                uint32_t *count_out);
```

Fields in `agent_policy.public_fields` are readable without a Capability Object — they are intended for status dashboards and monitoring agents that should not require elevated access.

### 11.2 Writing VM Configuration

```c
/* Set a single config field by JSON path */
int anx_vm_config_set(const anx_oid_t *vm_oid,
                      const char *field_path,
                      const struct anx_state_object *value);

/* Apply a structured patch (RFC-7396 merge patch semantics) */
int anx_vm_config_patch(const anx_oid_t *vm_oid,
                        const struct anx_state_object *patch);
```

Both calls require a Capability Object covering the targeted fields. Both record a provenance event. `anx_vm_config_patch()` is atomic: either all fields in the patch are applied, or none are.

### 11.3 Agent-Driven Lifecycle

An agent with the appropriate credential binding may trigger lifecycle transitions programmatically:

```c
int anx_vm_start(const anx_oid_t *vm_oid);
int anx_vm_stop(const anx_oid_t *vm_oid, bool force);
int anx_vm_pause(const anx_oid_t *vm_oid);
int anx_vm_resume(const anx_oid_t *vm_oid);
int anx_vm_snapshot(const anx_oid_t *vm_oid, const char *name,
                    anx_oid_t *snap_out);
int anx_vm_exec(const anx_oid_t *vm_oid,
                const char *command,
                struct anx_state_object **output_out);
```

`anx_vm_exec()` injects a command into the running VM via a guest agent (virtio-serial channel). Output is returned as an `ANX_OBJ_STRUCTURED_DATA` object containing stdout, stderr, and exit code, with full provenance.

---

## 12. Hypervisor Backend Abstraction

The VM subsystem defines a backend interface that isolates the kernel's VM Object management layer from the specific hypervisor implementation. All backends implement the same interface; the correct backend is selected at VM Object creation time based on host capabilities.

```c
struct anx_vm_backend {
    const char *name;

    /* Lifecycle */
    int (*create)(struct anx_vm_object *, const struct anx_vm_config *);
    int (*start)(struct anx_vm_object *);
    int (*pause)(struct anx_vm_object *);
    int (*resume)(struct anx_vm_object *);
    int (*stop)(struct anx_vm_object *, bool force);
    int (*destroy)(struct anx_vm_object *);

    /* State capture */
    int (*snapshot)(struct anx_vm_object *, anx_oid_t *snap_out);
    int (*suspend)(struct anx_vm_object *, anx_oid_t *state_out);
    int (*restore)(struct anx_vm_object *, const anx_oid_t *state_oid);

    /* Hotplug */
    int (*hotplug_cpu)(struct anx_vm_object *, uint32_t count);
    int (*hotplug_mem)(struct anx_vm_object *, uint64_t size_mb);
    int (*hotplug_net)(struct anx_vm_object *,
                       const struct anx_vm_netdev *dev);

    /* Console and exec */
    int (*console_read)(struct anx_vm_object *, void *buf, uint32_t len,
                        uint32_t *read_out);
    int (*console_write)(struct anx_vm_object *, const void *buf,
                         uint32_t len);
    int (*exec)(struct anx_vm_object *, const char *cmd,
                struct anx_state_object **output_out);

    /* Introspection */
    int (*query_state)(struct anx_vm_object *,
                       enum anx_vm_state *state_out);
    int (*query_stats)(struct anx_vm_object *,
                       struct anx_vm_stats *stats_out);
};
```

### 12.1 KVM Backend (`anx_vm_backend_kvm`)

The KVM backend uses Linux `/dev/kvm` directly for x86_64 and ARM64 guests. It is the preferred backend on jekyll and any other Linux host with KVM support. Implementation lives in `kernel/core/vm/backend_kvm.c`.

The backend creates one KVM VM fd, one vCPU fd per CPU, and one memslot per memory region. QEMU user-mode networking is used for the network backend unless a TAP interface is explicitly requested via the network config.

Availability check: `access("/dev/kvm", R_OK | W_OK) == 0`.

### 12.2 QEMU Process Backend (`anx_vm_backend_qemu`)

The QEMU backend forks `qemu-system-x86_64` or `qemu-system-aarch64` as a child process, passing it a machine description constructed from the VM configuration object. Control is via the QEMU Machine Protocol (QMP) socket. Console I/O uses a virtio-serial channel.

This backend runs on any host with QEMU installed, including macOS (for development testing). It is slower than the KVM backend but requires no kernel module.

Availability check: `access("/usr/bin/qemu-system-x86_64", X_OK) == 0` (or host-arch variant).

### 12.3 Apple Virtualization Backend (`anx_vm_backend_vz`) — Future

On macOS ARM64 (Mac Studio, MacBook), the Apple Virtualization.framework provides near-native ARM64 guest performance. This backend is deferred to a future implementation increment but the interface above is designed to accommodate it. The backend would live in `kernel/core/vm/backend_vz.m` (Objective-C bridge).

### 12.4 Backend Selection

```c
static struct anx_vm_backend *anx_vm_select_backend(void)
{
    if (anx_vm_backend_kvm.available())
        return &anx_vm_backend_kvm;
    if (anx_vm_backend_qemu.available())
        return &anx_vm_backend_qemu;
    return NULL;  /* ANX_ENODEV returned to caller */
}
```

A VM Object may optionally specify a preferred backend in its configuration. If the preferred backend is unavailable, the selection falls through to the next available backend.

---

## 13. Networking

VMs receive network connectivity through the standard Anunix network plane (RFC-0006). Each network interface in the VM config references an Anunix Network Object OID, which the kernel maps to the appropriate host-side network resource.

Three network attachment modes are supported:

**User-mode (SLIRP).** No host privilege required. The VM gets 10.0.2.x DHCP, with port forwarding rules expressed as RFC-0006 flow objects. Used for development and single-VM workloads.

**Bridge attachment.** The VM's virtio-net device is bridged to a host network interface. The network object OID references a bridge interface managed by the Anunix network plane. Used for production VMs that need direct LAN access.

**Internal network.** Multiple VMs share an isolated virtual network. Traffic stays within the host. Used for multi-VM workloads (e.g., build clusters, test environments) where VMs need to communicate with each other but not the LAN.

Network configuration is hotplug-capable: network interfaces may be added or removed while the VM is running, subject to the `ANX_VM_FIELD_NETWORK` capability check.

---

## 14. Snapshots

### 14.1 Snapshot as Sealed VM Object

When `anx_vm_snapshot()` is called, the kernel:

1. Checks the `vm/<name>/snapshot` credential binding of the calling cell.
2. Pauses the VM if running (or takes a live snapshot via the backend if supported).
3. Creates a new `ANX_OBJ_VM` with:
   - `parent_vm_oid` = OID of the source VM Object
   - `snapshot_time` = current timestamp
   - `snapshot_credential` = name (not payload) of the authorizing credential
   - A frozen copy of the config child object
   - A copy-on-write overlay reference to each disk object
   - A copy of the NVRAM object
4. Seals the new VM Object (makes it immutable).
5. Resumes the VM if it was running.
6. Records a provenance event on the source VM Object linking to the new snapshot OID.

### 14.2 Cloning from a Snapshot

A snapshot cannot be started directly. To run a VM from a snapshot:

```c
int anx_vm_clone(const anx_oid_t *snapshot_oid,
                 const char *new_name,
                 anx_oid_t *new_vm_out);
```

This creates a new mutable VM Object with its own config copy, disk copy (or CoW reference depending on policy), and NVRAM copy. The new VM's provenance records its origin snapshot. The new VM starts in `DEFINED` state and can be modified before being started.

### 14.3 Snapshot Retention Policy

Snapshots are State Objects and participate in the Memory Control Plane retention policy (RFC-0004). The VM Object's config `agent_policy` may specify a `snapshot_max_count` and `snapshot_max_age_seconds`. The kernel enforces these limits automatically, deleting the oldest snapshots when limits are exceeded (subject to retention policy constraints — snapshots with `retain_until` set are never auto-deleted).

---

## 15. Kernel API Summary

```c
/* Creation and deletion */
int anx_vm_create(const struct anx_vm_config *config,
                  anx_oid_t *vm_oid_out);
int anx_vm_destroy(const anx_oid_t *vm_oid);

/* Lifecycle */
int anx_vm_start(const anx_oid_t *vm_oid);
int anx_vm_stop(const anx_oid_t *vm_oid, bool force);
int anx_vm_pause(const anx_oid_t *vm_oid);
int anx_vm_resume(const anx_oid_t *vm_oid);
int anx_vm_suspend(const anx_oid_t *vm_oid, anx_oid_t *state_out);
int anx_vm_restore(const anx_oid_t *vm_oid,
                   const anx_oid_t *state_oid);

/* Configuration */
int anx_vm_config_get(const anx_oid_t *vm_oid,
                      const char *field_path,
                      struct anx_state_object **value_out);
int anx_vm_config_set(const anx_oid_t *vm_oid,
                      const char *field_path,
                      const struct anx_state_object *value);
int anx_vm_config_patch(const anx_oid_t *vm_oid,
                        const struct anx_state_object *patch);
int anx_vm_config_dump(const anx_oid_t *vm_oid,
                       struct anx_state_object **config_out);

/* Snapshots */
int anx_vm_snapshot(const anx_oid_t *vm_oid, const char *name,
                    anx_oid_t *snap_out);
int anx_vm_clone(const anx_oid_t *snap_oid, const char *new_name,
                 anx_oid_t *new_vm_out);

/* Disk management */
int anx_vm_disk_create(uint64_t size_bytes, enum anx_vm_disk_format fmt,
                       anx_oid_t *disk_out);
int anx_vm_disk_attach(const anx_oid_t *vm_oid,
                       const anx_oid_t *disk_oid,
                       const struct anx_vm_disk_config *cfg);
int anx_vm_disk_detach(const anx_oid_t *vm_oid, uint32_t disk_index);

/* Console and exec */
int anx_vm_console(const anx_oid_t *vm_oid, int *fd_out);
int anx_vm_exec(const anx_oid_t *vm_oid, const char *cmd,
                struct anx_state_object **output_out);

/* Enumeration */
int anx_vm_list(anx_oid_t *results, uint32_t max, uint32_t *count_out);
int anx_vm_state_get(const anx_oid_t *vm_oid,
                     enum anx_vm_state *state_out);
int anx_vm_stats_get(const anx_oid_t *vm_oid,
                     struct anx_vm_stats *stats_out);
```

---

## 16. Shell Integration

The `vm` builtin exposes VM operations to Anunix shell sessions and agent cells running CEXL (RFC-0016):

```
vm create <name> [--cpu N] [--mem MB] [--disk SIZE] [--net virbr0]
vm list
vm start <name>
vm stop <name>
vm pause <name>
vm resume <name>
vm snapshot <name> [--tag <tag>]
vm snapshots <name>
vm restore <name> --snap <snap-name>
vm clone <snap-name> <new-name>
vm config get <name> [<field-path>]
vm config set <name> <field-path> <value>
vm console <name>
vm exec <name> <command>
vm destroy <name>
```

All commands check credentials before execution. `vm config set` checks the Capability Object of the calling cell. Commands that succeed emit provenance records.

---

## 17. Implementation Plan

Implementation proceeds in three phases:

**Phase 1 — Object model and QEMU backend:**
- Add `ANX_OBJ_VM` to state object type enum
- Add `ANX_CELL_VM` to cell type enum
- Implement VM Object CRUD in `kernel/core/vm/vm_object.c`
- Implement VM config schema and validation in `kernel/core/vm/vm_config.c`
- Implement QEMU process backend in `kernel/core/vm/backend_qemu.c`
- Add `vm` shell commands in `kernel/core/tools/vm.c`
- Tests in `tests/test_vm_object.c`, `tests/test_vm_config.c`

**Phase 2 — Credential and capability enforcement:**
- Implement credential binding checks in all `anx_vm_*` API calls
- Implement `struct anx_vm_capability` and field mask enforcement
- Implement `anx_vm_config_patch()` atomicity
- Tests in `tests/test_vm_credentials.c`

**Phase 3 — KVM backend and snapshots:**
- Implement KVM backend in `kernel/core/vm/backend_kvm.c`
- Implement snapshot create/restore with CoW disk overlay
- Implement `anx_vm_exec()` via virtio-serial guest agent protocol
- Tests in `tests/test_vm_snapshot.c`, `tests/test_vm_exec.c`

---

## 18. Directory Structure

```
kernel/core/vm/
    vm_object.c         — ANX_OBJ_VM CRUD, lifecycle state machine
    vm_config.c         — config schema, validation, get/set/patch
    vm_cell.c           — ANX_CELL_VM admission, tracing, policy
    vm_disk.c           — disk object management, CoW overlay
    vm_snapshot.c       — snapshot create/restore/clone
    vm_credential.c     — credential binding checks
    vm_capability.c     — capability object enforcement
    backend_qemu.c      — QEMU process backend (Phase 1)
    backend_kvm.c       — KVM direct backend (Phase 3)
kernel/include/anx/
    vm.h                — public VM API and type definitions
    vm_config.h         — config schema structs
    vm_backend.h        — backend interface
kernel/core/tools/
    vm.c                — shell 'vm' builtin
tests/
    test_vm_object.c
    test_vm_config.c
    test_vm_credentials.c
    test_vm_snapshot.c
    test_vm_exec.c
```

---

## Appendix A: Example — Agent Creates and Configures a VM

```
; CEXL cell that provisions a build worker VM
(cell build-worker-provision
  :credentials ["vm/build-worker/lifecycle"
                "vm/build-worker/config"
                "vm/build-worker/snapshot"]
  :capabilities [(vm-cap build-worker
                   :fields (cpu memory network)
                   :max-cpu 8
                   :max-memory-mb 32768)]
  (do
    ; Create the VM object
    (vm create "build-worker"
      :cpu 4 :mem 16384
      :disk (vm disk-create 40GB :format qcow2)
      :net (net-object "internal-build"))

    ; Agent inspects and adjusts based on available host resources
    (let [host-cpus (sysinfo :cpu-count)]
      (when (>= host-cpus 16)
        (vm config set "build-worker" "cpu.count" 8)))

    ; Take a pre-start snapshot as a clean baseline
    (vm snapshot "build-worker" :tag "pre-start")

    ; Boot it
    (vm start "build-worker")))
```

---

## Appendix B: Credential Provisioning for VMs

VM credentials follow the RFC-0008 provisioning model. At system setup, the administrator provisions credentials into the credential store:

```sh
# Provision credentials for all VM operations on 'build-worker'
ansh> cred provision vm/build-worker/lifecycle --from-file /keys/vm-lifecycle.key
ansh> cred provision vm/build-worker/config    --from-file /keys/vm-config.key
ansh> cred provision vm/build-worker/snapshot  --from-file /keys/vm-snapshot.key
```

Agent cells are then admitted with the credential bindings they need:

```sh
ansh> cell run provision-agent \
  --cred vm/build-worker/lifecycle \
  --cred vm/build-worker/config \
  --cap  "vm-cap:build-worker:cpu,memory:rw"
```

The kernel resolves `vm-cap:build-worker:cpu,memory:rw` to an `anx_vm_capability` with `field_mask = ANX_VM_FIELD_CPU | ANX_VM_FIELD_MEMORY` and `read_only = false`. No other VM fields are accessible to the agent.
