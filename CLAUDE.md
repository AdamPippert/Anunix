# CLAUDE.md — Anunix Development Guide

## Goals

Anunix is an AI-native operating system framework that redefines UNIX primitives for distributed, probabilistic, memory-aware computation. The immediate goal is a **Phase 1 userland prototype** in Python that demonstrates:

1. State Objects as the canonical data abstraction (replacing passive files)
2. Execution Cells as the work abstraction (replacing flat processes)
3. A Memory Control Plane with tiered, trust-aware, retrievable memory
4. A Routing Engine that selects engines by capability (not brand)
5. A Unified Scheduler binding routes to heterogeneous resources
6. A Network Plane enabling federated execution and memory

Success criteria: the meeting recording workflow runs end-to-end — audio → transcript → summary → action items → memory integration → retrieval.

## Environment

- **Language**: Python 3.11+
- **Build**: `hatchling` via `pyproject.toml`
- **Install**: `pip install -e ".[all]"` for full dev setup
- **Test**: `make test` (pytest with asyncio)
- **Lint**: `make lint` (ruff)
- **Type check**: `make typecheck` (mypy strict)
- **Config**: TOML (`config/default.toml`), loaded via Pydantic models in `src/anunix/core/config.py`

### Key Dependencies

| Package | Purpose |
|---------|---------|
| pydantic v2 | All data models, validation, serialization |
| networkx | Graph memory backend |
| chromadb | Embedding/vector storage |
| click | CLI framework |
| litellm | Unified model API (OpenAI, Anthropic, Ollama) |
| pyzmq + msgpack | Inter-service IPC |
| structlog | Structured logging |

### Project Layout

```
src/anunix/core/       → Foundation (types, errors, config, events)
src/anunix/state/      → State Object model and storage backends
src/anunix/memory/     → Memory Control Plane (tiers, taxonomy, graph, embeddings)
src/anunix/execution/  → Execution Cell Runtime (cells, contracts, streams, trace)
src/anunix/routing/    → Routing Engine (registry, classifier, scorer, strategies)
src/anunix/scheduler/  → Unified Scheduler (queue, resources, bindings)
src/anunix/validation/ → Validation layer (rules, schema checks, trust scoring)
src/anunix/network/    → Network Plane (peers, transport, replication, reconciliation)
src/anunix/posix/      → POSIX compatibility (FUSE, process adapter)
cli/                   → Click CLI entry point and command groups
config/                → Default and example TOML configuration
tests/unit/            → Per-module unit tests
tests/integration/     → Cross-module integration tests
docs/rfcs/             → Formal architecture specifications (RFC-0001 through RFC-0006)
```

### Branch

All development happens on `claude/ai-native-os-rfc-8emkB`. Push with `git push -u origin claude/ai-native-os-rfc-8emkB`.

## Preferences

### Architecture

- Every data model uses **Pydantic BaseModel** — no plain dicts for structured data
- All ID types are opaque strings with a prefix (`so_`, `cell_`, `eng_`, `plan_`, `trace_`, `node_`)
- The **State Object** is the universal data carrier — all persisted or shared data flows through it
- Modules communicate through well-defined interfaces, not direct imports of internals
- Async-first design using `asyncio` for any I/O-bound or concurrent operations
- Configuration is always loaded from TOML via the config system, never hardcoded

### Coding Style

- Type annotations everywhere — mypy strict mode must pass
- Ruff for formatting and linting (line length 100)
- Use enums for finite value sets (status codes, strategy names, etc.)
- Prefer composition over inheritance
- Every public function in a module should be importable from the module's `__init__.py`
- Tests are mandatory for any new functionality — one test file per source module minimum

### RFCs Are the Spec

The `docs/rfcs/` directory is the authoritative specification. When implementing a module:
1. Read the corresponding RFC first
2. Use the schemas and field names from the RFC
3. Follow the lifecycle and state machine definitions exactly
4. Implement the API surface described in the RFC

### Commit Style

- `feat:` for new functionality
- `docs:` for documentation changes
- `fix:` for bug fixes
- `refactor:` for restructuring without behavior change
- Commit messages explain the "why" in 1-2 sentences

## Anti-patterns

- **No god objects** — each module owns its domain; don't put routing logic in the state module
- **No silent failures** — use the exception hierarchy in `core/errors.py`; raise explicitly
- **No untyped dicts** — if data has structure, model it with Pydantic
- **No implicit network calls** — all remote operations must go through the Network Plane with policy checks
- **No validation bypass** — outputs from model/probabilistic sources must pass through the validation layer before memory promotion
- **No hardcoded model names** — use the capability registry and routing engine; engines are selected by capability
- **No monolithic implementations** — keep functions small and composable (UNIX philosophy)
- **No premature optimization** — Phase 1 is about getting the abstractions right, not performance
- **No state leakage** — Execution Cells should not share mutable state; all communication goes through State Objects or Semantic Streams
