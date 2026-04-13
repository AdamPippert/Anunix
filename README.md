# Anunix — Agent Native UNIX

A proposal for a next-generation UNIX-like operating system designed for both classical computing and model-based computation.

## Thesis

Traditional operating systems treat:
- files as passive data
- processes as isolated execution units
- memory as a hidden implementation detail

This architecture proposes:

- **State Objects instead of Files**
- **Execution Cells instead of Processes**
- **Semantic Streams instead of Byte Pipes**
- **Memory as a First-Class System Resource**
- **Provenance as a Default Property**

## Core Idea

Preserve the composability and minimalism of UNIX, but redefine the primitives around:

- state
- transformation
- memory
- provenance
- policy

## Structure

```
docs/
  rfcs/                          # Formal RFC specifications
    RFC-INDEX.md                 # Master index of all RFCs
    RFC-0001 through RFC-0006    # Architecture specifications
  diagrams/                      # Architecture diagrams (Mermaid)
  AI_NATIVE_OS_ARCHITECTURE_PROPOSAL.md

src/anunix/                      # Phase 1 userland prototype (Python)
  core/                          # Foundation types, config, events
  state/                         # State Object layer
  memory/                        # Memory Control Plane
  execution/                     # Execution Cell Runtime
  routing/                       # Routing Engine
  scheduler/                     # Unified Scheduler
  validation/                    # Validation layer
  network/                       # Network Plane
  posix/                         # POSIX compatibility

cli/                             # CLI entry point and commands
config/                          # Default and example configuration
tests/                           # Unit and integration tests
examples/                        # Example workflows
```

## Roadmap

### RFCs

- [RFC-0001: Architecture Thesis](docs/rfcs/RFC-0001-architecture-thesis.md)
- [RFC-0002: State Object Model](docs/rfcs/RFC-0002-state-object-model.md)
- [RFC-0003: Execution Cell Runtime](docs/rfcs/RFC-0003-execution-cell-runtime.md)
- [RFC-0004: Memory Control Plane](docs/rfcs/RFC-0004-memory-control-plane.md)
- [RFC-0005: Routing Plane and Unified Scheduler](docs/rfcs/RFC-0005-routing-and-scheduler.md)
- [RFC-0006: Network Plane and Federated Execution](docs/rfcs/RFC-0006-network-plane.md)

### Implementation Phases

1. **Phase 1 — Userland Prototype** (current)
   - Linux base (Fedora preferred)
   - State Object layer, Memory Control Plane, Execution Cell Runtime
   - Model router, Unified Scheduler, Validation layer
   - CLI and local REST API
2. **Phase 2 — Deep Integration**
   - Scheduler hooks, filesystem overlay, network-aware routing
3. **Phase 3 — Kernel Extensions**
   - State object primitives, memory hooks, scheduling hints

## Technology Stack (Phase 1)

| Concern | Choice |
|---------|--------|
| Language | Python 3.11+ |
| Data models | Pydantic v2 |
| Graph memory | NetworkX |
| Embeddings | ChromaDB |
| CLI | Click |
| Config | TOML |
| IPC | ZeroMQ |
| Model integration | litellm |
| Testing | pytest |
| Logging | structlog |

## Why This Matters

AI-native workloads require:
- semantic awareness
- recursive execution
- probabilistic computation
- memory orchestration

This repo explores how an OS should evolve to support that natively.

## License

MIT
