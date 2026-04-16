# Anunix OS Tools Implementation Plan

## Shell: `ansh` (Anunix Shell)

The current kernel monitor (`shell.c`) is renamed to **ansh** — the Anunix Shell. It evolves from a monolithic 1670-line dispatch table into a modular shell with pipes, variables, and scripting, backed by tools factored into `kernel/core/tools/*.c`.

## Architecture Decisions

1. **All Phase 1-2 tools are kernel builtins** — the kernel runs in a single address space without userspace process isolation. Tools are implemented as `cmd_<tool>(int argc, char **argv)` functions in `kernel/core/tools/<tool>.c`. The shell dispatch table references these.

2. **Shell refactoring** — `shell.c` is split:
   - `kernel/core/ansh.c` — REPL loop, line editor, history, dispatch table
   - `kernel/core/tools/*.c` — one file per tool
   - `kernel/include/anx/tools.h` — declares all tool entry points

3. **Pipe operator creates transient State Objects** — UNIX pipes are byte streams; Anunix pipes are State Objects. Each pipeline stage's output is captured into a transient State Object. The next stage receives it as input via `$_`. Pipeline results are inspectable, versioned, and have provenance.

4. **Agent awareness via engine registration** — every tool can register as a deterministic engine via `anx_engine_register()`. The routing plane can then dispatch cells to tools. The adapter between `cmd_<tool>(argc, argv)` and `cell handler(cell *)` is the Phase 4 deliverable.

## Kernel Prerequisites

Before building tools, these kernel APIs must be added:

| API | File | Needed By | Description |
|-----|------|-----------|-------------|
| `anx_ns_list()` | `namespace.c` | `ls` | Enumerate entries in a namespace path |
| `anx_ns_unbind()` | `namespace.c` | `mv`, `rm` | Remove a namespace binding (currently stub) |
| `anx_ns_rename()` | `namespace.c` | `mv` | Rename a namespace binding |
| `anx_objstore_iterate()` | `objstore.c` | `search` | Walk all objects with a callback |
| `anx_so_write_string()` | `state_object.h` | `write` | Convenience for text object creation |

## Phase 1: Minimum Usable System

### Core Tools

| Tool | UNIX Equiv | Anunix Difference | File | Complexity |
|------|-----------|-------------------|------|------------|
| `ls` | `ls` | Lists namespace bindings. Shows OID, object type, lifecycle state, version, payload size. `-l` long format, `-a` all namespaces, `-t` sort by version timestamp. | `tools/ls.c` | Low (needs `anx_ns_list`) |
| `cat` | `cat` | Reads State Object payload by OID-prefix or namespace path. `-p` shows provenance chain, `-v` all versions, `-x` hex dump. | `tools/cat.c` | Low |
| `write` | `echo >` | Creates typed State Object with provenance. Sets creator_cell. `-t <type>` sets object type (byte_data, structured, etc.). | `tools/write.c` | Low |
| `cp` | `cp` | Creates new OID, copies payload, records derivation provenance (`parent_oids` links to source). Content-addressed dedup automatic via `content_hash`. | `tools/cp.c` | Low-Med |
| `mv` | `mv` | Rebinds namespace entry — object doesn't move, OID stable. Only the name changes. | `tools/mv.c` | Low |
| `rm` | `rm` | Transitions to DELETED state (or TOMBSTONE with `-f`). Respects retention policies. Checks refcount before destroy. Already partially implemented as `state delete`. | `tools/rm.c` | Low |
| `cells` | `ps` | Shows cell CID, intent name, status (14 lifecycle states), type, attempt count, child count, output count. `-a` includes completed/failed, `-t` tree view with parent/child lineage. | `tools/cells.c` | Low |
| `sysinfo` | `uname`+`free`+`lscpu` | Unified system info: kernel version, arch, page allocator stats, memory tier occupancy, scheduler queue depths, engine count, object count. Consolidates existing `mem stats`, `hw-inventory`, `sched status`. | `tools/sysinfo.c` | Low |

### ansh Enhancements

| Feature | UNIX Equiv | Implementation | Complexity |
|---------|-----------|----------------|------------|
| Pipe `\|>` | `\|` | Detect `\|>` delimiter. Capture kprintf output into transient State Object. Pass OID as `$_` to next stage. Auto-delete transient object after pipeline completes. | Medium |
| `$_` variable | `$!` / `$_` | Last command's result OID (for pipe and manual use) | Low |
| `$?` variable | `$?` | Last command's return code | Low |
| Named variables | `$VAR` | Variables stored as namespace bindings in `ansh:` namespace | Medium |

## Phase 2: Productive System

### Tools

| Tool | UNIX Equiv | Anunix Difference | File | Complexity |
|------|-----------|-------------------|------|------------|
| `search` | `grep` | Searches across State Objects by payload content. Filters by namespace, object type, state. Results returned as State Object listing. Eventually hooks into semantic search (RFC-0009). `-t <type>` filter, `-i` case-insensitive. | `tools/search.c` | Medium (needs `anx_objstore_iterate`) |
| `inspect` | `hexdump`+`file` | Full State Object internals: OID, content hash, version, metadata stores, provenance log, access policy, retention, lifecycle state, refcount. Hex dump with ASCII sidebar. `-m` metadata only, `-x` hex only, `-p` provenance only. | `tools/inspect.c` | Medium |
| `fetch` | `curl`/`wget` | HTTP GET/POST → result stored as State Object. Provenance records source URL. `-c <cred>` injects credential for auth. Result is a first-class object, not raw bytes. Extends existing `http-get` and `api` commands. | `tools/fetch.c` | Low-Med |
| `netinfo` | `ifconfig`/`ip` | IP config from network stack: local IP, netmask, gateway, DNS, MAC. All from `anx_net_config` and virtio-net. | `tools/netinfo.c` | Low |
| `head`/`tail` | `head`/`tail` | Partial payload read using offset-based `anx_so_read_payload`. `-n <lines>` or `-c <bytes>`. | `tools/headtail.c` | Low |
| `wc` | `wc` | Bytes/lines/words in payload. Also reports object metadata: type, version count, provenance depth. | `tools/wc.c` | Low |
| `history` | `history` | Extend existing 32-entry ring buffer: numbered display, `!N` recall, persist to disk store. | `tools/history.c` | Low |

### ansh Enhancements

| Feature | UNIX Equiv | Implementation | Complexity |
|---------|-----------|----------------|------------|
| `if/then/end` | `if/then/fi` | `if $? == 0 then <cmd> end`. Basic conditionals. | Medium |
| `for/do/done` | `for/do/done` | `for OBJ in namespace:path do <cmd> done`. Iterates namespace entries. | Medium |
| Job control | `&`, `bg`, `fg` | `<cmd> &` creates Execution Cell with `ANX_CELL_TASK_EXECUTION`, queues via scheduler. `jobs` lists active cells. `fg <cid>` attaches output. | High |

## Phase 3: Developer Tools

| Tool | UNIX Equiv | Anunix Difference | File | Complexity |
|------|-----------|-------------------|------|------------|
| `celldbg` | `gdb attach`/`strace` | Attach to running cell by CID. Real-time status transitions, input/output OIDs, error messages, routing decisions. Sub-commands: `status`, `inputs`, `outputs`, `trace`, `cancel`, `retry`. Reads from existing `cell_trace.c` infrastructure. | `tools/celldbg.c` | High |
| `perf` | `perf stat`/`time` | Extend existing TSC profiler. `perf <cmd>` wraps command with timing. `perf` alone shows boot profile. Per-subsystem breakdown. | `tools/perf.c` | Low-Med |
| `route` | (none) | Routing plane inspector. `route plan <cid>` shows candidates and scores. `route engines` lists engines with status. `route feedback` shows outcome history. Exercises `anx_route_plan()` and `anx_route_score_engine()`. | `tools/route.c` | Medium |
| `meta` | `xattr`/`getfattr` | Read/write system and user metadata on State Objects. `meta show <oid>`, `meta set <oid> key value`, `meta get <oid> key`. | `tools/meta.c` | Low-Med |

## Phase 4: Agent-Aware Tools

### Engine Registration Framework

Every tool from Phases 1-3 registers as a deterministic engine:

```c
struct anx_tool_entry {
    const char *name;           /* "tool.ls", "tool.cat", etc. */
    anx_eid_t eid;
    void (*handler)(struct anx_cell *cell);
    uint32_t capabilities;
};
```

Registration during init:
```c
anx_engine_register("tool.ls", ANX_ENGINE_DETERMINISTIC_TOOL,
                     ANX_CAP_TOOL_EXECUTION, &eng);
```

The routing plane dispatches `ANX_CELL_TASK_EXECUTION` cells to tools. Each tool reads cell inputs (namespace path OID), executes, writes results as output State Objects.

### Agent-Specific Tools

| Tool | Description | Complexity |
|------|-------------|------------|
| `agent` | Submit natural-language intent → routing decomposition → tool execution. Uses model client. `agent "find all structured objects and summarize"` creates cells for search + summarize, routes each to engines. | High |
| `plan` | Shows execution plan decomposition. `plan <cid>` displays child cells, selected engines, constraints. Like SQL `EXPLAIN`. | Medium |

## File Structure

```
kernel/core/
  ansh.c              REPL loop, line editor, history, dispatch
  tools/
    ls.c              List namespace entries
    cat.c             Read object payload
    write.c           Create State Object
    cp.c              Copy with derivation provenance
    mv.c              Rebind namespace entry
    rm.c              Delete object
    cells.c           List/inspect execution cells
    sysinfo.c         Unified system info
    search.c          Payload search across objects
    inspect.c         Object hexdump + full metadata
    fetch.c           HTTP client → State Object
    netinfo.c         Network configuration display
    headtail.c        Partial payload read
    wc.c              Object statistics
    history.c         Extended command history
    celldbg.c         Cell debugger
    perf.c            Performance profiler
    route.c           Routing plane inspector
    meta.c            Metadata editor
  tools.h             All tool entry point declarations
```

## Implementation Order

1. **Kernel prerequisites** — `anx_ns_list`, `anx_ns_unbind`, `anx_objstore_iterate`
2. **Shell refactor** — rename to `ansh`, split into `ansh.c` + `tools/*.c`
3. **Phase 1 tools** — `ls`, `cat`, `write`, `rm`, `cells`, `sysinfo`
4. **Pipe operator** — `|>` with transient State Objects
5. **Phase 1 remaining** — `cp`, `mv`, variables
6. **Phase 2 tools** — `search`, `inspect`, `fetch`, `netinfo`
7. **Shell scripting** — `if/then/end`, `for/do/done`
8. **Phase 2 remaining** — `head/tail`, `wc`, `history`, job control
9. **Phase 3 tools** — `celldbg`, extended `perf`, `route`, `meta`
10. **Phase 4** — engine registration, `agent`, `plan`

## Estimated Effort

| Phase | Tools | Lines (est.) | Key Deliverable |
|-------|-------|-------------|-----------------|
| Prerequisites | 3 APIs | 200 | Namespace list, object iteration |
| Shell refactor | — | 300 (net) | ansh.c + tools.h + dispatch split |
| Phase 1 | 8 tools + pipes | 1,200 | Usable object-oriented shell |
| Phase 2 | 7 tools + scripting | 1,500 | Productive development environment |
| Phase 3 | 4 tools | 800 | Developer debugging/profiling |
| Phase 4 | 2 tools + framework | 600 | Agent-driven tool composition |
| **Total** | **24 tools** | **~4,600** | |
