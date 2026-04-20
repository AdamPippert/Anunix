# RFC-0016: Cell Execution Language (CEXL)

**Status**: Draft  
**Author**: Adam Pippert  
**Created**: 2026-04-20  
**Depends on**: RFC-0003 (Execution Cell Runtime), RFC-0002 (State Object Model), RFC-0009 (Agent Memory)

---

## Abstract

RFC-0016 specifies CEXL (Cell Execution Language): a Lisp-like S-expression DSL for composing, orchestrating, and recursively planning execution cells at Layer 2 — between the kernel's cell runtime (RFC-0003) and model-level orchestration.

CEXL is not a general-purpose language.  It is a narrow, typed IR for cell graphs: spawn, sequence, recurse, critic-loop, budget, and LLM invocation.  Programs are serialized as State Objects, traced as provenance chains, and evaluated by the cell runtime without a separate interpreter process.

---

## Motivation

The existing cell runtime (RFC-0003) executes cells imperatively: each cell is a C function with defined inputs and outputs.  Composing complex multi-step agent behaviors today requires writing C code.  There is no standard representation for a "plan" that can be stored, inspected, versioned, replayed, or self-modified by a running agent.

CEXL provides that representation.  An agent that needs to:
- Decompose a goal into sub-tasks
- Spawn parallel workers
- Critique its own output and recurse
- Stop when a budget runs out

...can express this as a CEXL program, store it as a State Object, and submit it to the cell runtime for evaluation.  The runtime evaluates CEXL natively — no bytecode VM, no foreign interpreter, no subprocess.

---

## Design

### 6.1 S-Expression Grammar

CEXL programs are lists.  An atom is a symbol, integer, float (as uint32 bits), or string.  A list is `(op arg1 arg2 ...)`.

```
expr   ::= atom | list
atom   ::= symbol | integer | string
list   ::= '(' op expr* ')'
op     ::= cell | spawn | seq | recurse | critic-loop
         | with-budget | llm | checkpoint | if | let | get | set
```

Only a fixed set of operators exists.  User-defined functions are not supported — CEXL is not Turing-complete by design.  Recursion is bounded by `max-depth` in `recurse` and by the budget in `with-budget`.

### 6.2 Core Operators

#### `cell` — invoke a registered execution cell

```lisp
(cell "name" input-obj)
```

Looks up the cell named `name` in the cell store, creates an execution context with `input-obj` as the input State Object, runs the cell synchronously, returns the output State Object path.

#### `spawn` — parallel execution

```lisp
(spawn
  (cell "a" in-a)
  (cell "b" in-b)
  (cell "c" in-c))
```

Submits all child expressions to the scheduler simultaneously.  Returns a list of output paths.  Order of completion is not guaranteed; the runtime collects all results before continuing.

#### `seq` — sequential pipeline

```lisp
(seq
  (cell "tokenizer" input)
  (cell "encoder" _)
  (cell "decoder" _))
```

Each step's output is the next step's input.  `_` is a forward reference to the previous output.

#### `recurse` — self-referential planning

```lisp
(recurse
  :plan    "plan-cell"      ; cell that produces the next CEXL fragment
  :critic  "critic-cell"    ; cell that scores the current output
  :goal    0.9              ; target critic score (float as bits)
  :max-depth 8)             ; hard limit on recursion depth
```

The planner generates a CEXL fragment, the critic scores it.  If the score exceeds `:goal` the fragment is returned.  Otherwise the planner is re-invoked with the critic's feedback.  Terminates at `:max-depth` regardless of score.

This is the key operator for self-improving agents: the planner and critic are ordinary cells, the recursion is tracked as a provenance chain, and the terminal plan is a State Object that can be replayed.

#### `critic-loop` — evaluate-critique-revise

```lisp
(critic-loop
  :target   obj-path
  :critic   "critic-cell"
  :reviser  "reviser-cell"
  :rounds   3)
```

Runs critic → reviser → critic → ... for up to `:rounds` iterations.  Each iteration stores a versioned State Object.  Returns the final revised object.

#### `with-budget` — resource-bounded execution

```lisp
(with-budget
  :tokens 4096
  :time-ms 5000
  (cell "inference" prompt))
```

The enclosed expression is given a budget.  If the budget is exhausted, the expression is cancelled and `nil` is returned.  The budget is tracked in the cell's execution context and decremented by each LLM call and each `cell` invocation.

#### `llm` — model invocation

```lisp
(llm
  :prompt  "summarize this:"
  :context obj-path
  :model   "default"
  :max-tokens 256)
```

Routes to the Routing Plane (RFC-0005) engine selection.  Returns a State Object containing the model's response.  Budget is decremented by actual tokens consumed.

#### `checkpoint` — durable save point

```lisp
(checkpoint "step-name" expr)
```

Evaluates `expr`, seals the result as a State Object at `default:/checkpoints/<name>`, and returns the path.  If the cell crashes and is restarted, the checkpoint is found and evaluation resumes from the next step.

#### `if`, `let`, `get`, `set` — structural control

```lisp
(if (get :score result) > 0.8
    (checkpoint "good" result)
    (recurse ...))

(let ((x (cell "extractor" input)))
  (spawn
    (cell "a" x)
    (cell "b" x)))

(set "default:/state/key" value)
(get :field obj-path)
```

These are intentionally minimal.  `get` reads a field from a State Object's metadata.  `set` writes to the object store.  `if` uses a simple three-value comparison.

### 6.3 Serialization

CEXL programs are stored as State Objects with `dtype = ANX_CEXL`.  The payload is the UTF-8 S-expression text.  The parser is a 200-line recursive descent reader — no external dependencies.

```
anx> so create default:/plans/summarize-loop
anx> write default:/plans/summarize-loop "(critic-loop ...)"
anx> cell run default:/plans/summarize-loop
```

### 6.4 Runtime Integration

The cell runtime evaluates CEXL directly:

```c
int anx_cexl_eval(const char *src, size_t len,
                   const char *input_path,
                   char *output_path, size_t output_path_max);
```

Steps:
1. Parse S-expression into `struct cexl_node` tree (stack-allocated, depth ≤ 32)
2. Walk the tree, dispatch each operator to its C handler
3. Operators that spawn cells call `anx_cell_submit()` (RFC-0003)
4. Budget tracking via thread-local counter (decremented by each LLM call)
5. Provenance: each `cell` invocation appends to the active provenance chain (RFC-0009)

### 6.5 Provenance Tracking

Every CEXL evaluation generates a provenance record:

```
default:/prov/<eval-id>/plan      — the CEXL source text
default:/prov/<eval-id>/trace     — ordered list of cell invocations with timing
default:/prov/<eval-id>/result    — path to final output
default:/prov/<eval-id>/score     — critic score if critic-loop was used
```

This makes self-modifying agents auditable: the full plan + execution trace is stored for every run.

---

## Implementation Plan

### Phase 1: Parser + AST (~300 lines)
- `kernel/core/exec/cexl_parse.c` — S-expression reader, `struct cexl_node`
- `kernel/include/anx/cexl.h` — node types, public eval API
- Tests: round-trip parse/print, error cases

### Phase 2: Core operators (~400 lines)
- `kernel/core/exec/cexl_eval.c` — `cell`, `seq`, `spawn`, `if`, `let`, `get`, `set`
- `kernel/core/exec/cexl_budget.c` — budget tracking
- Tests: seq pipeline, spawn collection, budget exhaustion

### Phase 3: Recursive operators (~350 lines)
- `kernel/core/exec/cexl_recurse.c` — `recurse`, `critic-loop`
- `kernel/core/exec/cexl_checkpoint.c` — checkpoint save/resume
- Tests: recurse with mock planner/critic, checkpoint resume after crash

### Phase 4: LLM integration + shell (~250 lines)
- Wire `llm` operator to `anx_model_infer()` (Routing Plane)
- `kernel/core/tools/cexl.c` — `cexl eval|run|trace|list` shell commands
- Tests: full `critic-loop` with local model

### Estimated effort

| Phase | Lines | Deliverable |
|-------|-------|-------------|
| 1 | ~300 | Parser + AST |
| 2 | ~400 | Core operators |
| 3 | ~350 | Recursive operators |
| 4 | ~250 | LLM + shell |
| **Total** | **~1300** | **Full CEXL runtime** |

---

## Design Constraints

- **No heap allocation during eval**: the AST is stack-allocated (max depth 32 nodes at 64 bytes each = 2 KiB).  Cell dispatch is the only allocation site, and cells use the standard cell allocator.
- **No Turing completeness**: no loops without a bound, no tail recursion without a depth limit.  CEXL programs always terminate.
- **No foreign functions**: every callable must be a registered execution cell.  No raw function pointers, no dlopen.
- **Compatible with `-mgeneral-regs-only`**: no float registers.  Budget values and critic scores are stored as uint32_t IEEE 754 bit patterns.

---

## Example: Self-Improving Summarizer

```lisp
(with-budget :tokens 8192 :time-ms 10000
  (critic-loop
    :target (seq
               (cell "chunker" input)
               (cell "summarizer" _))
    :critic  "length-critic"
    :reviser "rewriter"
    :rounds  3))
```

This program:
1. Chunks the input, summarizes it
2. Critiques the summary (e.g., checks length, coherence)
3. Rewrites the summary based on critique feedback
4. Repeats up to 3 times
5. Terminates early if budget exhausted
6. Returns the final summary as a State Object

All intermediate outputs are State Objects, the full provenance chain is stored, and the entire program is itself a State Object that can be versioned and replayed.

---

## Success Criteria

```
anx> cexl eval "(seq (cell \"echo\" default:/test) (checkpoint \"done\" _))"
eval: seq → echo → checkpoint
result: default:/checkpoints/done
anx> cexl trace last
  0.0ms  cell:echo     in=default:/test   out=default:/tmp/0
  0.1ms  checkpoint:done  saved=default:/checkpoints/done
```
