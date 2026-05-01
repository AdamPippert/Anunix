---
name: anunix-build
description: Build the Anunix kernel for x86_64 or arm64 on Hyde (build machine). Run tests and rebuild the QEMU wrapper. Use when compiling, testing, or preparing for deployment.
argument-hint: [arch]
disable-model-invocation: false
allowed-tools: Bash(ssh *) Bash(scp *) Read
when_to_use: build kernel, compile, run tests, check build
---

# Anunix Kernel Build

All builds run on Hyde (`adam@hyde`, `~/Development/Anunix/Anunix`).

## Arguments

- `$ARGUMENTS` — target architecture: `x86_64` (default) or `arm64`

## Steps

1. Determine architecture (default to x86_64 if not specified):
   ```bash
   ARCH=${ARGUMENTS:-x86_64}
   ```

2. Pull latest code on Hyde:
   ```bash
   ssh adam@hyde "cd ~/Development/Anunix/Anunix && git pull --ff-only"
   ```

3. Build the kernel:
   ```bash
   ssh adam@hyde "cd ~/Development/Anunix/Anunix && make kernel ARCH=$ARCH"
   ```

4. If x86_64, also rebuild the QEMU wrapper:
   ```bash
   ssh adam@hyde "cd ~/Development/Anunix/Anunix && rm -f build/x86_64/anunix-qemu.elf build/x86_64/qemu_boot.o && make build/x86_64/anunix-qemu.elf ARCH=x86_64"
   ```

5. Run host-native tests on Hyde:
   ```bash
   ssh adam@hyde "cd ~/Development/Anunix/Anunix && make test"
   ```

6. Report results: binary size, test pass/fail counts, any errors.

## Notes

- Hyde is at `100.92.49.6` (Tailscale alias `hyde`), user `adam`.
- Hyde uses system `ld.lld` 22 and `clang` 22 — no local toolchain download needed.
- arm64 kernel compiles all `.c` files but link fails on missing arch symbols (boot stub not yet implemented) — compilation-only validation is still useful.
- Tests run on Hyde (Linux x86_64), not in QEMU. They use mock_arch.c stubs.
- The QEMU wrapper (`anunix-qemu.elf`) embeds the raw kernel binary and must be rebuilt after any kernel change.
