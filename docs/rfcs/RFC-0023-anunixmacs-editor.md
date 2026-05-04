# RFC-0023: anunixmacs — Object-Native Editor with eLISP

| Field      | Value                                                     |
|------------|-----------------------------------------------------------|
| RFC        | 0023                                                      |
| Title      | anunixmacs — Object-Native Editor with eLISP              |
| Author     | Adam Pippert                                              |
| Status     | Draft                                                     |
| Created    | 2026-05-02                                                |
| Updated    | 2026-05-04                                                |
| Depends On | RFC-0002, RFC-0003, RFC-0010, RFC-0012, RFC-0018          |

---

## Executive Summary

anunixmacs is the canonical text editor for Anunix. It is not an Emacs port. It is a from-scratch C implementation of the **shape** of Emacs (buffer, point, mark, mode, keymap, eLISP) bound to **State Objects** instead of files. A buffer is a view onto an OID. Saving means sealing a new object version. Every text mutation is a Cell side effect with provenance. eLISP primitives manipulate OIDs, capabilities, and workflow nodes directly — the editor's extension language is a first-class agent of the Anunix kernel.

Scope of v1 (this RFC):

- An ASCII text-buffer engine over `ANX_OBJ_BYTE_DATA` State Objects.
- A minimal eLISP interpreter (reader, evaluator, printer) — Lisp-1, lexically scoped, no macros, no continuations, reference-counted GC.
- Built-in primitives covering buffer I/O, point/mark, search, insertion/deletion, and OID/capability operations via the libanx adapter pattern.
- A specialized cell type `ANX_CELL_EDITOR` dispatched from `workflow_exec` on `editor-*` intents.
- A workflow template `anx:app/anunixmacs` that opens a buffer for an OID, applies an eLISP transformation, and writes the result.

Scope explicitly deferred:

- Macros, lexical closures over mutable cells, continuations, byte-compilation.
- Multibyte text (UTF-8 only, no shaping); display redisplay against an iface Surface.
- Mode system beyond fundamental-mode.

The primary differentiator is not the editor — it is the binding from eLISP forms to kernel objects. `(anx-open "models:/llama-3-8b/manifest")` returns a buffer over a State Object. `(anx-cap-grant ...)` manipulates capabilities. `(anx-wf-node-add ...)` edits a workflow visually then commits.

---

## 1. Problem Statement

Every editor in every prior OS treats text as bytes-on-disk. The editor sees a file, knows nothing of its semantic type, has no concept of the object's lineage, capabilities, or compute provenance. When an agent edits a workflow definition, it is editing a serialized blob, not the workflow object itself.

Anunix needs an editor that:

1. Treats the State Object as the unit of editing, not the file.
2. Versions buffers as object versions, with sealed history.
3. Lets agents script mutation in eLISP, with primitives that operate on OIDs and capabilities.
4. Composes with the workflow library: a workflow can include a `CELL_CALL` to "transform this OID with this eLISP form" as one node in a graph.

---

## 2. Architecture

### 2.1 Layers

```
┌──────────────────────────────────────────────────┐
│ User: keys + ~/.anunixmacs.el + M-: minibuffer   │
├──────────────────────────────────────────────────┤
│ Editor window (modeline · minibuffer · keymap)   │  [apps/anunixmacs/window.c]
│   takes over the WM terminal surface             │
├──────────────────────────────────────────────────┤
│ eLISP interpreter (reader → eval → printer)      │  [apps/anunixmacs/elisp.c]
│   special forms + built-ins (incl. editing       │
│   primitives bound to the active buffer)         │
├──────────────────────────────────────────────────┤
│ Buffer engine (gap buffer over byte data)        │  [apps/anunixmacs/buffer.c]
├──────────────────────────────────────────────────┤
│ Editor cell dispatch (workflow CELL_CALL hook)   │  [apps/anunixmacs/cell.c]
├──────────────────────────────────────────────────┤
│ State Object payload (ANX_OBJ_BYTE_DATA) +       │
│ namespace bind (default: "posix" — visible to    │
│ external programs via the POSIX shim)            │
└──────────────────────────────────────────────────┘
```

### 2.2 Objects

| Object type            | Purpose                                          |
|------------------------|--------------------------------------------------|
| `ANX_OBJ_BYTE_DATA`    | Raw text payload of a buffer (UTF-8)             |
| `ANX_OBJ_EXECUTION_TRACE` | One trace entry per editor cell invocation   |

No new object types are required for v1.

### 2.3 Cell Type

```c
ANX_CELL_EDITOR  /* added to enum anx_cell_type */
```

Dispatched by `workflow_exec.c` when a `CELL_CALL` node carries an intent prefixed with `editor-`.

### 2.4 Intent Verbs (v1)

| Intent             | Inputs               | Output                              |
|--------------------|----------------------|-------------------------------------|
| `editor-open`      | source OID           | buffer-state OID (snapshot)         |
| `editor-eval`      | buffer OID, form OID | new buffer-state OID                |
| `editor-save`      | buffer-state OID     | sealed `ANX_OBJ_BYTE_DATA` OID      |
| `editor-search`    | buffer OID, query OID | match-list OID                     |

---

## 3. eLISP Subset

### 3.1 Reader

S-expressions, single-line `;` comments, integers, double-quoted strings (with `\n`, `\t`, `\\`, `\"`), symbols (any non-whitespace non-paren), `'` shorthand for `quote`, `()` for nil.

### 3.2 Evaluator

- Lisp-1 namespace.
- Lexical scope via a chained env list.
- Special forms: `quote if and or cond when unless let let* setq lambda progn while defun dolist`.
- No macros. No `defmacro`. `defun` is sugar for `(setq f (lambda …))`.
- Tail calls not optimized (acceptable for v1).

### 3.3 Built-ins (always-bound)

```
;; arithmetic
+ - * / = < >        ;; mod, <=, >=, 1+, 1- — Phase 2

;; predicates
null? eq? not        ;; consp/atom?/equal?/stringp/numberp/symbolp — Phase 2

;; cons / list
cons car cdr list length      ;; nth/append/reverse — Phase 2

;; strings
string-append                  ;; string=?/substring/concat — Phase 2

;; I/O
princ                          ;; print — Phase 2

;; function call
funcall                        ;; (funcall f args...) — applies without re-eval

;; buffer ops (session-handle based; used by editor-eval cell intent)
buffer-create     ;; (buffer-create) → buffer-handle
buffer-insert     ;; (buffer-insert buf str) → t
buffer-delete     ;; (buffer-delete buf n) → t
buffer-point      ;; (buffer-point buf) → integer
buffer-goto       ;; (buffer-goto buf pos) → t
buffer-text       ;; (buffer-text buf) → string
buffer-search     ;; (buffer-search buf needle) → integer | nil
buffer-replace    ;; (buffer-replace buf needle replacement) → count

;; Interactive editor primitives (operate on the *active* editor buffer —
;; what the user sees in the anunixmacs window).  These are the primitives
;; users call from ~/.anunixmacs.el and from M-: at the minibuffer.
point                          ;; → integer
point-min point-max
goto-char                      ;; (goto-char N) → N
insert                         ;; (insert "text" …) — accepts strings or ints
delete-char                    ;; (delete-char N) — N>0 forward, N<0 backward
forward-char backward-char     ;; (forward-char [N])
beginning-of-line end-of-line
beginning-of-buffer end-of-buffer
save-buffer                    ;; → t on success, nil otherwise
find-file                      ;; (find-file "/path") — opens via posix ns
current-buffer-name            ;; → "ns:path"

;; Hooks
add-hook                       ;; (add-hook 'find-file-hook 'fn)
run-hooks                      ;; (run-hooks 'find-file-hook)
;; Bound by anx_ed_open_path / anx_ed_save:
;;   find-file-hook   — fired after a buffer is loaded
;;   after-save-hook  — fired after a successful save
```

Built-ins still on the Phase 2 list are flagged in the comments above; user
configs that need them today have to define them in eLISP themselves.

Buffer handles are tagged integers (small, opaque) referring to a per-cell buffer table.

### 3.4 GC

Reference counted on cons cells and strings. Cycles are not supported in v1 (the subset cannot construct them since there is no `set-car!` / `set-cdr!`). The interpreter resets all allocations at cell-completion via an arena.

---

## 4. Buffer Engine

A simple gap buffer over a heap-allocated byte array:

```c
struct anx_ed_buffer {
    char       *data;
    uint32_t    size;        /* total allocated */
    uint32_t    gap_start;
    uint32_t    gap_end;
    uint32_t    point;       /* logical cursor (0-indexed) */
    anx_oid_t   source_oid;  /* origin object, or zero */
    bool        dirty;
};
```

- `insert(buf, s)` — moves gap to point, copies `s` into gap, advances gap_start and point.
- `delete(buf, n)` — moves gap, retreats `gap_end` by n.
- `text(buf)` — flattens to a fresh allocation, returns a NUL-terminated copy.
- Initial size 4096; grows by doubling.

---

## 5. Workflow Template

```text
anx:app/anunixmacs

  TRIGGER ──────► STATE_REF (read source) ─┐
                                           ▼
                                       CELL_CALL "editor-open"
                                           │
                                           ▼
                                       CELL_CALL "editor-eval"
                                           │
                                           ▼
                                       CELL_CALL "editor-save"
                                           │
                                           ▼
                                         OUTPUT
```

Tags: `edit, editor, anunixmacs, elisp, transform, buffer`.

---

## 6. Test Plan (host-native, must pass `make test`)

1. `test_anunixmacs_lisp` — reader/eval roundtrip for primitive forms.
2. `test_anunixmacs_buffer` — insert/delete/search invariants on the gap buffer.
3. `test_anunixmacs_cell` — `editor-open` → `editor-eval` → `editor-save` round trip on a temporary OID.

---

## 7. Implementation Plan

The work is split into three phases.  Phase 1 is the editor shell — open
and edit files interactively, save them as State Objects, allow user
customization in eLISP.  Phase 2 builds out the display infrastructure
that Org needs.  Phase 3 implements Org mode itself in C.

### Phase 1 — Editor shell + extensible eLISP (done)

| # | Deliverable                                                     | Status |
|---|-----------------------------------------------------------------|--------|
| 1.1 | eLISP reader + evaluator + arithmetic & cons primitives       | done   |
| 1.2 | Gap-buffer engine + `buffer-*` primitives                     | done   |
| 1.3 | Editor cell + `workflow_exec` dispatch hook (`editor-eval`)   | done   |
| 1.4 | Interactive editor window taking over the terminal surface    | done   |
| 1.5 | Modeline (path · dirty flag · L/C) and minibuffer line        | done   |
| 1.6 | Keymap with prefix-key support (`C-x` chords); motion + edit  | done   |
| 1.7 | `M-:` minibuffer eLISP eval                                   | done   |
| 1.8 | File open by `[ns:]<path>` resolved through namespace layer   | done   |
| 1.9 | `C-x C-s` save via `anx_so_replace_payload` — POSIX-visible   | done   |
| 1.10 | Shell command `anx [ns:]<path>` (default ns = "posix")       | done   |
| 1.11 | eLISP customization surface: `defun`, `cond`, `when`,        | done   |
|      | `unless`, `dolist`, `not`, `funcall`                         |        |
| 1.12 | Editing primitives bound to active buffer (`point`,          | done   |
|      | `goto-char`, `insert`, `delete-char`, `forward-char`,        |        |
|      | `backward-char`, `save-buffer`, `find-file`, …)              |        |
| 1.13 | Hooks: `add-hook`, `run-hooks`; `find-file-hook` and         | done   |
|      | `after-save-hook` fired by the editor                        |        |
| 1.14 | `~/.anunixmacs.el` loaded at editor start (via posix ns)     | done   |

Demo path:

```
$ anx /tmp/notes.txt          # shell command launches anunixmacs
... edit text ...
M-: (insert "hello")          # any eLISP form evaluates against buffer
C-x C-s                       # save → new SO version, visible to POSIX
C-x C-c                       # close editor
$ cat /tmp/notes.txt          # POSIX-side read sees the new bytes
```

### Phase 2 — Display infrastructure (Org-mode prerequisites)

Org mode leans on text properties, overlays, markers, and regex.  Phase 2
adds these to the buffer engine and exposes them to eLISP.  This is also
where `defmacro` and `condition-case` go — Org's source uses both.

| # | Deliverable                                                       |
|---|-------------------------------------------------------------------|
| 2.1 | Text properties: `(put-text-property START END KEY VAL)`,       |
|     | `(get-text-property POS KEY)` — interval-tree storage           |
| 2.2 | Faces: `(defface NAME ATTRS)` and `face` text property;         |
|     | renderer respects `:foreground`, `:background`, `:bold`,        |
|     | `:italic`, `:underline`                                         |
| 2.3 | Markers (positions that survive insert/delete): `(make-marker)`,|
|     | `(set-marker M POS)`, `(marker-position M)`                     |
| 2.4 | Regex engine + `re-search-forward` / `re-search-backward`       |
| 2.5 | `defmacro` and macroexpansion at eval time                      |
| 2.6 | `condition-case` / `unwind-protect`                             |
| 2.7 | `define-key` from eLISP; expose the global keymap as a table    |
| 2.8 | Major-mode plumbing: `define-derived-mode`, mode hooks,         |
|     | mode-local variables                                            |
| 2.9 | UTF-8 insert path and rendering through `anx_font_draw_codepoint` |
| 2.10 | `define-key` over Org keymap so users can rebind `C-c C-t` etc. |

### Phase 3 — Org mode in C

Native implementation, exposed to eLISP as `org-*` symbols so user
configs do `(setq org-todo-keywords '(…))`, `(add-hook 'org-mode-hook
…)`, `(define-key org-mode-map …)` exactly the way GNU Emacs configs do.

Scope split inside Phase 3:

| # | Deliverable                                                       |
|---|-------------------------------------------------------------------|
| 3.1 | Outline parser: headlines (`*`, `**`, …), section ranges,       |
|     | invariants under edit                                           |
| 3.2 | Folding: per-headline visibility state; `org-cycle` (TAB),      |
|     | `org-shifttab` (S-TAB)                                          |
| 3.3 | TODO state machine: `org-todo` cycles `TODO → DONE`,            |
|     | configurable sequences                                          |
| 3.4 | Markup fontification: `*bold*`, `/italic/`, `_underline_`,      |
|     | `=verbatim=`, `~code~`                                          |
| 3.5 | Lists: `-`/`+`/`1.`, indentation, `org-cycle` on item           |
| 3.6 | Tables: `|`-delimited rows, TAB-driven auto-format, column      |
|     | width tracking                                                  |
| 3.7 | Links: `[[target][label]]`, `org-open-at-point`                 |
| 3.8 | Timestamps: `<YYYY-MM-DD>`, `[YYYY-MM-DD]`, parsing + display   |
| 3.9 | Tags: `:foo:bar:` on headlines, search by tag                   |
| 3.10 | Property drawers: `:PROPERTIES: … :END:`                       |
| 3.11 | Capture (basic): `org-capture` with simple templates           |

Out of scope for the first Org cut (revisit as their own RFCs):

- Agenda views across multiple files (RFC-XXXX).
- Babel source-block evaluation (RFC-XXXX).
- Export backends — HTML, LaTeX, Markdown (RFC-XXXX).
- Calendar / diary integration.

---

## 8. References

- RFC-0010 §3 (libanx adaptation patterns)
- RFC-0012 §6 (Surface Object model)
- RFC-0018 §4 (CELL_CALL node dispatch)
