---
name: anunix-deploy
description: Build Anunix on Hyde, copy to Jekyll, write ISO to /dev/sda, then boot in QEMU on Jekyll for testing. Use after every kernel change.
disable-model-invocation: false
allowed-tools: Bash(ssh *) Bash(scp *)
when_to_use: deploy to jekyll, boot qemu, test on vm, live test, after any kernel change
---

# Deploy Anunix: Hyde → Jekyll

Build happens on Hyde. Hyde deploys directly to Jekyll. This Mac is not in the data path.

## Full Deploy Sequence (run after every change)

```bash
# 1. Pull latest and rebuild kernel + QEMU wrapper on Hyde
ssh adam@hyde "cd ~/Development/Anunix/Anunix && git pull --ff-only"
ssh adam@hyde "cd ~/Development/Anunix/Anunix && make kernel ARCH=x86_64"
ssh adam@hyde "cd ~/Development/Anunix/Anunix && rm -f build/x86_64/anunix-qemu.elf build/x86_64/qemu_boot.o && make build/x86_64/anunix-qemu.elf ARCH=x86_64"

# 2. Build ISO on Hyde
ssh adam@hyde "cd ~/Development/Anunix/Anunix && make iso"

# 3. Copy ISO + QEMU ELF from Hyde directly to Jekyll
ssh adam@hyde "scp ~/Development/Anunix/Anunix/build/anunix-x86_64.iso jekyll:/tmp/anunix-x86_64.iso"
ssh adam@hyde "scp ~/Development/Anunix/Anunix/build/x86_64/anunix-qemu.elf jekyll:/tmp/anunix-qemu.elf"

# 4. Write ISO to /dev/sda on Jekyll (3.8 GB USB drive)
ssh adam@hyde "ssh jekyll 'sudo dd if=/tmp/anunix-x86_64.iso of=/dev/sda bs=4M status=progress oflag=sync'"
```

## QEMU Smoke Test (run from Hyde → Jekyll)

```bash
ssh adam@hyde "ssh jekyll 'timeout 30 qemu-system-x86_64 -m 2G -nographic -no-reboot \
  -serial mon:stdio -netdev user,id=net0,hostfwd=tcp::18080-:8080 \
  -device virtio-net-pci,netdev=net0 -kernel /tmp/anunix-qemu.elf &
sleep 8
curl -s http://localhost:18080/api/v1/health
echo \"\"
curl -s -X POST http://localhost:18080/api/v1/exec \
  -H \"Content-Type: application/json\" \
  -d \"{\\\"command\\\": \\\"sysinfo\\\"}\"
kill %1 2>/dev/null'"
```

## Notes

- Hyde: `adam@hyde` (100.92.49.6), repo at `~/Development/Anunix/Anunix`
- Jekyll: `jekyll` (100.100.56.37) — x86_64 QEMU test target, reachable directly from Hyde
- `/dev/sda` on Jekyll is a 3.8 GB USB drive — always the Anunix deploy target
- QEMU uses virtio-net + SLIRP; port 8080 inside VM → 18080 on Jekyll
- Boot takes ~8 seconds (DHCP is the slowest part)
- `make iso` requires `make iso-deps` to have been run once on Hyde
