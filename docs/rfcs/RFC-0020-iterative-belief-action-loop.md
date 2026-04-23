# RFC-0020: Iterative Belief-Action Loop — EBM/JEPA/LLM Hybrid Execution Engine

| Field      | Value                                                                                             |
|------------|---------------------------------------------------------------------------------------------------|
| RFC        | 0020                                                                                              |
| Title      | Iterative Belief-Action Loop — EBM/JEPA/LLM Hybrid Execution Engine                              |
| Author     | Adam Pippert                                                                                      |
| Status     | Draft                                                                                             |
| Created    | 2026-04-22                                                                                        |
| Updated    | 2026-04-22                                                                                        |
| Depends On | RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0007, RFC-0009, RFC-0013, RFC-0016, RFC-0018, JEPA   |
| Blocks     | Anunix-World project implementation                                                               |

---

## Executive Summary

A language-model call is a function, not a reasoning process.  Calling a model once, parsing its output, and acting on it misses the structural properties of deliberate reasoning: hypothesis formation, belief revision, multi-hypothesis evaluation, anomaly detection, and energy-minimizing convergence.  The result is brittle agents that confidently emit wrong answers.

RFC-0020 introduces the **Iterative Belief-Action Loop (IBAL)** — a kernel-native closed-loop execution engine that implements deliberate reasoning as a first-class OS workflow.  IBAL is not an agent framework.  It is a typed execution graph of specialized cells operating over a shared, versioned object graph, with explicit loop control, energy-based arbitration, and world-model-informed proposal generation.

The design combines three complementary ideas:

- **JEPA world model** (from the JEPA kernel subsystem): latent-state prediction in embedding space, providing a learned prior over how system/world state evolves under actions.
- **Energy-Based Model (EBM) scoring**: a family of scalar energy functions that evaluate candidate world hypotheses for consistency, goal alignment, constraint compliance, and epistemic uncertainty.
- **LLM reasoning cells**: language-model inference for natural-language planning, tool selection, and context synthesis — grounded by the JEPA prior and gated by energy scores.

The system is implemented in two layers:

1. **Anunix kernel extensions** — new object types (`ANX_OBJ_LOOP_SESSION`, `ANX_OBJ_BELIEF_STATE`, `ANX_OBJ_WORLD_PROPOSAL`, `ANX_OBJ_SCORE`, `ANX_OBJ_PLAN`, `ANX_OBJ_COUNTEREXAMPLE`) and a new `kernel/core/loop/` subsystem.
2. **Anunix-World project** — a separate, standalone project that implements the full IBAL workflow using those kernel primitives, the JEPA subsystem, and the existing cell/routing/memory infrastructure.

The key architectural principle: **symbolic governance remains in State Objects, Capabilities, and policy gates; learned inference lives in JEPA and EBM cells; natural-language reasoning lives in LLM cells; the loop supervisor orchestrates all three without any single component being authoritative alone.**

---

## 1. Problem Statement

### 1.1 Single-shot LLM reasoning is structurally inadequate

A single-turn LLM inference call has no mechanism for:

- Testing whether its output is consistent with a world model.
- Revising beliefs when contradictory evidence is encountered.
- Holding multiple hypotheses simultaneously and selecting the most energy-consistent one.
- Detecting when it has insufficient context to make a confident decision.
- Producing provenance for every intermediate belief update.

These failures are not fixable by better prompts.  They require a different execution model.

### 1.2 Agent frameworks paper over the problem

Existing agent frameworks (LangChain, AutoGen, etc.) add tool-calling loops around LLM inference.  The loop is unstructured: the LLM decides when to stop, what tools to call, and what to do with the results.  The kernel has no visibility into the loop, no ability to enforce resource budgets, no structured representation of intermediate beliefs, and no mechanism for energy-based arbitration.

### 1.3 Anunix has the right primitives but no loop primitive

Anunix already has:
- Content-addressed, provenance-bearing State Objects (RFC-0002)
- Lifecycle-managed, policy-governed Execution Cells (RFC-0003)
- Tiered memory with decay and promotion (RFC-0004)
- Type-aware routing with multi-objective scoring (RFC-0005)
- Capability-scoped access control (RFC-0007)
- Agent memory with graph structure (RFC-0009)
- JEPA latent-state world model (JEPA subsystem)
- Workflow graph execution (RFC-0018)

What is missing is a **first-class loop session primitive** — a kernel-visible object that represents an iterative reasoning episode with explicit iteration count, belief state, candidate hypotheses, energy scores, and halting policy.

---

## 2. Design Principles

1. **The loop is explicit and kernel-visible.** Every iteration is a distinct kernel event. Iteration count, current belief, candidate set, and energy scores are State Objects, not memory inside a running process.

2. **Symbolic governance is never delegated to learned models.** Capabilities, policy gates, and commit rules are enforced by the kernel. JEPA and EBM cells produce signals that inform decisions; they do not make them.

3. **Hypotheses are State Objects, not LLM outputs.** A world hypothesis is a typed `ANX_OBJ_WORLD_PROPOSAL` with provenance, a producing cell, and a score. It is content-addressed and retrievable.

4. **Energy is composable and inspectable.** Each energy function is a separate scoring cell producing an `ANX_OBJ_SCORE`. Arbitration is deterministic given the scores. The score history is traceable.

5. **Failed branches accumulate as negative knowledge.** Rejected hypotheses are not discarded. They are stored as `ANX_OBJ_COUNTEREXAMPLE` objects in the agent memory system (RFC-0009) and inform future loop iterations and JEPA training.

6. **The system degrades gracefully.** JEPA unavailable → proposals use retrieval only. EBM cells unavailable → arbitration falls back to LLM self-evaluation. Any single component failing does not halt the loop.

---

## 3. New Object Types

Add the following to `enum anx_object_type` in `kernel/include/anx/state_object.h`:

### 3.1 `ANX_OBJ_LOOP_SESSION`

Represents one iterative reasoning episode.

```
Fields:
  session_id      anx_oid_t    — unique session identifier
  parent_task_oid anx_oid_t    — the cell or workflow that spawned this session
  world_uri       char[128]    — active JEPA world profile URI
  iteration       uint32_t     — current iteration count
  max_iterations  uint32_t     — halting budget (0 = unlimited)
  halt_policy     enum         — HALT_ON_CONVERGENCE | HALT_ON_BUDGET |
                                 HALT_ON_CONFIDENCE | HALT_MANUAL
  halt_threshold  float        — energy delta threshold for HALT_ON_CONVERGENCE
  confidence_min  float        — minimum confidence for HALT_ON_CONFIDENCE
  branch_budget   uint32_t     — maximum parallel hypothesis branches
  active_belief   anx_oid_t    — current ANX_OBJ_BELIEF_STATE OID
  best_candidate  anx_oid_t    — current best ANX_OBJ_WORLD_PROPOSAL OID
  score_history   anx_oid_t[]  — ANX_OBJ_SCORE OIDs per iteration (max 64)
  capability_scope anx_oid_t   — bounding capability object for this session
  status          enum         — PENDING | RUNNING | HALTED | COMMITTED | ABORTED
  started_at      anx_time_t
  halted_at       anx_time_t
```

### 3.2 `ANX_OBJ_BELIEF_STATE`

The current working belief about the world/task at a given iteration.

```
Fields:
  session_oid     anx_oid_t    — parent loop session
  iteration       uint32_t     — which iteration produced this belief
  latent_oid      anx_oid_t    — JEPA latent vector (ANX_OBJ_JEPA_LATENT)
  context_oids    anx_oid_t[]  — retrieved context objects (max 32)
  context_count   uint32_t
  summary_oid     anx_oid_t    — LLM-produced natural-language summary
  uncertainty     float        — aggregate epistemic uncertainty (0-1)
  producer_cid    anx_cid_t    — cell that produced this belief
  parent_belief   anx_oid_t    — prior iteration's belief (null if iteration 0)
```

### 3.3 `ANX_OBJ_WORLD_PROPOSAL`

A candidate world hypothesis: a proposed next state, action, or plan.

```
Fields:
  session_oid     anx_oid_t    — parent loop session
  iteration       uint32_t
  source          enum         — SOURCE_JEPA | SOURCE_LLM | SOURCE_RETRIEVAL |
                                 SOURCE_SYMBOLIC | SOURCE_SIMULATION
  latent_oid      anx_oid_t    — predicted JEPA latent (if SOURCE_JEPA)
  content_oid     anx_oid_t    — structured proposal content (ANX_OBJ_STRUCTURED_DATA)
  action_id       uint32_t     — proposed JEPA action (if applicable)
  score_oids      anx_oid_t[]  — energy scores assigned to this proposal (max 8)
  score_count     uint32_t
  aggregate_score float        — weighted sum of all energy scores
  status          enum         — CANDIDATE | SELECTED | REJECTED | COMMITTED
  producer_cid    anx_cid_t
```

### 3.4 `ANX_OBJ_SCORE`

A typed scalar or vector score produced by one energy cell against one proposal.

```
Fields:
  session_oid     anx_oid_t
  target_oid      anx_oid_t    — the proposal or belief being scored
  scorer_id       char[64]     — "world_consistency" | "goal_alignment" |
                                 "constraint_compliance" | "epistemic_uncertainty" |
                                 "operational_executability"
  scalar          float        — aggregate score (lower energy = better fit)
  components      float[8]     — sub-scores for diagnostics
  component_count uint32_t
  threshold_class enum         — ACCEPT | MARGINAL | REJECT
  confidence      float        — scorer's confidence in its own score
  producer_cid    anx_cid_t
  scored_at       anx_time_t
```

### 3.5 `ANX_OBJ_PLAN`

A committed or candidate plan: an ordered sequence of actions with dependencies.

```
Fields:
  session_oid     anx_oid_t
  source_proposal anx_oid_t    — the ANX_OBJ_WORLD_PROPOSAL this plan derives from
  steps           anx_oid_t[]  — ordered step OIDs (ANX_OBJ_STRUCTURED_DATA, max 32)
  step_count      uint32_t
  total_cost      uint32_t     — estimated cost in scheduler units
  confidence      float
  status          enum         — CANDIDATE | COMMITTED | EXECUTING | COMPLETED |
                                 FAILED | ROLLED_BACK
  committed_at    anx_time_t
  rollback_oid    anx_oid_t    — snapshot OID for rollback (if applicable)
```

### 3.6 `ANX_OBJ_COUNTEREXAMPLE`

A rejected hypothesis or failed plan stored as negative knowledge.

```
Fields:
  session_oid     anx_oid_t
  rejected_oid    anx_oid_t    — the rejected ANX_OBJ_WORLD_PROPOSAL or ANX_OBJ_PLAN
  reason          enum         — ENERGY_TOO_HIGH | CONSTRAINT_VIOLATED |
                                 CAPABILITY_DENIED | DIVERGENCE_TOO_HIGH |
                                 EXECUTION_FAILED | HUMAN_REJECTED
  rejection_score float        — the energy score that caused rejection
  context_summary char[512]    — human-readable summary of why this failed
  admitted_to_mem bool         — whether this was written to agent memory (RFC-0009)
```

---

## 4. Execution Cell Topology

### 4.1 Cell inventory

The IBAL workflow uses eleven specialized cell types.  These map to existing `anx_cell_type` entries where possible; new entries are marked NEW.

| Cell | `anx_cell_type` | Purpose |
|---|---|---|
| `ingest_cell` | `ANX_CELL_TASK_EXECUTION` | Normalize input → observational objects |
| `context_cell` | `ANX_CELL_TASK_RETRIEVAL` | Assemble belief from memory, retrieval, graph |
| `jepa_proposal_cell` | `ANX_CELL_TASK_EXECUTION` | Generate world proposals via JEPA predict |
| `llm_proposal_cell` | `ANX_CELL_MODEL_SERVER` | Generate proposals via LLM reasoning |
| `retrieval_proposal_cell` | `ANX_CELL_TASK_RETRIEVAL` | Generate proposals via semantic search |
| `recurrent_update_cell` | **NEW** `ANX_CELL_BELIEF_UPDATE` | Merge proposals → update belief state |
| `world_energy_cell` | `ANX_CELL_TASK_VALIDATION` | Score world consistency |
| `goal_energy_cell` | `ANX_CELL_TASK_VALIDATION` | Score goal alignment |
| `constraint_energy_cell` | `ANX_CELL_TASK_POLICY_CHECK` | Score constraint compliance |
| `uncertainty_energy_cell` | `ANX_CELL_TASK_VALIDATION` | Score epistemic uncertainty |
| `arbitration_cell` | **NEW** `ANX_CELL_ARBITRATION` | Aggregate scores → halt/continue/branch |
| `commit_cell` | `ANX_CELL_TASK_SIDE_EFFECT` | Write plan, promote traces, record negatives |
| `consolidation_cell` | `ANX_CELL_TASK_MEMORY_UPDATE` | Compress traces → agent memory |

New cell types added to `enum anx_cell_type`:
- `ANX_CELL_BELIEF_UPDATE` — recurrent workspace update with provenance
- `ANX_CELL_ARBITRATION` — score aggregation and halting decision
- `ANX_CELL_LOOP_SUPERVISOR` — loop session lifecycle management (spawns all others)

### 4.2 Execution graph

```
TRIGGER (manual | event | cron)
  │
  ▼
INGEST CELL
  Inputs:  raw input (user message, tool result, sensor event)
  Outputs: ANX_OBJ_BYTE_DATA (normalized),
           session bootstrap: ANX_OBJ_LOOP_SESSION
  │
  ▼
CONTEXT ASSEMBLY CELL                         ◄─── RFC-0009 agent memory
  Inputs:  session OID, current observation
  Outputs: ANX_OBJ_BELIEF_STATE (iteration 0)
           context bundle (retrieved objects)
  │
  ▼
┌─────────────────────────────── LOOP ────────────────────────────────┐
│                                                                      │
│  ┌──────────────┬──────────────┬─────────────────┐                  │
│  │ JEPA         │ LLM          │ RETRIEVAL        │  (fan-out)       │
│  │ PROPOSAL     │ PROPOSAL     │ PROPOSAL         │                  │
│  └──────┬───────┴──────┬───────┴────────┬─────────┘                  │
│         │              │                │                            │
│         └──────────────┴────────────────┘                            │
│                         │ (fan-in: ANX_OBJ_WORLD_PROPOSAL[])        │
│                         ▼                                            │
│             RECURRENT UPDATE CELL                                    │
│             Inputs:  current belief + proposals                      │
│             Outputs: updated ANX_OBJ_BELIEF_STATE                   │
│                      candidate ANX_OBJ_WORLD_PROPOSAL[]             │
│                         │                                            │
│         ┌───────────────┼───────────────┬──────────────────┐        │
│         ▼               ▼               ▼                  ▼        │
│    WORLD ENERGY    GOAL ENERGY   CONSTRAINT ENERGY  UNCERTAINTY     │
│    CELL            CELL          CELL                ENERGY CELL    │
│    (ANX_OBJ_SCORE) (ANX_OBJ_SCORE) (ANX_OBJ_SCORE) (ANX_OBJ_SCORE)│
│         └───────────────┴───────────────┴──────────────────┘        │
│                         │ (fan-in: all scores)                      │
│                         ▼                                            │
│               ARBITRATION CELL                                       │
│               Inputs:  all scores + belief + proposals               │
│               Decision: CONTINUE | HALT | BRANCH | ABORT            │
│                         │                                            │
│               ┌─────────┴──────────┐                                │
│               │ CONTINUE           │ HALT / ABORT                   │
│               │ (loop back)        │                                 │
│               ▼                    ▼                                 │
└──────── increment iteration  COMMIT CELL ──────────────────────────┘
                                     │
                            ┌────────┴────────┐
                            ▼                 ▼
                     write ANX_OBJ_PLAN   record ANX_OBJ_COUNTEREXAMPLE
                     promote to memory    (rejected hypotheses)
                     emit action/output
                            │
                            ▼
                     CONSOLIDATION CELL (async, low-priority)
                     compress traces → agent memory (RFC-0009)
```

### 4.3 Branch/merge semantics

When arbitration decides `BRANCH`:
1. The loop supervisor forks the current `ANX_OBJ_BELIEF_STATE` into N copies (one per top-N candidates).
2. Each branch runs one full iteration independently, scheduled in parallel.
3. All branches produce scores; arbitration selects the winner.
4. Winning branch merges its belief state into the main workspace.
5. Losing branches produce `ANX_OBJ_COUNTEREXAMPLE` objects.

This requires a new scheduler primitive: **branch group** — a set of cells with a shared join point. The scheduler tracks the group; the arbitration cell is released only when all branches reach their energy scoring step.

---

## 5. Memory Plane Policy

| Tier | Contents | TTL policy |
|---|---|---|
| L0 (hot workspace) | Active belief state, current candidates, live scores | Duration of one loop iteration; evicted on iteration advance |
| L1 (session cache) | All objects from current session, recent tool results, failed candidates | Duration of session + 30 min; promoted to L2 on commit |
| L2 (durable semantic) | Committed plans, validated traces, compressed belief history | 30 days; promoted to L3 on reinforcement |
| L3 (long-term semantic) | Consolidated episodic memory (RFC-0009 integration), world model checkpoints | Indefinite; decays by access frequency |
| L4 (graph/structured) | Counterexample corpus, negative knowledge, constraint graph | Indefinite; low-decay (high value for future training) |
| L5 (remote/federated) | Cross-node shared knowledge, JEPA training datasets | External; no local TTL |

---

## 6. Routing Plane Task Families

Add the following route families to the routing plane taxonomy:

| Route key | Description | Default engine class |
|---|---|---|
| `route:proposal.jepa` | JEPA-based world proposal | `ANX_ENGINE_LOCAL_MODEL` (JEPA) |
| `route:proposal.llm` | LLM-based world proposal | `ANX_ENGINE_REMOTE_MODEL` or `LOCAL_MODEL` |
| `route:proposal.retrieval` | Semantic retrieval proposal | `ANX_ENGINE_RETRIEVAL_SERVICE` |
| `route:proposal.symbolic` | Symbolic/graph transition proposal | `ANX_ENGINE_GRAPH_SERVICE` |
| `route:update.recurrent` | Recurrent belief update | `ANX_ENGINE_LOCAL_MODEL` |
| `route:score.world` | World consistency energy scoring | `ANX_ENGINE_LOCAL_MODEL` (JEPA divergence) |
| `route:score.goal` | Goal alignment scoring | `ANX_ENGINE_DETERMINISTIC_TOOL` or `LOCAL_MODEL` |
| `route:score.constraint` | Constraint compliance scoring | `ANX_ENGINE_DETERMINISTIC_TOOL` |
| `route:score.uncertainty` | Epistemic uncertainty scoring | `ANX_ENGINE_LOCAL_MODEL` |
| `route:arbitrate` | Score aggregation and halting decision | `ANX_ENGINE_DETERMINISTIC_TOOL` |
| `route:memory.consolidate` | Loop trace compression | `ANX_ENGINE_LOCAL_MODEL` (background) |

---

## 7. Capability Model

Loop sessions are capability-scoped.  The session's `capability_scope` field holds an `ANX_OBJ_CAPABILITY` OID that bounds:

- Which world model surfaces a session may query.
- Which memory tiers a session may read or write.
- Which action types a commit cell may emit.
- Which external tools or routes are callable within the loop.
- Maximum iteration count and branch budget.

This prevents unbounded loops, uncontrolled tool calls, and cross-session memory contamination without relying on prompt engineering.

---

## 8. Shell / API Commands

New `loop` and `world` command families for the Anunix shell and HTTP API:

```
loop create  [--world <uri>] [--max-iter N] [--halt <policy>] [--budget N]
loop step    <session-id>
loop status  <session-id>
loop score   <session-id> [--iter N]
loop branch  <session-id> --candidates N
loop merge   <session-id> --winner <proposal-oid>
loop halt    <session-id>
loop abort   <session-id>
loop trace   <session-id> [--iter N]
loop commit  <session-id> --plan <proposal-oid>

world propose  --session <id> --source <jepa|llm|retrieval|symbolic>
world score    --session <id> --proposal <oid> [--scorer <name>]
world inspect  --session <id> [--proposal <oid>]
world negatives --session <id>

belief show   <session-id> [--iter N]
belief diff   <session-id> --from N --to M
```

---

## 9. Project Structure: Anunix-World

The Anunix-World project implements the full IBAL workflow using the kernel primitives defined in this RFC.  It is a standalone project that links against or extends the Anunix kernel.

```
Anunix-World/
  kernel/                    ← kernel extensions (applied as patches to Anunix)
    include/anx/
      loop.h                 ← ANX_OBJ_LOOP_SESSION, ANX_OBJ_BELIEF_STATE, etc.
      ebm.h                  ← energy cell interfaces + scorer registration
      ibal.h                 ← IBAL loop supervisor public API
    core/
      loop/                  ← loop session subsystem (loop.c, loop_branch.c,
                                loop_commit.c, loop_score.c, loop_shell.c)
      ebm/                   ← energy-based model cells (ebm.c, ebm_world.c,
                                ebm_goal.c, ebm_constraint.c, ebm_uncertainty.c)

  src/                       ← userland Python (anunix_world package)
    anunix_world/
      cells/                 ← Python cell implementations (ingest, context, etc.)
      workflows/             ← workflow definitions
      models/                ← LLM and EBM model adapters
      training/              ← JEPA + EBM training utilities
      cli/                   ← loop / world / belief shell commands

  workflows/                 ← anx_wf_object definitions (JSON or C init code)
    ibal-default.wf          ← default full-stack IBAL workflow
    ibal-lite.wf             ← JEPA-only lightweight variant
    ibal-symbolic.wf         ← symbolic-only (no LLM) variant

  tests/                     ← unit + integration tests
  docs/                      ← design notes and usage guides
  Makefile                   ← builds kernel extensions + Python package
```

---

## 10. Implementation Phases

### Phase 1 — Kernel loop primitive (Anunix kernel)
- Add new object types to `state_object.h`
- Add `ANX_CELL_BELIEF_UPDATE`, `ANX_CELL_ARBITRATION`, `ANX_CELL_LOOP_SUPERVISOR`
- Implement `kernel/core/loop/` subsystem (loop session CRUD, iteration advance, halt)
- Add `loop create|step|halt|trace` shell commands
- No EBM, no JEPA integration yet — just the loop skeleton

### Phase 2 — Recurrent workspace (Anunix-World kernel extensions)
- Implement `ANX_OBJ_BELIEF_STATE` creation and versioning
- Implement `recurrent_update_cell` (belief merging, proposal aggregation)
- Implement `ANX_OBJ_WORLD_PROPOSAL` creation and lifecycle
- Wire JEPA `anx_jepa_predict()` as `jepa_proposal_cell`
- Implement `ANX_OBJ_SCORE` and the `world_energy_cell` using JEPA divergence
- Loop runs with JEPA proposal + world consistency score only

### Phase 3 — Full energy stack (Anunix-World `ebm/`)
- Implement `goal_energy_cell`, `constraint_energy_cell`, `uncertainty_energy_cell`
- Implement `arbitration_cell` with configurable halt policies
- Implement `ANX_OBJ_COUNTEREXAMPLE` recording
- Wire LLM proposal cell via RLM harness (RFC-0009 injection mode)
- Full IBAL loop operational

### Phase 4 — Branch/merge
- Add branch group primitive to scheduler
- Implement `loop branch|merge` commands
- Implement parallel candidate evaluation
- Persist rejected branches as `ANX_OBJ_COUNTEREXAMPLE`

### Phase 5 — Memory integration and consolidation
- Wire session traces to RFC-0009 agent memory
- Implement `consolidation_cell` for loop trace compression
- Use counterexample corpus as JEPA negative training signal
- Wire `loop commit` to promote validated plans to L3/L4 memory tiers

---

## 11. Relationship to Existing RFCs

| RFC | Relationship |
|---|---|
| RFC-0002 | New object types added to `anx_object_type` enum |
| RFC-0003 | New cell types `ANX_CELL_BELIEF_UPDATE`, `ANX_CELL_ARBITRATION`, `ANX_CELL_LOOP_SUPERVISOR` |
| RFC-0004 | Memory plane policy defined per loop object type (Section 5) |
| RFC-0005 | New routing task families added (Section 6) |
| RFC-0007 | Loop sessions are capability-scoped (Section 7) |
| RFC-0009 | Committed traces and counterexamples promote to agent memory |
| RFC-0013 | Belief states reference JEPA latent tensors (ANX_OBJ_JEPA_LATENT) |
| RFC-0016 | CEXL `critic-loop` operator maps to the IBAL arbitration step |
| RFC-0018 | IBAL loop runs as a Workflow Object with a loop-back edge |
| JEPA     | `jepa_proposal_cell` wraps `anx_jepa_predict()`; `world_energy_cell` wraps `anx_jepa_divergence()` |

---

## 12. What This Is Not

- **Not a replacement for RFC-0018 Workflow Objects.** IBAL is a specific workflow topology; it runs inside the workflow engine.
- **Not a general agent framework.** IBAL does not abstract over arbitrary agent architectures. It is one specific, well-defined loop structure.
- **Not an autonomous system.** Halting, committing, and branch merging all produce kernel-visible events. Human review gates can be inserted at any arbitration decision point using `ANX_WF_NODE_HUMAN_REVIEW`.
- **Not dependent on LLM availability.** JEPA and EBM cells are sufficient for a functional loop. LLM cells are optional enrichment.

---

## 13. Open Questions

1. **Concurrent sessions.** Should the loop supervisor support multiple active sessions per agent, or one at a time? Resource budgeting across sessions needs definition.
2. **EBM training.** The goal and uncertainty energy cells need trained models. Who trains them and how are they loaded? Likely via the same JEPA world_rebuild path.
3. **Cross-node belief propagation.** RFC-0006 network plane. Should belief states be synchronizable across federated nodes for distributed IBAL?
4. **CEXL integration depth.** RFC-0016 defines `critic-loop`. Should the IBAL loop be expressible entirely in CEXL, or is it a fixed kernel workflow?
