# Anunix 2026.8.30 Release Notes

Milestone: **Prism support.** Real code from `UOR-Foundation/prism` —
the standard-library layer `Hologram-Technologies/hologram` builds
on — now runs on Anunix independently of Hologram. Getting a correct
result surfaced a third real Anunix bug: the syscall trap did not
preserve `rcx` and `r8`-`r11` across the call into C, corrupting a
ring-3 program's own register state on every syscall.

## Highlights

- **Real Prism code runs, independently of Hologram.**
  `uor-prism-tensor::CpuI8MatmulSquare<4>` computes a signed `4 × 4`
  integer matrix product and returns the exact expected result.
- **A third real Anunix bug fixed.** `isr_stub_syscall` did not save
  `rcx` or `r8`-`r11` around its call into `anx_syscall_trap`, so a
  ring-3 program's own values in those registers could be silently
  overwritten by every syscall.
- **The syscall ABI doc now states what it always should have.**
  `kernel/include/anx/posix.h` documents that every register except
  `rax`, `rdi`, `rsi`, and `rdx` survives a syscall unchanged.

## The bug: unpreserved registers across the syscall trap

`isr_stub_syscall` (`kernel/arch/x86_64/usermode.S`) shuffles the
incoming `rax`/`rdi`/`rsi`/`rdx` into the C argument registers, then
calls `anx_syscall_trap()`. The SysV ABI lets a called C function
clobber `rcx` and `r8`-`r11` freely; they are caller-saved. The stub
called into C without saving them first. Whatever `anx_syscall_trap()`
left in those five registers — or whatever code it called left there —
replaced the ring-3 program's own values on return.

The documented syscall convention only names `rax`, `rdi`, `rsi`, and
`rdx` as touched. A ring-3 program has no reason to expect the other
registers to change. This bug broke that contract silently: no
exception, no crash, just wrong values after a syscall. It surfaced
only when the compiler happened to keep live state in one of the five
affected registers across a syscall call site.

## Verified: `UOR-Foundation/prism`

`uor-prism-tensor` is `no_std` by default, with `#![forbid(unsafe_code)]`
in its own source, and needs no `default-features = false` override —
unlike the Hologram crates in `RELEASE-2026.8.28-1.md`, it never
depended on `std` in the first place. `CpuI8MatmulSquare<4>` multiplies
`A` (`A[i][j] = i - j`, row-major `i8`) by a `4 × 4` identity matrix and
returns `A` unchanged as saturating `i16`. The shell command
`exec /bin/prism5`, run against a QEMU boot of `anunix-qemu.elf` on
2026-08-30, printed:

```
prism_tensor::CpuI8MatmulSquare<4> on Anunix (real UOR-Foundation/prism code):
0 -1 -2 -3 1 0 -1 -2 2 1 0 -1 3 2 1 0
exec: exit_status=0
```

Every value matches `A[i][j] = i - j` in row-major order. Exit status
`0` confirms the test binary checked all 16 cells before calling
`ANX_SYSCALL_EXIT`.

## How the bug surfaced

The first build of this demo printed a garbled result and an
unexplained `exit_status=11`. A debug loop counter came back
corrupted, incrementing by `16` instead of `1` per iteration. Removing
the debug prints changed the failure again, to a silently-truncated
loop instead of a crash. Both symptoms match register corruption that
depends on exact code layout, not the exec path's control flow. After
the fix, a `trip_count` probe confirmed all 16 loop iterations ran and
every comparison passed, before the final, clean version above.

## Fixed API

```c
/* kernel/include/anx/posix.h */
/*
 * All other registers, including rcx and r8-r11, keep their values
 * across the trap — isr_stub_syscall (usermode.S) saves and restores
 * them around the call into C.
 */
```

## Tests

Host-native suite, `make test` on `hyde`:

```
=== Results: 59 passed, 0 failed ===
```

The register-preservation fix touches only `kernel/arch/x86_64/usermode.S`
and a doc comment in `posix.h`; no test changed.

## Breaking changes

None. The syscall trap's documented inputs and outputs (`rax`, `rdi`,
`rsi`, `rdx`) are unchanged. This release only stops the trap from
touching registers the convention never claimed.

## Relationship to `RELEASE-2026.8.28-1.md`

That release verified `hologram-types`, `hologram-compute`, and the
`hologram-compiler`/`hologram-exec` round trip. All three depend on
`uor-prism` and `uor-prism-tensor` — the same crates this release
verifies directly. `CpuI8MatmulSquare` is Prism's own reference kernel.
Hologram's `HologramF32MatmulSquare` (2026.8.28-1) reaches the same
`TensorAxis` trait through a Hologram-authored `f32` kernel, in place
of Prism's `i8` one.

## Forward-looking

- **`uor-prism-crypto`'s `Digest`** and **`uor-addr`'s κ-address
  computation** — Prism's content-addressing layer, not yet verified
  on Anunix.
- **A `uor-prism-numerics` demo** exercising fixed-point or quantized
  arithmetic directly, not only through a tensor kernel.
- **An audit of every `int 0x80` call site** in `RELEASE-2026.8.28-1.md`'s
  Hologram demos, to confirm none silently relied on the now-fixed
  register-clobber bug for correctness by accident.
