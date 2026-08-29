# Anunix 2026.8.28-1 Release Notes

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
- **A real Hologram matmul kernel runs.** `hologram-compute::HologramF32MatmulSquare<4>`
  multiplies a real `4 × 4` matrix and returns the correct product.
- **The Hologram compiler and runtime executor run, with a correct
  numeric result.** `hologram_compiler::compile_from_source()` compiles
  a real ReLU graph; `hologram_exec::InferenceSession` runs it against
  16 `f32` inputs and returns the mathematically correct output.
- **A second real Anunix bug fixed.** The ring-3 stack Anunix handed to
  a binary was 16-byte aligned; the SysV ABI needs 8-byte alignment at
  entry. Any binary using an aligned SSE instruction raised a `#GP`
  until this release's fix.

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

## Verified: real Hologram tensor compute

`hologram-compute::prism_axes::HologramF32MatmulSquare<4>`, reached
through the `prism_tensor::tensor::TensorAxis` trait every external
caller uses, multiplies a `4 × 4` identity matrix by a second matrix
`B`. The kernel is a hand-written for-loop, not the LUT-dispatch path;
see Forward-looking. The identity product equals `B`, so a correct
kernel returns `B` unchanged. The shell command `exec /bin/holomm`,
run against a QEMU boot of `anunix-qemu.elf` on 2026-08-28, printed:

```
hologram-compute::HologramF32MatmulSquare<4>::matmul on Anunix:
0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
exec: exit_status=0
```

Exit status `0` confirms all 16 output elements matched `B` within a
`0.0001` tolerance, checked in the test binary before it called
`ANX_SYSCALL_EXIT`. `hologram-compute` needs `alloc`; the test binary
supplies a 4096-byte bump allocator as `#[global_allocator]`, since
Anunix has no heap allocator syscall for a ring-3 binary to call.

## Verified: the Hologram compiler and runtime executor

`hologram_compiler::compile_from_source()` and `hologram_exec::InferenceSession`
compile a native-DSL Hologram program and run it, the same round trip
`hologram-exec`'s own `session.rs` test exercises upstream. The source
declares a real shape (Hologram's native DSL takes a `:16` token for a
16-element tensor) and a ReLU op over 16 `f32` inputs running from `-8`
to `7`:

```
input x :16
op relu x :16 as=y
output y
```

These crates pull in `blake3` and `hashbrown`, and both emit SSE
instructions. The stock `x86_64-unknown-none` softfloat target cannot
codegen those instructions correctly, so this demo needed a custom
hardfloat Rust target (nightly `rustc` plus `-Z build-std`) and exposed
a second real Anunix bug — see "Ring-3 stack alignment" below. The
shell command `exec /bin/holo5`, run against a QEMU boot of
`anunix-qemu.elf` on 2026-08-28, printed:

```
hologram-compiler: compile+load+execute a real graph on Anunix
compiled: archive_bytes=572
loaded: kernels=1 inputs=1 outputs=1
executed: output_buffers=1
output_bytes=64
0 0 0 0 0 0 0 0 0 1 2 3 4 5 6 7
exec: exit_status=0
```

The compiler parsed the source and built a 572-byte archive. The
executor loaded it and ran the compiled ReLU kernel against the 16
input values. `relu(x) = max(x, 0)` zeroes the nine inputs `-8` through
`0` and passes `1` through `7` unchanged — exactly what the printed
output shows. The test binary checked every element against that
expected value before calling `ANX_SYSCALL_EXIT`; exit status `0`
confirms all 16 matched.

## Ring-3 stack alignment (second Anunix bug found)

The SysV ABI requires `RSP % 16 == 0` immediately before a `call`. A
normally-called function sees `RSP % 16 == 8` at its own entry, since
the `call` instruction pushes an 8-byte return address. `_start` is
reached through `iretq`, not `call`, but compiler-generated code still
assumes that entry convention for aligned SSE spills (`movaps`).

`anx_posix_loader_load_and_run()` passed a 16-byte-aligned stack top to
`arch_enter_usermode()`, not an 8-byte-aligned one. The first `movaps`
in any binary that used SSE raised a `#GP`; the error code `0` is the
alignment-fault signature. Earlier test binaries in this release never
hit this, since they targeted the stock softfloat `x86_64-unknown-none`,
which never emits SSE. This release fixes it in
`kernel/core/posix/posix.c` by subtracting `8`, not `16`, from
`ANX_USER_STACK_TOP`.

## Scope of the Hologram verification

Three crates verified: `hologram-types` (dtype and shape vocabulary),
`hologram-compute` (one CPU matmul kernel), and the compile-and-execute
round trip through `hologram-compiler` and `hologram-exec`.

The `hologram` CLI is out of scope for a different reason than the
LUT-dispatch path above. It is not merely `std`-gated; it is a
declared host-only binary. `hologram-cli/Cargo.toml` states this
directly:

```
# The CLI is a host binary: it opts the otherwise-`no_std` library crates
# back into `std` and selects the CPU backend.
```

Its dependencies on `wasmtime` (`hologram-runtime`, feature
`engine-wasmtime`), `tokio`, and native storage and networking
(`hologram-store`, `hologram-net`) are unconditional, not
feature-gated. Hologram's own maintainers drew this `no_std`/`std` boundary. Running
the CLI on Anunix needs a full `std` port, not a smaller step past
this release's work. See "Closed: the LUT-dispatch path" above and
Forward-looking below.

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

## Closed: the LUT-dispatch path is not reachable under no_std

`hologram_compute::cpu::lut` is Hologram's compute-once mechanism: it
memoizes a 16-bit activation as a `[u16; 65536]` lookup table instead
of computing it per element. `hologram-compute`'s own source gates it
behind `#[cfg(feature = "std")]`, with `mathf` as the `not(feature =
"std")` alternative:

```rust
/// LUT-accelerated low-precision activations (PM_7 Q0/Q1). Needs `OnceLock`
/// (std) for the process-lifetime table cache; under no_std the activations
/// are computed directly (a compile-time choice, not a runtime fallback).
#[cfg(feature = "std")]
pub mod lut;
#[cfg(not(feature = "std"))]
pub mod mathf;
```

`std::sync::OnceLock` needs OS-backed atomics and thread parking, which
a freestanding `no_std` target does not provide. Hologram's authors
made this choice deliberately; it is not a gap in this exec path.
Reaching the LUT path on Anunix would need a real `std` port: threads,
thread-local storage, and atomic synchronization primitives. This
release's segment-loading and syscall work does not reach that far.

## Forward-looking

- **Per-process page tables and per-page protection**, to close the
  coarse-grained window described above.
- **A disk- or virtio-backed binary transfer path.** This release's
  510 KB compiler-and-executor binary took 3397 `appendb64` requests
  and about five minutes to load. That does not scale past a few
  hundred kilobytes.
