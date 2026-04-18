---
name: anunix-test
description: Run the full Anunix test suite — host-native unit tests and optionally live QEMU integration tests. Use after making code changes to verify nothing is broken.
disable-model-invocation: false
allowed-tools: Bash(make *) Bash(ssh *) Read
when_to_use: run tests, verify changes, check for regressions, test suite
---

# Anunix Test Suite

Run unit tests and optionally live VM tests.

## Host-Native Unit Tests

```bash
make test
```

Expected output: `=== Results: N passed, 0 failed ===`

Current test suites (15):
- state_object, cell_lifecycle, cell_runtime, memplane
- engine_registry, scheduler, capability, fb
- engine_lifecycle, resource_lease, model_server, posix
- tensor, model, tensor_ops

## Live QEMU Integration Tests

Deploy and test on Jekyll:

```bash
# Build and deploy
make kernel ARCH=x86_64
rm -f build/x86_64/anunix-qemu.elf build/x86_64/qemu_boot.o
make build/x86_64/anunix-qemu.elf ARCH=x86_64
scp build/x86_64/anunix-qemu.elf jekyll:/tmp/anunix-qemu.elf

# Boot and test via HTTP API
ssh jekyll 'qemu-system-x86_64 -m 512M -nographic -no-reboot -serial mon:stdio \
  -netdev user,id=net0,hostfwd=tcp::18080-:8080 \
  -device virtio-net-pci,netdev=net0 \
  -kernel /tmp/anunix-qemu.elf &
sleep 6

# Health check
curl -s http://localhost:18080/api/v1/health

# Tensor smoke test
curl -s -X POST http://localhost:18080/api/v1/exec \
  -H "Content-Type: application/json" \
  -d "{\"command\": \"tensor create default:/t 4,4 int8\"}"
curl -s -X POST http://localhost:18080/api/v1/exec \
  -H "Content-Type: application/json" \
  -d "{\"command\": \"tensor fill default:/t range\"}"
curl -s -X POST http://localhost:18080/api/v1/exec \
  -H "Content-Type: application/json" \
  -d "{\"command\": \"tensor stats default:/t\"}"

# System check
curl -s -X POST http://localhost:18080/api/v1/exec \
  -H "Content-Type: application/json" \
  -d "{\"command\": \"sysinfo\"}"

kill %1 2>/dev/null'
```

## Verification Checklist

After changes, verify:
1. `make kernel ARCH=x86_64` — zero errors/warnings
2. `make test` — all tests pass
3. `make kernel ARCH=arm64` — all `.c` files compile (link may fail on missing arch symbols, that's expected)
4. Live QEMU test — health endpoint responds, basic tensor ops work
