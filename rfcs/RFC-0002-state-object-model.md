# RFC-0002: State Object Model

| Field       | Value                          |
|-------------|--------------------------------|
| RFC         | 0002                           |
| Title       | State Object Model             |
| Author      | Adam Pippert                   |
| Status      | Draft                          |
| Created     | 2026-04-13                     |
| Requires    | RFC-0001 (Architecture Thesis) |
| Supersedes  | —                              |

## 1. Abstract

In a traditional UNIX system, the **file** is the universal abstraction: an opaque sequence of bytes identified by a path in a hierarchical namespace. This design is simple and composable, but it forces all higher-level semantics — structure, provenance, access policy, lifecycle — into userland conventions that the kernel cannot reason about or enforce.

The **State Object** is Anunix's replacement for the file. A State Object is a self-describing, policy-governed unit of state that carries its own metadata, provenance history, and access rules as intrinsic properties rather than external conventions. Where a UNIX file is passive data that programs interpret, a State Object is an active participant in the system: the kernel understands its type, tracks its lineage, enforces its policies, and can make scheduling and storage decisions based on its semantic properties.

This RFC defines:

- The structure and fields of a State Object
- The six canonical object types
- The identity and addressing model
- The metadata, provenance, and policy schemas
- The object lifecycle (creation through deletion)
- The POSIX compatibility mapping
- The kernel system call interface

State Objects are the foundational primitive of Anunix. Every other subsystem — Execution Cells (RFC-0003), the Memory Control Plane (RFC-0004), and the Scheduler (RFC-0005) — operates on State Objects as its unit of work.
