# RFC-0010: Userland Utility Layer — POSIX Port and Anunix Adaptation

| Field      | Value                                          |
|------------|------------------------------------------------|
| RFC        | 0010                                           |
| Title      | Userland Utility Layer — POSIX Port and Anunix Adaptation |
| Author     | Adam Pippert                                   |
| Status     | Draft                                          |
| Created    | 2026-04-16                                     |
| Updated    | 2026-04-16                                     |
| Depends On | RFC-0001, RFC-0002, RFC-0003, RFC-0004, RFC-0005, RFC-0006, RFC-0007, RFC-0008 |

---

## Executive Summary

This RFC specifies the **Userland Utility Layer**: the minimum set of UNIX utilities required to make Anunix a fully functional, realistic operating system, and the adaptation strategy that makes each utility a native citizen of the Anunix kernel rather than a thin wrapper over a POSIX shim.

Every utility described here must be modified or re-engineered to:

1. Understand and manipulate **State Objects** instead of files
2. Spawn and observe **Execution Cells** instead of processes
3. Delegate appropriately between the POSIX compatibility shim and Anunix-native constructs
4. Produce provenance-linked outputs wherever the native APIs allow

The utilities are organized into three tiers based on dependency and urgency. A shared adapter library (`libanx`) encodes five recurring adaptation patterns to avoid N independent reimplementations of the same kernel-interface boilerplate.

---

## 1. Status

**Status:** Draft  
**Author:** Adam Pippert  
**Depends on:** RFC-0001 through RFC-0008  
**Blocks:** RFC-0011 (agent-native utilities depend on several Tier 1 utilities being functional)

---

## 2. Problem Statement

Anunix replaces UNIX's core abstractions:

| POSIX Concept | Anunix Replacement | Kernel Location |
|---|---|---|
| File | State Object (`ANX_OBJ_*`) | `kernel/core/state/` |
| Process | Execution Cell (`anx_cell`) | `kernel/core/exec/` |
| IPC / Pipe | Routing Plane stream binding | `kernel/core/route/` |
| Permission bits | Capability Objects | `kernel/core/cap/` |
| Address space | Memory Control Plane | `kernel/core/mem/` |

A utility that calls `open(2)`, `fork(2)`, and `pipe(2)` directly will work through the POSIX shim, but it will be blind to object types, memory tiers, cell decomposition, and provenance. The goal of this RFC is to define exactly where each utility should reach through the shim and where it should call native APIs directly, so that Anunix utilities are meaningfully richer than their POSIX ancestors rather than mere wrappers.

---

## 3. Utility Tiers

### Tier 1 — Survival

Required before any serious use is possible. System is not navigable without these.

| Utility | Role | Primary API Path |
|---------|------|------------------|
| `sh` | Interactive shell and script runner | Hybrid: POSIX shim I/O, native Cell API for command execution |
| `echo` | Output text | POSIX shim only |
| `true` / `false` | Boolean exit codes | No API |
| `cat` | Read object payload to stdout | POSIX shim |
| `ls` | List namespace entries with Anunix metadata | Native namespace + object APIs |
| `cp` | Copy State Objects (preserving provenance) | Native State Object API |
| `mv` | Rebind namespace path | Native namespace API |
| `rm` | Unbind; optionally delete object | Native namespace + lifecycle API |
| `mkdir` | Create namespace subtree node | Native namespace API |
| `pwd` / `cd` | Navigate namespace (shell builtins) | Native namespace API |
| `env` | Print/set cell runtime bindings | Native Cell API |

### Tier 2 — Usable

System is navigable; now you can inspect what the OS is doing.

| Utility | Role | Primary API Path |
|---------|------|------------------|
| `ps` / `cell` | List and inspect Execution Cells | Native Cell API exclusively |
| `kill` | Cancel or fail a cell | Native Cell API |
| `stat` | Full State Object metadata | Native object + memplane APIs |
| `find` | Search namespace by pattern or object type | Native namespace API |
| `grep` | Search object payload content | POSIX shim + optional native type awareness |
| `head` / `tail` | First/last N bytes of payload | POSIX shim |
| `wc` | Count bytes/lines | POSIX shim |
| `sort` / `uniq` | Line sorting and dedup | POSIX shim |
| `date` | System time | Arch time API |
| `uname` | Kernel version / arch | Kernel constants |
| `df` | Object store and tier capacity | Native memplane API |
| `dmesg` / `klog` | Kernel trace objects | Native namespace + object API |
| `shutdown` / `reboot` | Halt / reset | Arch API |
| `mount` / `umount` | Attach/detach namespace backends | Native namespace API |

### Tier 3 — Productive

System is working; now you can build, debug, network, and script.

| Utility | Role | Primary API Path |
|---------|------|------------------|
| `vi` / `ed` | Edit object payload in-place | Native `anx_so_write_payload` |
| `make` | Build system | Hybrid: CLW adapter for child cells |
| `diff` | Compare object payloads | POSIX shim |
| `awk` / `sed` | Stream text transformation | POSIX shim |
| `tar` | Archive multiple objects | Native object API |
| `gzip` | Compress object payload | Native payload mutation |
| `ping` | ICMP via Network Plane | Native `anx_netplane_*` |
| `anx-fetch` | HTTP(S) via Network Plane | Native `anx_netplane_*` + credential store |
| `install` | Copy + capability-checked bind | Native object + capability APIs |
| `objdump` / `nm` | Inspect binary object payloads | POSIX shim |

---

## 4. POSIX Syscall to Anunix API Mapping

| POSIX Syscall | Anunix Native Equivalent | Header |
|---|---|---|
| `open(path, flags)` | `anx_ns_resolve()` + `anx_so_open()` | `namespace.h`, `state_object.h` |
| `read(fd, buf, n)` | `anx_so_read_payload()` | `state_object.h` |
| `write(fd, buf, n)` | `anx_so_write_payload()` | `state_object.h` |
| `stat(path, buf)` | `anx_ns_resolve()` + `anx_objstore_lookup()` + `anx_memplane_lookup()` | `namespace.h`, `state_object.h`, `memplane.h` |
| `unlink(path)` | `anx_ns_unbind()` + conditional `anx_so_delete()` | `namespace.h`, `state_object.h` |
| `rename(old, new)` | `anx_ns_unbind()` + `anx_ns_bind()` | `namespace.h` |
| `mkdir(path)` | `anx_ns_create()` | `namespace.h` |
| `fork()` | `anx_cell_derive_child()` | `cell.h` |
| `execve(path, argv)` | Cell plan with binary input OID → `anx_cell_run()` | `cell.h` |
| `waitpid(pid)` | Poll cell store until `COMPLETED` or `FAILED` | `cell.h` |
| `exit(n)` | `anx_cell_transition(ANX_CELL_COMPLETED)` with `error_code = n` | `cell.h` |
| `pipe(fds[2])` | Stream binding between cells via routing plane | `route.h` |
| `getpid()` | `anx_cell_store_lookup(current_cid)` | `cell.h` |
| `getenv(name)` | Cell runtime bindings (`anx_cell.execution_policy`) | `cell.h` |
| `socket/connect/send/recv` | `anx_netplane_*()` | `netplane.h` |
| `mmap()` | `anx_memplane_admit()` + tier placement | `memplane.h` |
| `kill(pid, sig)` | `anx_cell_cancel()` (graceful) or force `ANX_CELL_FAILED` | `cell.h` |

The POSIX shim (`kernel/core/posix/`) translates these automatically for unmodified utilities. Ported utilities call native APIs directly and only fall through to the shim for byte-stream I/O convenience.

---

## 5. Shared Adapter Library: `libanx`

Five adaptation patterns recur across nearly every utility. These are encoded once in `userland/lib/libanx/` and linked by all utilities.

### Pattern 1: Path-to-Object Resolution (POR) — `libanx/path.c`

Resolve a path string to an object handle. Handles multi-namespace syntax (`ns:path/to/entry`), relative paths against the current cell's working namespace, and the `ANX_ENOENT` error path uniformly.

```c
int anx_path_open(const char *path, enum anx_open_mode mode,
                  struct anx_object_handle *out);
```

Used by: `ls`, `cp`, `mv`, `rm`, `stat`, `cat`, `find`, `grep`, `vi`, `install`.

### Pattern 2: Cell Lifecycle Wrap (CLW) — `libanx/exec.c`

Derive a child cell, bind its input OIDs, run it, wait for completion, return `error_code`. This is the `fork + execve + waitpid` equivalent for Anunix. Also wires stdout/stderr stream bindings to the parent cell's output FDs.

```c
int anx_exec_child(struct anx_cell *parent, const char *binary_path,
                   char *const argv[], int *exit_code_out);
```

Used by: `sh` (every command), `make`, `install`.

### Pattern 3: Capability-Checked Open (CCO) — `libanx/access.c`

Check an object's access policy against the calling cell's capability mask before any destructive operation. Returns `ANX_OK` or `ANX_EPERM`.

```c
int anx_access_check(const anx_oid_t *oid, uint32_t required_caps,
                     const anx_cid_t *caller_cid);
```

Used by: `rm --purge`, `install`, `cp` (cross-namespace), `mv` (cross-namespace).

### Pattern 4: Streaming Output as Trace Object (SOT) — `libanx/stream.c`

When `ANX_COMMIT_PIPELINE` environment binding is set, materialize pipeline output as a `ANX_OBJ_BYTE_DATA` State Object with `parent_oids` pointing to the input. Otherwise behave as a normal write to stdout.

```c
int anx_stream_write(int fd, const void *buf, size_t len,
                     const anx_oid_t *source_oid, bool commit);
```

Used by: `grep`, `cat`, `head`, `tail`, `awk`, `sed`, `sort`.

### Pattern 5: Metadata-Aware Format Output (MFO) — `libanx/fmt.c`

Human-readable formatting for all Anunix-native field types. Avoids N independent OID abbreviation and enum-to-string implementations.

```c
void anx_fmt_oid(char *buf, size_t len, const anx_oid_t *oid);
void anx_fmt_tier(char *buf, size_t len, uint8_t tier_mask);
const char *anx_fmt_obj_type(enum anx_object_type t);
const char *anx_fmt_cell_status(enum anx_cell_status s);
const char *anx_fmt_engine_class(enum anx_engine_class c);
const char *anx_fmt_mem_validation(enum anx_mem_validation_state v);
```

Used by: `ls`, `stat`, `ps`/`cell`, `df`, `find`.

---

## 6. New CLI Semantics

Ported utilities are extended with Anunix-specific output and flags.

### `ls` — Extended Columns

```
posix:/bin:
  sh          cap    L2  valid   48K
  cat         byte   L2  valid    8K
  kernel.log  trace  L1  prov   128K
```

Columns: name, object type (`byte`, `struct`, `embed`, `graph`, `output`, `trace`, `cap`, `cred`), memory tier (`L0`–`L5`), validation state (`unval`, `prov`, `valid`, `contest`), payload size.

Flag `--posix` reverts to traditional output for script compatibility.

### `stat` — Full Object Metadata

Shows: OID (abbreviated), object type, schema URI, payload size bytes, version counter, content hash (SHA-256), access policy, lifecycle state, memory tier, decay score, provenance chain length. Flag `--prov` prints the full provenance event log. Flag `--mem` prints tier placement history.

### `ps` / `cell` — Cell Columns

```
CELLID    TYPE       STATUS     QUEUE   INTENT                PARENT   CHILDREN
a3f2...   task_exec  running    q1      hardware-discovery    b8c1...  3
b8c1...   task_exec  completed  —       boot-init             —        12
```

Flag `--tree` renders the parent/child decomposition tree. Status values are direct `anx_cell_status` enum names.

### `rm` — Object Lifecycle Respect

`rm path` unbinds the namespace entry only. `rm --purge path` deletes the object if refcount is zero. Cannot delete `ANX_OBJ_SEALED` objects without `--force`. `rm --purge --force path` is the only path to deleting a sealed object; requires `ANX_CAP_SYSTEM_ADMIN` in the calling cell.

### `cp` — Provenance Preservation

The new object's `parent_oids[0]` is set to the source OID. Provenance event `ANX_PROV_DERIVED_FROM` is recorded automatically. The copy is not sealed by default — use `cp --seal` to immediately seal the destination object.

---

## 7. Source Layout

```
userland/
  bin/                    # Tier 1 + 2 utilities
    sh/
      sh.c
      Makefile
    ls/
      ls.c
      Makefile
    cat/
      cat.c
      Makefile
    cp/
      cp.c
      Makefile
    mv/
      mv.c
      Makefile
    rm/
      rm.c
      Makefile
    mkdir/
      mkdir.c
      Makefile
    stat/
      stat.c
      Makefile
    ps/
      ps.c
      Makefile
    kill/
      kill.c
      Makefile
    find/
      find.c
      Makefile
    grep/
      grep.c
      Makefile
    echo/
      echo.c
      Makefile
    env/
      env.c
      Makefile
    df/
      df.c
      Makefile
    dmesg/
      dmesg.c
      Makefile
    head/
      head.c
      Makefile
    tail/
      tail.c
      Makefile
    wc/
      wc.c
      Makefile
    sort/
      sort.c
      Makefile
    date/
      date.c
      Makefile
    uname/
      uname.c
      Makefile
  sbin/                   # System utilities
    mount/
      mount.c
      Makefile
    shutdown/
      shutdown.c
      Makefile
  lib/
    libanx/
      path.c
      exec.c
      access.c
      stream.c
      fmt.c
      libanx.h
      Makefile
  Makefile                # builds libanx, then bin, then sbin
```

Integration with the top-level build:

```make
# Top-level Makefile addition:
userland: kernel
	$(MAKE) -C userland ARCH=$(ARCH) CC=$(CC) CFLAGS="$(CFLAGS)"
```

---

## 8. Testing Strategy

Test files follow the existing `tests/test_<subsystem>_<thing>.c` convention:

```
tests/test_util_ls.c
tests/test_util_stat.c
tests/test_util_ps.c
tests/test_util_rm.c
tests/test_util_cp.c
tests/test_libanx_path.c
tests/test_libanx_exec.c
tests/test_libanx_fmt.c
```

Tests use the existing `tests/harness/` mock infrastructure. No network I/O, no real block device — all kernel APIs are called against mock state objects and a mock namespace. Behavior under error conditions (sealed objects, capability mismatch, cell timeout) is explicitly tested.

---

## 9. Implementation Sequence

### Phase 0: libanx (prerequisite for all utilities)

`fmt.c` → `path.c` → `access.c` → `stream.c` → `exec.c`

Start with `fmt.c` because it has no kernel dependencies beyond type definitions and is immediately testable on the host.

### Phase 1: Tier 1 Utilities

`echo` → `true`/`false` → `cat` → `ls` → `stat` → `mkdir`/`rm`/`mv`/`cp` → `env` → **`sh`**

`sh` is the last Tier 1 utility because it depends on the CLW adapter and all other Tier 1 utilities being testable.

Pipe support in `sh` is deferred to Phase 2.

### Phase 2: Tier 2 Utilities

`ps`/`cell`, `kill` (with Phase 1 cell runtime)  
`find`, `grep`, `head`, `tail`, `wc`, `sort`, `uniq` (POSIX shim path, parallel)  
`date`, `uname` (trivial)  
`df`, `dmesg` (memplane + namespace, after Phase 1 objects are populated)  
`shutdown`, `reboot` (arch API — implement last, risky on real hardware)

Pipe support in `sh` (stream bindings via routing plane) added here.

### Phase 3: Tier 3 Utilities

`vi`/`ed` → `diff`/`awk`/`sed` → `tar`/`gzip` → `ping`/`anx-fetch` → `install` → `make`

---

## 10. Open Questions

1. **Shell name**: call it `sh` (POSIX-compatible name, used when running as `/bin/sh`) or `ansh` (branded, distinguishes from kernel monitor `kernel/core/shell.c`)? Recommend: binary named `ansh`, symlinked as `/bin/sh` for POSIX compatibility.

2. **Pipe materialization**: should pipeline intermediates always produce trace State Objects, or only when `ANX_COMMIT_PIPELINE` is set? Default-off is less surprising; default-on is more valuable for provenance. Recommend default-off with per-session opt-in.

3. **`rm` default behavior**: unbind-only vs. delete-if-refcount-zero. Recommend unbind-only to be conservative with object lifetime.
