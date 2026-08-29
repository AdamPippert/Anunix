# Anunix 2026.8.28 Release Notes

Milestone: **Real ELF execution.** The previous `anx_posix_exec_in_proc()`
simulated running a binary and never ran its code. The new exec path
loads real segments, switches the CPU to ring 3, and runs the binary's
actual machine code. A real Rust `no_std` binary and real code from
`Hologram-Technologies/hologram` both execute successfully through this
exec path, verified in QEMU on `hyde`.

## Highlights

- **Real exec, not simulated.** `anx_posix_exec_in_proc()` maps a
  binary's `PT_LOAD` segments, drops to ring 3, and runs the binary's
  actual machine code. The previous implementation validated the ELF
  header and returned a hardcoded exit status without running anything.
- **New syscall trap.** `int 0x80` carries `ANX_SYSCALL_WRITE` and the
  new `ANX_SYSCALL_EXIT` from ring 3 into the kernel.
- **New shell tools.** `exec <path>` runs a binary and prints its exit
  status and captured stdout. `appendb64 <path> <base64-chunk>` loads a
  binary into the `posix` namespace across multiple shell lines, since
  one shell line holds at most 256 bytes.
- **A real Rust binary runs.** A `no_std`, `no_main` binary, built with
  stable `rustc` 1.98.0 for the `x86_64-unknown-none` target, executes
  and calls `ANX_SYSCALL_WRITE` and `ANX_SYSCALL_EXIT` directly.
- **Real Hologram code runs.** `hologram-types::DTypeId::storage_bytes()`,
  fetched from `github.com/Hologram-Technologies/hologram` and built
  `no_std` for Anunix, computes storage sizes for the `F32`, `I4`, and
  `E8CB` dtypes and exits `0` on a correct result.

## What changed

The old `anx_posix_exec_in_proc()` carried a comment stating it
simulated execution. It validated the ELF header and program headers,
then wrote a fixed string to `last_exec_result` and returned exit
status `42`. No instruction from the target binary ran.

The new implementation:

1. Copies each `PT_LOAD` segment to its linked `p_vaddr`, through the
   new arch hook `arch_exec_load_segment()`.
2. Calls `arch_enter_usermode()`, which builds an `iretq` frame and
   switches the CPU to ring 3 at the binary's entry point.
3. Handles `int 0x80` from ring 3 in `isr_stub_syscall`
   (`kernel/arch/x86_64/usermode.S`), which forwards the call to
   `anx_syscall_trap()` in `kernel/core/posix/posix.c`.
4. On `ANX_SYSCALL_EXIT`, calls `arch_return_to_kernel()`, which
   restores the caller's saved registers and makes `arch_enter_usermode()`
   return the exit code as a normal function call would.

`ANX_SYSCALL_WRITE` on file descriptor 1 or 2 no longer requires a prior
`open()` call. It appends to `anx_posix_exec_last_result()` and echoes
each byte to the console through `kputc()`.

## Fixed load window

Anunix has no per-process page tables. Every binary loads into a fixed
physical-equals-virtual window inside the kernel's existing low-1-GiB
identity map:

```c
/* kernel/include/anx/posix.h */
#define ANX_USER_LOAD_MIN    0x02000000ULL  /* 32 MiB: past the kernel image */
#define ANX_USER_LOAD_MAX    0x08000000ULL  /* 128 MiB ceiling */
#define ANX_USER_STACK_SIZE  0x00100000ULL  /* 1 MiB */
```

A binary's `PT_LOAD` segments must link inside `[ANX_USER_LOAD_MIN,
ANX_USER_LOAD_MAX)`. `anx_posix_loader_load_and_run()` rejects a
segment outside this window and returns `ANX_EINVAL`. It copies no
bytes and makes no ring-3 transition when it rejects a segment.

`ANX_USER_LOAD_MIN`, `ANX_USER_LOAD_MAX`, and `ANX_USER_STACK_SIZE` are
compile-time constants. A build cannot change them at runtime.

## Limitation: coarse-grained memory protection

`arch_exception_init()` marks the entire low 1 GiB user-accessible, to
let ring-3 code reach the fixed load window. This mark covers kernel
code and kernel data, not only the load window.

The x86-64 MMU ANDs the U/S bit across every level of a page-table
walk. A supervisor-only bit at any one level denies user access,
regardless of the other levels. `arch_exception_init()` sets the U/S
bit at both the PML4 and the PDPT level for that reason.

This coarse-grained fix is not process isolation. A ring-3 binary can
read and write kernel memory outside its own load window.
`kernel/arch/x86_64/exception.c` carries the same limitation in a
source comment. Per-page protection and per-process address spaces are
`Planned:` work; this release does not implement them.

## New API

```c
/* kernel/include/anx/arch.h */
uint64_t arch_enter_usermode(uint64_t entry, uint64_t user_rsp);
void     arch_return_to_kernel(int code) __attribute__((noreturn));
void     arch_exec_load_segment(uint64_t vaddr, const void *src,
                                 uint64_t filesz, uint64_t memsz);

/* kernel/include/anx/posix.h */
#define ANX_SYSCALL_EXIT  5
long anx_syscall_trap(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);
```

`anx_posix_exec_result.stdout_text` grows from 64 bytes to
`ANX_POSIX_EXEC_STDOUT_MAX` (4096 bytes), to hold real captured output
instead of one fixed 20-byte string. `ANX_POSIX_EXEC_STDOUT_MAX` is a
compile-time constant; a build cannot change it at runtime.

## Syscall ABI v0 (x86_64, `int 0x80`)

| Register | Carries |
| --- | --- |
| `rax` | Syscall number (`ANX_SYSCALL_*`) |
| `rdi` | Argument 0 |
| `rsi` | Argument 1 |
| `rdx` | Argument 2 |
| `rax` (return) | Result, or a negative `ANX_E*` code |

## Verified: a real Rust binary

The binary is built on `hyde` with `rustc` 1.98.0 (installed through
`rustup`, `target x86_64-unknown-none`, `profile minimal`) and linked
at `0x02000000` with a custom `link.ld`. The shell command
`exec /bin/rhello`, run against a QEMU boot of `anunix-qemu.elf` on
2026-08-28, printed:

```
hello from rust
exec: exit_status=9, stdout (16 bytes):
hello from rust
```

The exit status and printed text match the source exactly.

## Verified: Hologram-Technologies/hologram

`hologram-types` (`#![no_std]` unconditionally, per its own source) is
a direct Cargo git dependency on
`https://github.com/Hologram-Technologies/hologram`. The test binary
calls `DTypeId::F32.storage_bytes(1000)`, `DTypeId::I4.storage_bytes(1000)`,
and `DTypeId::E8CB.storage_bytes(1000)`, then exits `0` only if every
result matches the expected value. The shell command `exec /bin/holodemo`,
run against a QEMU boot of `anunix-qemu.elf` on 2026-08-28, printed:

```
hologram-types::DTypeId::storage_bytes on Anunix:
4000 bytes
500 bytes
125 bytes
exec: exit_status=0
```

`4000`, `500`, and `125` are the correct storage-byte counts for 1000
elements. `F32` uses 4 bytes per element. `I4` packs 2 elements per
byte. `E8CB` packs 8 elements per byte. Exit status `0` confirms all
three matched before the binary exited.

This validates only `hologram-types`, the dtype and shape vocabulary
crate. The Hologram CLI, compiler, and tensor-execution crates depend
on `std`, `tokio`, `wasmtime`, and `wgpu`, none of which build for
Anunix's freestanding target. Running them is `Planned:` work; see
Forward-looking.

## Tests

Host-native suite, `make test` on `hyde`:

```
=== Results: 59 passed, 0 failed ===
```

`tests/test_userspace_prereqs.c` moves its fixture ELF's link address
into the new exec window and asserts the mock-arch host result
(`exit_status == -1`, since `tests/harness/mock_arch.c` has no ring 3 to
enter) rather than the old simulated stub's hardcoded exit status `42`.

## Toolchain note

Rust is a **build-time tool only**, for compiling third-party artifacts
that target Anunix's syscall ABI. No Rust source enters the Anunix
kernel or core userland. The language policy in `CLAUDE.md` is
unchanged.

## Breaking changes

None. `ANX_SYSCALL_OPEN`, `ANX_SYSCALL_READ`, `ANX_SYSCALL_WRITE`,
`ANX_SYSCALL_CLOSE`, and `ANX_SYSCALL_TIME` keep their existing numbers.
`anx_posix_exec()` and `anx_posix_exec_in_proc()` keep their existing
signatures.

## Forward-looking

- **Per-process page tables and per-page protection**, to close the
  coarse-grained window described above.
- **A larger `no_std` Hologram surface**: `hologram-ops`,
  `hologram-graph`, and `hologram-compute` (`default-features = false`,
  `features = ["cpu"]`) compile a real tensor graph and dispatch a real
  operation, not only read dtype constants.
- **A disk- or virtio-backed binary transfer path**, to replace
  `appendb64`'s repeated 256-byte shell lines for binaries larger than
  a few kilobytes.
