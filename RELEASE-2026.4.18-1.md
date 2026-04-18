# Anunix 2026.4.18-1 Release Notes

Follow-up to the 2026.4.17 release, collecting input, discovery, tooling,
and developer-experience work from Apr 17–18. A second 2026.4.18 release
(2026.4.18-2) is planned for the evening of Apr 18 with the in-flight
crypto/SSH/tensor subsystem work.

## Highlights

- **USB HID mouse** — boot-protocol driver brings pointer input to bare-metal and VM targets.
- **Hardware discovery agent (`hwd`)** — unified PCI/ACPI/hwprobe walker exposed as an ansh command (RFC-0011).
- **RFC-0013 Tensor Objects and Model Representation** — design landed for first-class tensor State Objects and model namespaces. Implementation is in flight for 2026.4.18-2.
- **Cross-platform build** — kernel, ISO, and UTM tooling all work on both macOS and Linux hosts without per-platform branches in the common path.
- **Claude Code skills for Anunix** — four slash-command workflows (`/anunix-build`, `/anunix-deploy`, `/anunix-exec`, `/anunix-test`) shipping in `.claude/skills/` for AI-assisted development.

## New Features

### Input
- **USB HID boot-protocol mouse driver** (`4435248`). Handles usage page 0x01 / usage 0x02 devices with 3- or 4-byte HID boot reports. Works with USB mice and PS/2 trackballs behind USB adapters.

### Discovery
- **`hwd` — hardware discovery agent** (`667d85d`, RFC-0011). Backed by PCI, ACPI, and hwprobe; surfaced as an ansh command. Lays groundwork for capability-aware scheduling and driver autoload.

### Developer Experience
- **Claude Code skills** (`dc845fa`). Four SKILL.md files under `.claude/skills/` collapse common dev loops into single slash commands:
  - `/anunix-build [arch]` — compile kernel, rebuild QEMU wrapper, run host tests.
  - `/anunix-deploy` — scp to Jekyll and boot under QEMU with HTTP port forwarding.
  - `/anunix-exec <command>` — send ansh commands to a live VM via the HTTP API.
  - `/anunix-test` — full unit + live integration suite.

## RFCs

- **RFC-0013 — Tensor Objects and Model Representation** (`ffc61b3`). Introduces `ANX_OBJ_TENSOR` as a first-class kernel type. Models become namespaces of tensor objects with a manifest layer. Implementation lands in 2026.4.18-2.

## Build & Tooling Fixes

- **Cross-platform ISO build** (`fc5cebc`). `tools/build-iso.sh` now uses `mtools` (mformat/mcopy) on Linux and `hdiutil`/`newfs_msdos` on macOS, producing byte-equivalent EFI images.
- **Cross-platform build scripts** (`505c603`). All remaining tooling (toolchain fetch, VM helpers) now runs unchanged on macOS and Linux.
- **FAT12 for 8MB EFI image on Linux** (`e39df05`). Removed `-F` from `mformat`; the flag forces FAT32, but an 8MB volume has ~16k clusters — below the FAT32 minimum of 65525. OVMF correctly rejected the malformed filesystem and dropped to the UEFI boot manager. Default FAT12 matches the macOS path (`newfs_msdos -F 12`).
- **UTM 4.x plist schema for x86_64 VMs** (`6fae647`, `99b5d64`). Rewrote `utm-create-vm.py` against the real UTM 4.x plist structure — singular key names, no `System.Boot` dict, `Serial: []` in place of unsupported terminal config, `Notes` removed from `Information`. VMs now open cleanly in UTM 4.x.

## Statistics

- **9 commits** since 2026.4.17
- **1 new RFC** (0013)
- **2 new subsystems** (USB HID mouse, `hwd`)
- **4 new Claude Code skills**

## Known Issues

- Bare-metal UEFI boot on real firmware still intermittent (carried over from 2026.4.16).
- Tensor and model subsystem code not yet merged — lands in 2026.4.18-2.
- Shell scripting beyond `if/then/end` (loops, job control) still not implemented.

## What's Next (2026.4.18-2, tonight)

- Tensor objects and ops (matmul, ReLU, transpose, quantize, BRIN stats).
- Model namespace (import/info/layers/diff, safetensors support).
- Crypto library and SSHd.
- httpd / tcp_server refactor.
- `ansh` tensor/model command wiring.
