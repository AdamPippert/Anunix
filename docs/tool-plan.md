# Anunix OS Tools Implementation Plan

## Phase 1: Minimum Usable System

### Kernel Prerequisites
- `anx_ns_list()` — enumerate namespace entries (needed by `ls`)
- `anx_ns_unbind()` implementation (currently stub, needed by `mv`)
- `anx_objstore_iterate()` — walk all objects (needed by `search`)

### Core Tools (shell builtins → kernel/core/tools/*.c)

| Tool | UNIX Equiv | Anunix Difference | Priority |
|------|-----------|-------------------|----------|
| `ls` | `ls` | Lists namespace bindings, shows OID/type/state/version | Essential |
| `cat` | `cat` | Reads State Object payload by OID or path, `-p` for provenance | Essential |
| `write` | `echo >` | Creates typed State Object with provenance | Essential |
| `cp` | `cp` | New OID, copies payload, records derivation provenance | Essential |
| `mv` | `mv` | Rebinds namespace entry (object doesn't move, OID stable) | Essential |
| `rm` | `rm` | Transitions to DELETED/TOMBSTONE, respects retention | Essential |
| `cells` | `ps` | Shows cell CID/intent/status/type, 14 lifecycle states | Essential |
| `sysinfo` | `uname`+`free` | Unified: version, arch, memory, tiers, queues, engines | Essential |

### Shell Enhancements
- **Pipe operator `|>`** — creates transient State Object between stages
- **Variables** — `$_` = last result OID, `$?` = last return code

## Phase 2: Productive System

| Tool | UNIX Equiv | Anunix Difference | Priority |
|------|-----------|-------------------|----------|
| `search` | `grep` | Searches across State Objects, filters by type/state | High |
| `inspect` | `hexdump`+`file` | Full object internals: metadata, provenance, hex dump | High |
| `fetch` | `curl`/`wget` | HTTP GET → State Object with URL provenance + cred injection | High |
| `netinfo` | `ifconfig` | IP/mask/gateway/DNS/MAC from network stack | High |
| `head`/`tail` | `head`/`tail` | Offset-based partial payload read | High |
| `wc` | `wc` | Bytes/lines/words + object metadata stats | Medium |
| `history` | `history` | Extend existing: numbered, `!N` recall, persist to disk | Medium |

### Shell Enhancements
- **Conditionals** — `if $? == 0 then ... end`
- **Loops** — `for OBJ in namespace:path do ... done`
- **Job control** — `&` backgrounds as cell, `jobs`, `fg <cid>`

## Phase 3: Developer Tools

| Tool | UNIX Equiv | Anunix Difference | Priority |
|------|-----------|-------------------|----------|
| `celldbg` | `gdb attach` | Attach to cell, show transitions/IO/routing/traces | Medium |
| `perf` | `perf stat` | Extend existing: wrap commands, per-subsystem profiling | Medium |
| `route` | (none) | Routing plane inspector: scores, engines, decisions | Medium |
| `meta` | `xattr` | Read/write system/user metadata on objects | Medium |

## Phase 4: Agent-Aware Tools

- **Engine registration** — every tool registers via `anx_engine_register()` with capability tags
- **Tool dispatch adapter** — bridges shell commands to cell-based execution
- **`agent` command** — natural-language intent → routing decomposition → tool execution
- **`plan` command** — shows execution plan decomposition for a cell

## Architecture

All Phase 1-2 tools are kernel shell builtins (no userspace yet). Factor into `kernel/core/tools/*.c` with `cmd_<tool>(int argc, char **argv)` signatures. The shell dispatch table in `shell.c` references these. Each tool can later be promoted to a cell-backed engine.

**Pipe operator creates transient State Objects** — this is the key Anunix difference from UNIX byte-stream pipes. Pipeline results are inspectable, versioned, and have provenance.
