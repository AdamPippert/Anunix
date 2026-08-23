# Anunix in 60 Seconds

Anunix replaces classical UNIX abstractions with primitives designed for AI-native workloads. Every concept maps to something you already know, but with a different shape.

## The Five Core Primitives

### State Objects (replaces files)

A State Object is a versioned, content-addressed unit of data. Unlike a file, it has:
- An **object ID (oid)** — a content-derived identifier, not a path
- A **type** — `data`, `code`, `model`, `credential`, `tensor`
- A **state** — `draft`, `active`, `sealed`
- A **version counter** — every write increments it
- **Provenance** — every State Object knows where it came from

Shell commands: `obj ls`, `obj create`, `obj get`, `obj seal`, `write`, `cat`, `cp`, `mv`

### Execution Cells (replaces processes)

An Execution Cell is a lifecycle-managed compute unit. Unlike a process, it has:
- A **capability scope** — what objects it can read/write/execute, enforced by the kernel
- A **resource budget** — CPU, memory, and action limits set at creation
- **Composability** — cells can be chained into pipelines through the Routing Plane

Shell commands: `cell create`, `cell run`, `cell show`

### Memory Planes (replaces malloc / mmap)

Memory Planes are tiered memory regions with semantic properties:
- Objects are **admitted** to a plane and decay or get promoted based on access patterns
- Planes have **retention policies** — objects that aren't accessed get reclaimed
- The kernel tracks **heat** (access frequency) and can consolidate cold objects

Shell commands: `memplane admit`, `memplane show`

### Routing Plane (replaces pipes and sockets)

The Routing Plane moves data between Execution Cells with type awareness:
- Routes have **transformation engines** — data is processed in flight, not just forwarded
- Routes are **typed** — a tensor route is different from a byte-stream route
- The Routing Plane is also how model requests are dispatched to the Model Hosting layer

Shell commands: `engine register`, route configuration via RFCs 0005/0006

### Capabilities (replaces chmod / ACLs)

Capabilities are unforgeable, delegatable tokens that grant access to specific objects:
- **Object-level** — a capability is for one object, not a directory
- **Delegatable** — a cell can pass a subset of its capabilities to a child cell
- **Audit-logged** — the kernel records every access check

Shell commands: `cap create`, `access check`

---

## How They Relate

```
  User / Agent
      │
      ▼
  Execution Cell  ──── has ────►  Capability scope
      │                               │
      │  reads/writes                 │  controls access to
      ▼                               ▼
  State Objects  ─── admitted to ──► Memory Planes
      │
      │  routed through
      ▼
  Routing Plane  ──► Transformation engines ──► Model Hosting
```

---

## If You Know UNIX

| You know this | Anunix calls it | Key difference |
|--------------|-----------------|----------------|
| `file` | State Object | Content-addressed, versioned, typed |
| `process` | Execution Cell | Capability-scoped, resource-budgeted |
| `malloc` / `mmap` | Memory Plane | Semantic decay and promotion |
| `pipe` | Routing Plane | Type-aware, transformation-capable |
| `chmod` / ACL | Capability | Object-level, unforgeable, delegatable |
| `.env` file | Credential Object | Kernel-enforced, opaque payload |
| `ld.so` / model server | Model Hosting | Kernel control plane for model lifecycle |

---

## The Shell (`ansh`)

`ansh` is the kernel monitor. It is not a Unix shell — there are no environment variables, no filesystem paths, no fork/exec. Every command operates on Anunix primitives directly.

Key built-ins to try first:
```
obj ls                          list all State Objects
write demo:hello "hello world"  create a State Object
obj get demo:hello              read it back
cell create my-cell             create an Execution Cell
sysinfo                         system state summary
help                            all commands
```

---

## Design Documents

| RFC | Topic |
|-----|-------|
| [RFC-0001](rfcs/RFC-0001-architecture-thesis.md) | Architecture overview |
| [RFC-0002](rfcs/RFC-0002-state-object-model.md) | State Objects |
| [RFC-0003](docs/rfcs/RFC-0003-execution-cell-runtime.md) | Execution Cells |
| [RFC-0004](docs/rfcs/RFC-0004-memory-control-plane.md) | Memory Planes |
| [RFC-0005](docs/rfcs/RFC-0005-routing-and-scheduler.md) | Routing Plane |
| [RFC-0007](docs/rfcs/RFC-0007-capability-objects.md) | Capabilities |
| [RFC-0013](docs/rfcs/RFC-0013-tensor-objects.md) | Tensor Objects |
