# Anunix 2026.9.4 Release Notes

Milestone: **software RAID.** Anunix stripes across NVMe drives for
throughput and mirrors them for redundancy. The object store mounts from
the array, so the operating system runs on the array without knowing an
array exists. Building it exposed three defects in the NVMe driver. No
NVMe controller bound before those fixes. It exposed two more defects in
the ISO build, which left the UEFI image unbootable.

## Requirement keywords

These notes use `must` and `can` in the imperative. They do not use the
RFC 2119 keywords.

## Highlights

- **RAID 0 and RAID 1.** `kernel/core/md/` combines block devices into
  one array device. RAID 0 stripes on a power-of-two chunk, 64 KiB by
  default. RAID 1 mirrors, reads round-robin, runs degraded, and
  rebuilds onto a replacement member.
- **A block device registry.** Every drive a driver finds gets its own
  device and name: `nvme0`, `nvme1`, `ahci0`, `md0`. The block layer
  held exactly one device before this release.
- **Arrays assemble themselves at boot.** Each member carries a
  512-byte superblock. The kernel groups members by array UUID, drops
  members whose event counter is stale, and starts what it can.
- **Three NVMe defects fixed.** No NVMe controller reached
  `CSTS.RDY = 1` before this release.
- **Two ISO defects fixed.** The published image now boots under UEFI.
- **arm64 links again.** It does not boot. See Known issues.

## Software RAID

`RFC-0030` specifies the design. The short form:

| Level | Capacity | Loses a member |
| --- | --- | --- |
| RAID 0 | sum of members | array fails, restore from backup |
| RAID 1 | smallest member | array runs degraded, rebuild onto a spare |

Metadata sits at the head of each member by default, with data starting
1 MiB in. An array created with `tail` metadata puts array sector 0 at
member sector 0. UEFI firmware then reads a mirrored EFI system
partition without understanding the array.

Administer arrays with `raid`:

```
raid list                                     print one line per array
raid devs                                     print every block device and its role
raid detail <name>                            print members and geometry
raid create <0|1> <chunkKiB|-> <dev>... [tail] [force]
raid stop|assemble|fail|add|resync|zero       administration
```

`raid create` erases every listed device. There is no reversal.

The installer builds an array from the `install.raid` object in the
provisioning config, before it partitions the disk:

```json
{
  "install": {
    "raid": {
      "level": "raid0",
      "chunk_kib": 64,
      "metadata": "head",
      "members": ["nvme0", "nvme1"]
    }
  }
}
```

`make qemu-raid` boots with three emulated 256 MiB NVMe drives.

## The NVMe defects

All three predate this release and affect any NVMe drive, with or
without RAID.

**Queue pages overlapped.** The driver placed the admin completion
queue 2048 bytes into the page holding the admin submission queue. It
placed the I/O completion queue 4096 bytes into the page holding the
I/O submission queue, which put it outside that page entirely. The
controller needs a page-aligned base for each queue, so `CSTS.RDY`
never reached 1 and every probe reported
`nvme: timeout waiting for ready`. Each queue now gets its own page.

**An unclaimed completion stalled the queue forever.** `nvme_poll()`
skipped a completion queue entry whose command ID did not match,
without consuming the entry. One timed-out command left its completion
at the queue head. Every later command on that queue then spun its
whole budget against an entry nobody would claim. The driver keeps one
command outstanding, so the entry at the head belongs to the command
being polled. The driver now consumes it whichever command ID it carries.

**The completion poll budget was too short.** The budget was 500000
loop iterations. Metadata reads and writes finished inside it. A RAID 1
rebuild, which reads every sector of a member, timed out and faulted the
healthy member. `NVME_POLL_ITERS` is now 20000000.

## The ISO defects

Both predate this release. `make qemu-iso` produced no boot option.

**The EFI system partition was formatted FAT32 at 8 MiB.** FAT32 needs
at least 65525 clusters, which an 8 MiB volume cannot supply. `mtools`
wrote the out-of-spec filesystem without complaint and EDK2 refused to
mount it, so the firmware found nothing to boot. `tools/build-iso.sh`
no longer passes `-F` to `mformat`, which gives FAT12 at that size.

**The anxboot stub never reached the image.** `config/grub.cfg`
chainloads `/EFI/BOOT/ANUNIX.EFI`, and nothing copied it there. GRUB
loaded, drew its menu, and every entry failed with
``error: file `/EFI/BOOT/ANUNIX.EFI' not found``. The stub is now
staged into both the ISO tree and the EFI system partition.

## Security

`config/grub.cfg` carried a Wi-Fi SSID and pre-shared key on the
`Anunix (WiFi)` boot entry. The credentials are removed, so the
published image contains no key. Supply Wi-Fi credentials through
kickstart provisioning instead, or append `cred:wifi-ssid=` and
`cred:wifi-pass=` to that line on a machine you control.

The key remains in the public git history of this repository, in commits
reachable from `main`. Rotate the pre-shared key. Removing it from the
current tree does not withdraw it from history.

## Verification

| Check | Result |
| --- | --- |
| `make test` | 60 suites, 0 failed |
| `make kernel ARCH=x86_64` | builds |
| `make kernel ARCH=arm64` | builds, does not boot |
| `make iso` | 27 MB hybrid ISO |
| ISO under UEFI | boots to `ansh` |

RAID 0 across two emulated NVMe drives, QEMU 11.1.0:

```
anx> raid create 0 64 nvme0 nvme1 force
md: md0: raid0, 2 members, 510 MiB, clean
anx> store format
disk: formatted 'anunix' (510 MiB)
anx> write posix:/raidtest.txt hello-from-raid0
created 00000001-6a85-797a-960a-93c7874a97c8 (16 bytes) -> posix:/raidtest.txt
```

After a reboot on the same drives:

```
md: md0: raid0, 2 members, 510 MiB, clean
md: 1 array(s) assembled
anx> cat posix:/raidtest.txt
hello-from-raid0
```

RAID 1 losing a member and rebuilding onto a replacement:

```
anx> raid fail md0 0
md: md0: member 0 (nvme0) failed
md0     raid1   2 members  255 MiB  degraded
anx> cat posix:/mirror.txt
mirrored-payload
anx> raid add md0 nvme2
anx> raid resync md0
md: md0: rebuild complete, member 0 in sync
md0     raid1   2 members  255 MiB  clean
```

## Known issues

**arm64 builds but does not boot.** The ring-3 execution primitives were
x86_64-only since 2026.8.28-1, which left three symbols undefined and
broke the arm64 link. This release defines them as stubs that refuse
rather than half-work, so the kernel links. Booting then reaches
`anx_sink_registry_init()` and takes an alignment fault:

```
ESR_EL1 0x96000061 (EC 0x25 data abort, DFSC 0x21), FAR_EL1 0x4037dc8a
```

The port runs with the MMU off, so all memory is Device-nGnRnE and any
unaligned access faults. arm64 page-table bring-up is the fix. No arm64
binary ships with this release.

**Ring-3 execution is x86_64 only.**

**RAID has no parity levels.** RAID 5 and RAID 6 need a
read-modify-write path.

**A RAID 1 rebuild blocks its caller** and copies every sector. There is
no write-intent bitmap, so an unclean shutdown of a degraded array needs
a full rebuild.

**RAID members are whole devices.** The block layer exposes no partition
devices.

**ACPI reports `RSDP not found` under UEFI.** The firmware passes the
tables through the UEFI configuration table, and the kernel scans for a
BIOS-style RSDP.

## Upgrading

The array format is new, so nothing needs migrating. An existing
single-drive install keeps working: the first device registered becomes
the active device, exactly as before.

To move an existing install onto a striped array, back the object store
up first. `raid create` erases every member, and `store format` erases
the array.
