---
name: anunix-test
description: Run the full Anunix test suite on Hyde — host-native unit tests and optionally live QEMU integration tests on Jekyll. Use after making code changes to verify nothing is broken.
disable-model-invocation: false
allowed-tools: Bash(ssh *) Read
when_to_use: run tests, verify changes, check for regressions, test suite
---

# Anunix Test Suite

Unit tests run on Hyde. Live QEMU tests run on Jekyll (triggered from Hyde).

## Host-Native Unit Tests (on Hyde)

```bash
ssh adam@hyde "cd ~/Development/Anunix/Anunix && make test"
```

Expected output: `=== Results: N passed, 0 failed ===`

Current test suites (15):
- state_object, cell_lifecycle, cell_runtime, memplane
- engine_registry, scheduler, capability, fb
- engine_lifecycle, resource_lease, model_server, posix
- tensor, model, tensor_ops

## Live QEMU Integration Tests (Hyde builds, Jekyll runs)

```bash
# Build on Hyde
ssh adam@hyde "cd ~/Development/Anunix/Anunix && make kernel ARCH=x86_64"
ssh adam@hyde "cd ~/Development/Anunix/Anunix && rm -f build/x86_64/anunix-qemu.elf build/x86_64/qemu_boot.o && make build/x86_64/anunix-qemu.elf ARCH=x86_64"

# Copy from Hyde to Jekyll
ssh adam@hyde "scp ~/Development/Anunix/Anunix/build/x86_64/anunix-qemu.elf jekyll:/tmp/anunix-qemu.elf"

# Boot and test via HTTP API on Jekyll
ssh adam@hyde "ssh jekyll 'qemu-system-x86_64 -m 512M -nographic -no-reboot -serial mon:stdio \
  -netdev user,id=net0,hostfwd=tcp::18080-:8080 \
  -device virtio-net-pci,netdev=net0 \
  -kernel /tmp/anunix-qemu.elf &
sleep 6

curl -s http://localhost:18080/api/v1/health

curl -s -X POST http://localhost:18080/api/v1/exec \
  -H \"Content-Type: application/json\" \
  -d \"{\\\"command\\\": \\\"tensor create default:/t 4,4 int8\\\"}\"
curl -s -X POST http://localhost:18080/api/v1/exec \
  -H \"Content-Type: application/json\" \
  -d \"{\\\"command\\\": \\\"tensor fill default:/t range\\\"}\"
curl -s -X POST http://localhost:18080/api/v1/exec \
  -H \"Content-Type: application/json\" \
  -d \"{\\\"command\\\": \\\"tensor stats default:/t\\\"}\"

curl -s -X POST http://localhost:18080/api/v1/exec \
  -H \"Content-Type: application/json\" \
  -d \"{\\\"command\\\": \\\"sysinfo\\\"}\"

kill %1 2>/dev/null'"
```

## Verification Checklist

After changes, verify:
1. `make kernel ARCH=x86_64` on Hyde — zero errors/warnings
2. `make test` on Hyde — all tests pass
3. `make kernel ARCH=arm64` on Hyde — all `.c` files compile (link may fail on missing arch symbols, that's expected)
4. Live QEMU test on Jekyll — health endpoint responds, basic tensor ops work
