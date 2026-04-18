---
name: anunix-deploy
description: Deploy Anunix kernel to Jekyll (Linux server) and boot in QEMU for testing. Use when you need to test on a real QEMU instance or validate network/hardware features.
disable-model-invocation: false
allowed-tools: Bash(scp *) Bash(ssh *) Bash(make *)
when_to_use: deploy to jekyll, boot qemu, test on vm, live test
---

# Deploy Anunix to Jekyll QEMU

Build, deploy, and boot Anunix on Jekyll's QEMU for live testing.

## Prerequisites

- Kernel must be built first (`/anunix-build` or `make kernel ARCH=x86_64`)
- Jekyll accessible via SSH: `ssh jekyll` (100.100.56.37, user adam)
- QEMU installed on Jekyll: `/usr/bin/qemu-system-x86_64`

## Steps

1. Ensure QEMU wrapper is current:
   ```bash
   rm -f build/x86_64/anunix-qemu.elf build/x86_64/qemu_boot.o
   make build/x86_64/anunix-qemu.elf ARCH=x86_64
   ```

2. Deploy to Jekyll:
   ```bash
   scp build/x86_64/anunix-qemu.elf jekyll:/tmp/anunix-qemu.elf
   ```

3. Boot with port forwarding (HTTP API on 18080, serial on stdio):
   ```bash
   ssh jekyll "qemu-system-x86_64 -m 512M -nographic -no-reboot -serial mon:stdio \
     -netdev user,id=net0,hostfwd=tcp::18080-:8080 \
     -device virtio-net-pci,netdev=net0 \
     -kernel /tmp/anunix-qemu.elf"
   ```

4. In a separate connection, test the HTTP API:
   ```bash
   ssh jekyll "curl -s http://localhost:18080/api/v1/health"
   ssh jekyll "curl -s -X POST http://localhost:18080/api/v1/exec \
     -H 'Content-Type: application/json' \
     -d '{\"command\": \"sysinfo\"}'"
   ```

## Automated Test Script

For non-interactive testing with a timeout:
```bash
ssh jekyll 'timeout 30 qemu-system-x86_64 -m 512M -nographic -no-reboot \
  -serial mon:stdio -netdev user,id=net0,hostfwd=tcp::18080-:8080 \
  -device virtio-net-pci,netdev=net0 -kernel /tmp/anunix-qemu.elf &
sleep 6
curl -s http://localhost:18080/api/v1/health
curl -s -X POST http://localhost:18080/api/v1/exec \
  -H "Content-Type: application/json" \
  -d "{\"command\": \"$ARGUMENTS\"}"
kill %1 2>/dev/null'
```

## Notes

- QEMU uses virtio-net with SLIRP networking (10.0.2.x)
- DHCP auto-configures IP, gateway, DNS
- Port 8080 inside VM is forwarded to 18080 on Jekyll
- Boot takes ~6 seconds (DHCP discovery is the slowest part)
- The VM has 512M RAM but kernel heap is 16 MiB (linker script)
