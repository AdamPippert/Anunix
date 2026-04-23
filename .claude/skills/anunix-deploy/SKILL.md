---
name: anunix-deploy
description: Deploy Anunix kernel to Jekyll (Linux server): build ISO, copy to jekyll, write to /dev/sda, then boot in QEMU for testing. Use after every kernel change.
disable-model-invocation: false
allowed-tools: Bash(scp *) Bash(ssh *) Bash(make *)
when_to_use: deploy to jekyll, boot qemu, test on vm, live test, after any kernel change
---

# Deploy Anunix to Jekyll

After every kernel change, three things must happen in order:
1. Build the ISO
2. Copy the ISO to Jekyll
3. Write the ISO to `/dev/sda` on Jekyll (3.8 GB USB drive)

Then optionally boot in QEMU to validate.

## Full Deploy Sequence (run after every change)

```bash
# 1. Rebuild kernel + QEMU wrapper
make kernel ARCH=x86_64
rm -f build/x86_64/anunix-qemu.elf build/x86_64/qemu_boot.o
make build/x86_64/anunix-qemu.elf ARCH=x86_64

# 2. Build ISO
make iso

# 3. Copy ISO + QEMU ELF to Jekyll
scp build/anunix-x86_64.iso jekyll:/tmp/anunix-x86_64.iso
scp build/x86_64/anunix-qemu.elf jekyll:/tmp/anunix-qemu.elf

# 4. Write ISO to /dev/sda on Jekyll
ssh jekyll "sudo dd if=/tmp/anunix-x86_64.iso of=/dev/sda bs=4M status=progress oflag=sync"
```

## QEMU Smoke Test (automated, with timeout)

Run after the ISO is written to confirm the kernel boots and the API responds:

```bash
ssh jekyll 'timeout 30 qemu-system-x86_64 -m 2G -nographic -no-reboot \
  -serial mon:stdio -netdev user,id=net0,hostfwd=tcp::18080-:8080 \
  -device virtio-net-pci,netdev=net0 -kernel /tmp/anunix-qemu.elf &
sleep 8
curl -s http://localhost:18080/api/v1/health
echo ""
curl -s -X POST http://localhost:18080/api/v1/exec \
  -H "Content-Type: application/json" \
  -d "{\"command\": \"sysinfo\"}"
kill %1 2>/dev/null'
```

## Notes

- Jekyll is at 100.100.56.37 (Tailscale alias: `jekyll`), user `adam`
- `/dev/sda` is a 3.8 GB USB drive — always the Anunix deploy target
- QEMU uses virtio-net + SLIRP; port 8080 inside VM → 18080 on Jekyll
- Boot takes ~8 seconds (DHCP is the slowest part)
- `make iso` requires `make iso-deps` to have been run once (tools/grub/ must exist)
