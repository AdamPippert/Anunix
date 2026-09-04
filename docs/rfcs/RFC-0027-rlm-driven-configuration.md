# RFC-0027: RLM-Driven Configuration and Transformation

| Field      | Value                                                                  |
|------------|------------------------------------------------------------------------|
| RFC        | 0027                                                                   |
| Title      | RLM-Driven Configuration and Transformation                           |
| Author     | Adam Pippert                                                           |
| Status     | Draft                                                                  |
| Created    | 2026-09-04                                                             |
| Updated    | 2026-09-04                                                             |
| Depends On | RFC-0002, RFC-0003, RFC-0005, RFC-0020, RFC-0021, RFC-0026            |
| Blocks     | —                                                                      |

---

## Executive Summary

The goal is a programmable, self-refining layer for **configuration** and **data
transformation** as operating-system primitives — the Prime-Agent / RLM idea of
"persistent programmable context that composes tools and refines itself,"
remixed for Anunix.

The important finding is that **most of the machinery already exists** in the
tree, so this RFC composes rather than invents:

| Prime-Agent / RLM idea | Already in Anunix |
|------------------------|-------------------|
| Persistent programmable reasoning kernel | **RLM harness** — `anx_rlm_*` (`anx/rlm.h`): multi-step rollouts on Execution Cells, pluggable inference adapter, tool injection, batch runner |
| Programmable surface without a Python interpreter | **CEXL** (RFC-0016) — a Lisp IR for recursive cell orchestration |
| Long-running / self-refinement (`/refine`) | **IBAL loop** (RFC-0020) + JEPA world model + **PAL** cross-session priors + **CEXL critic** |
| Recursive sub-agents + messaging | **Agent cells** (`anx_agent_cell_dispatch`) + Network Plane |
| Programmatic tool/data ops | **Routing Plane** engines (RFC-0005) + tensor ops (RFC-0013) + State Object CRUD |
| Inference execution | **anxml** (RFC-0021) |

So the delta is not "add an RLM." It is a thin **configurator** and
**data-manipulator** that bind the existing RLM harness to a *governed* update
path. This RFC's implementation delivers the **configurator** first; the
data-manipulator is specified here and deferred to a follow-on.

## Design Principle: Governance, Not a New Primitive

A self-modifying agent that can rewrite configuration or transform data is only
safe if every change is **evidence-backed, validated, versioned, and
auditable**. RFC-0026 already provides exactly that shape: a change is a *patch*
that must clear a *commit gate* (provider authority + validators) before it
merges, and every merge is materialized as a State Object with provenance.

Therefore configuration is **not** a new object type. A setting is a node in the
`config.*` slice of a dedicated world graph:

- identity = the node oid (RFC-0002),
- value = a node property,
- every change = a patch through the commit gate (RFC-0026),
- history = the graph version plus per-object provenance.

The reasoning model *proposes*; the gate *decides*. A hallucinated or unsafe
configuration change is rejected by the same validator that rejects a
hand-typed one.

## Configuration (this RFC's MVP)

### The `config.*` slice

`anx_config_set(area, key, value)` finds or adds the node
`config.<area>`/`<key>`, sets its `value` property, and commits the patch
through the gate as the `config.writer` provider (which holds `config.*` write
authority — a prefix-wildcard capability added to the manifest model).
`anx_config_get` / `anx_config_list` read the slice back.

### Schema validation at the gate

`anx_config_declare(area, key, type, spec)` registers a typed schema
(`string` | `int` | `bool` | `enum`). The `config.schema` validator runs on the
commit gate and rejects any `config.*` value that fails its declared type —
non-integers for an `int`, values outside the set for an `enum`, empties for
anything. Undeclared keys are accepted as free strings. Governance is opt-in
per key but enforced uniformly once declared.

### RLM configurator

`anx_configurator_run(goal)` is the self-refining path:

1. the goal becomes a prompt State Object;
2. the **existing RLM harness** runs a rollout (the model client in production,
   a test double under test) with a system prompt constraining output to
   `SET <area> <key> <value>` directives;
3. the final response is parsed into a single atomic patch and submitted through
   the **same commit gate** — so the model's proposal is advisory and the
   schema validator has the final say.

`anx_configurator_apply(text)` is the deterministic half: it applies a
directive block as one all-or-nothing gated patch, ignoring prose. This is what
makes the loop safe to run autonomously — the worst a bad rollout can do is
propose a patch the gate rejects.

## Data Transformation (specified, deferred)

The symmetric case: an RLM cell attached to a route that transforms State
Objects. The same shape applies — the model emits a **verified op sequence**
(CEXL, RFC-0016, or a composition of Routing-Plane engine calls and tensor
ops), which is applied under a gate and recorded with provenance, and can refine
its own pipeline across turns via the IBAL loop. Because CEXL and the routing
engines already exist, this is a binding layer, not new compute. It is deferred
to a follow-on so the configurator lands first.

## Security Model

- **Provider authority.** Writers declare a manifest; `config.writer` and
  `config.rlm` are scoped to `config.*` and cannot touch other world slices.
- **Commit gate.** Every change — human or model — passes the schema validator.
- **Provenance.** Each committed setting is a State Object carrying the
  authoring provider, so an evidence trail exists for every change.
- **Resource budgets.** RLM rollouts inherit Execution Cell budgets and the
  scheduler's queue classes; a configurator rollout is bounded like any cell.

## Scope and Non-Goals

**In scope (this RFC's code):** configuration as a governed `config.*` world
slice (`anx_config_*`), the schema validator, and the RLM configurator
(`anx_configurator_*`), with the `config` ansh tool and host + QEMU tests.

**Deferred:** the data-manipulator binding (RLM-emitted CEXL/route transforms
over State Objects); wiring the configurator's refinement into the IBAL loop /
PAL priors for autonomous long-horizon tuning; a richer schema language
(ranges, cross-key invariants).

**Explicitly not built:** a new RLM primitive (the harness exists), a Python
interpreter in the kernel (CEXL is the programmable surface), or a config object
type separate from the world graph.

## Implementation Notes

Configuration uses a dedicated world graph so it does not compete with the
default graph's node budget; RFC-0026's conservative caps apply and grow with a
dedicated arena. The proposal grammar is deliberately line-oriented and
whitespace-delimited (`SET <area> <key> <value>`) so it is trivial to emit and
parse without a tokenizer, and unparsable lines are ignored rather than
erroring — a model may wrap directives in prose.
