---
name: anunix-build
description: Build the Anunix kernel for x86_64 or arm64, run tests, and rebuild the QEMU wrapper. Use when compiling, testing, or preparing for deployment.
argument-hint: [arch]
disable-model-invocation: false
allowed-tools: Bash(make *) Read Grep
when_to_use: build kernel, compile, run tests, check build
---

# Anunix Kernel Build

Build the Anunix kernel and run tests.

## Arguments

- `$ARGUMENTS` — target architecture: `x86_64` (default) or `arm64`

## Steps

1. Determine architecture (default to x86_64 if not specified):
   ```bash
   ARCH=${ARGUMENTS:-x86_64}
   ```

2. Build the kernel:
   ```bash
   make kernel ARCH=$ARCH
   ```

3. If x86_64, also rebuild the QEMU wrapper:
   ```bash
   rm -f build/x86_64/anunix-qemu.elf build/x86_64/qemu_boot.o
   make build/x86_64/anunix-qemu.elf ARCH=x86_64
   ```

4. Run host-native tests:
   ```bash
   make test
   ```

5. Report results: binary size, test pass/fail counts, any errors.

## Notes

- arm64 kernel compiles all `.c` files but link fails on missing arch symbols (boot stub not yet implemented) — compilation-only validation is still useful.
- Tests run on host (macOS/Linux), not in QEMU. They use mock_arch.c stubs.
- The QEMU wrapper (`anunix-qemu.elf`) embeds the raw kernel binary and must be rebuilt after any kernel change.
