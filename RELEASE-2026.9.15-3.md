# Anunix 2026.9.15-3

A safety release. It merges the partition layer (RFC-0031) into the release
line and supersedes 2026.9.15-2 on any machine that runs Anunix beside another
operating system.

## Why this release exists

2026.9.15-2 could destroy the partition table of a disk it was booted beside.

`main.c` formatted the active block device whenever it failed to find an object
store on it. The active device is whichever device registers first, and the
storage drivers register a whole namespace, not a partition. On a partitioned
machine the active device is therefore the whole disk, whose first sector is a
protective MBR and never an Anunix superblock. `anx_disk_format()` then zeroed
the first 513 sectors, taking the protective MBR, the GPT header and the head
of the EFI system partition with it. It read nothing before writing.

The fix was written and pushed before 2026.9.15-2, on
`rfc-0031-partition-layer`, and was not merged. That release cherry-picked the
MMIO commit from the branch and left the four commits that matter here.

## What changed

| Commit | Effect |
|---|---|
| `5dac351` | registers GPT partitions as bounded block devices |
| `c8d7b0a` | chooses the active device by its superblock |
| `7f829ef` | refuses to format a device Anunix did not write |
| `8de5bc6` | installer accepts a partition or an array as a target |

Anunix now enumerates partitions, selects the one carrying its superblock, and
runs without persistence rather than formatting when it finds no store. Running
without a store is recoverable. Formatting somebody's disk is not.

The partition layer is also what lets Anunix reach a store at all when that
store lives in a partition. Without it Anunix can address only whole disks.

## Tests

`make test` reports 67 passed, 0 failed, up from 64. The three added suites are
`part`, `blk_probe` and `blk_select`. The Python suites report 9 and 12. The
x86_64 kernel, the arm64 kernel and the ISO all build.

## Boot validation

Two QEMU disks were built with a GPT holding an EFI system partition, an
`ANUNIX_RAID0_A` partition and a third partition, then booted under OVMF with
the store presented as an NVMe namespace.

With a store present, Anunix enumerated all three partitions, reported
`object store found on nvme0p2`, made it the active device and mounted it.

With no store present, Anunix reported `no object store on nvme0 (holds MBR
partition table)` and `running without persistence`, and did not format.

A byte-for-byte comparison against a pristine copy of the disk shows that only
the Anunix partition was written. The primary GPT, the EFI system partition,
the third partition and the backup GPT are unchanged. The store superblock kept
its object count across the boot; the journal head and commit pointers match,
so no recovery is pending.

## Conflicts resolved in the merge

`ANX_ERANGE` and `ANX_ECANCELED` both claimed -16. The research line's
`ANX_ECANCELED -16` and `ANX_EAUDIT -17` are kept. `ANX_ERANGE` moves to -18,
the first free code. The Makefile, the test registry and the RFC index each
took the union of both sides.

## Not tested on real hardware

This release has not booted on the Framework Laptop 16. The USB, I2C and
Wi-Fi work it carries has never run on that machine. See RELEASE-2026.9.15-2.md
for the full list.
