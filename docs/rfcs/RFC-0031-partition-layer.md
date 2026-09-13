# RFC-0031: Partition Layer — Block Devices Backed by GPT Partitions

| Field      | Value                                                            |
|------------|------------------------------------------------------------------|
| RFC        | 0031                                                             |
| Title      | Partition Layer — Block Devices Backed by GPT Partitions         |
| Author     | Adam Pippert                                                     |
| Status     | Draft                                                            |
| Created    | 2026-09-12                                                       |
| Depends On | RFC-0002 (State Object Model), RFC-0030 (Software RAID)          |
| Blocks     | —                                                                |

This RFC uses the RFC 2119 requirement keywords `MUST`, `MUST NOT`, `SHOULD`,
and `MAY`, as RFC 8174 amends them.

---

## Executive Summary

Anunix registers whole drives. `nvme0` is a drive, `md0` is an array, and there
is nothing in between. The object store writes its superblock to sector 0 of
whatever device it is given, so installing Anunix means giving it a drive and
losing everything already on that drive.

That is the wrong trade on every machine anyone actually owns. A Framework
Laptop 16 with Fedora on it has no spare drive, and it never will.

This RFC adds one layer. A partition is a block device that forwards reads and
writes to its parent with an LBA offset added and a length clamped. Everything
above the block layer — the object store, the RAID layer, the installer — works
unchanged, because a partition presents exactly the interface a drive presents.

Four pieces make that work:

1. **A GPT scan at probe time.** Each drive that registers gets its partition
   table read. Each usable entry becomes its own block device: `nvme0p2`.
2. **Offset and clamp.** The partition's operations add `start_lba` and reject
   any request that would cross `sectors`. This is the safety boundary that
   keeps Anunix inside its partition and off the host's filesystems.
3. **Superblock-directed activation.** The kernel picks the active device by
   finding an Anunix object store superblock, instead of taking whichever
   device registered first.
4. **No formatting without being asked.** The boot path currently writes a
   fresh object store over the active device whenever it fails to find one,
   with no check for a partition table or filesystem. That is removed, and
   `anx_disk_format()` gains a guard (§8). Of everything here this is the
   piece that matters most: the bounds and the selection rules all exist to
   keep Anunix off the host's data, and they count for nothing while the
   boot path erases the disk before anyone asks it to.

---

## 1. Motivation

### 1.1 The sector-zero problem

`ANX_SUPER_SECTOR` is 0 (`kernel/include/anx/objstore_disk.h`). The object store
superblock, journal, index, and data all address the device from its first
sector. `anx_disk_format()` writes sector 0 of the active device.

On a drive holding a GPT, sector 0 is the protective MBR and sector 1 is the GPT
header. Formatting an object store onto such a drive destroys the partition
table and every filesystem the table described.

`kernel/core/install/gpt.c` compounds this. `anx_gpt_create_default()` writes a
fresh protective MBR, a primary GPT, and a backup GPT across the whole device.
The installer's only disk model is "this drive is now mine."

### 1.2 The machines this has to run on

Both target machines run Fedora Sway Atomic and have reserve partitions already
cut for Anunix:

| Machine | Model | Reserve partitions |
| --- | --- | --- |
| jekyll | Framework Laptop 16 (Ryzen AI 9 HX 370) | `nvme1n1p2` `ANUNIX_RAID0_A`, `nvme0n1p2` `ANUNIX_RAID0_B`, 25 GiB each |
| hyde | Framework Desktop (Ryzen AI MAX+ 395) | none yet; to be cut on rebuild |

Neither machine has a spare drive. Both have two NVMe drives carrying an ESP, a
`/boot` RAID 1, a root RAID 0, and data. Anunix gets a partition on each drive or
it gets nothing.

The reserve partitions on jekyll are deliberately paired so RAID 0 (RFC-0030) can
stripe across them, which is what the `_A` and `_B` suffixes mean. That only
works if an array member can be a partition.

### 1.3 What already exists

More of this is built than it appears:

- `crc32()` in `kernel/core/install/gpt.c` — correct, but `static`.
- `anx_gpt_read()` and `anx_gpt_find_type()` in `kernel/include/anx/gpt.h`.
- `struct anx_gpt_partition` with `start_lba`, `end_lba`, type GUID, and name.
- `ANX_GPT_TYPE_ANX_LO` / `ANX_GPT_TYPE_ANX_HI` — an Anunix partition type GUID.
- The synthetic-device pattern, proven by `md` (RFC-0030 §2).

What is missing is that `anx_gpt_read()` reads through the whole-system API, so
it can only read the active device, and nothing turns a parsed entry into a
registered device.

### 1.4 A dependency, discovered on the way

None of this is reachable on a UEFI machine until a driver can read its own
registers. `boot.S` claims "UEFI identity mapping covers all physical memory".
That was true of the firmware's page tables; the anxboot stub then replaces
them with a 4 GiB map of its own (`efi_stub.c`, PML4[0] and PDPT[0..3]) and
the claim stops holding. UEFI puts 64-bit BARs above 4 GiB, so the first
register access faults.

`anx_mmio_map()` (`kernel/arch/x86_64/mmio.c`) adds 1 GiB uncached pages for a
requested physical range, and NVMe and AHCI now map their BARs through it
rather than treating a physical address as a pointer. It is not part of the
partition layer, but the partition layer could not be exercised on the target
machines without it, and §8 depends on knowing it is fixed.

---

## 2. Partition devices

### 2.1 Structure

`kernel/include/anx/part.h` defines the per-partition state.

```c
struct anx_part {
	struct anx_blk_dev *parent;	/* drive or array this sits on */
	uint64_t            start_lba;	/* first sector, absolute on parent */
	uint64_t            sectors;	/* length; the clamp bound */
	uint32_t            index;	/* GPT entry index, 1-based */
	uint64_t            type_lo, type_hi;
	char                label[36];	/* GPT partition name, ASCII */
};
```

The partition is registered with an operations table in the shape `md` already
uses, so one table serves every partition and per-partition state lives in
`priv`.

### 2.2 Offset and clamp

```c
static int part_blk_read(struct anx_blk_dev *dev, uint64_t lba,
			 uint32_t count, void *buf)
{
	struct anx_part *p = dev->priv;

	if (count == 0)
		return ANX_OK;
	if (lba >= p->sectors || count > p->sectors - lba)
		return ANX_ERANGE;

	return anx_blk_dev_read(p->parent, p->start_lba + lba, count, buf);
}
```

`part_blk_write()` is the same with `anx_blk_dev_write()`. `part_blk_capacity()`
returns `p->sectors`.

`ANX_ERANGE` does not exist yet. `kernel/include/anx/types.h` MUST gain
`#define ANX_ERANGE -16 /* request outside device bounds */`. Return codes in
this codebase are already negative at the point of definition — `ANX_EINVAL` is
`-2` — so the value is returned as `ANX_ERANGE`, never `-ANX_ERANGE`. Using
`ANX_EINVAL` instead would work but conflates a malformed argument with a
correctly formed request that simply does not fit, and the clamp is the one
error worth being able to grep for on its own.

Note for whoever writes this: `CLAUDE.md` says error codes live in
`kernel/include/anx/errno.h`. That file does not exist; the codes are in
`types.h`. The guide should be corrected or the file created.

The bounds check is the point of this RFC. An implementation MUST reject any
request whose range is not wholly inside the partition, and MUST perform the
check in a form that cannot overflow — `count > p->sectors - lba` after
establishing `lba < p->sectors`, never `lba + count > p->sectors`.

A partition device MUST NOT be created with `start_lba` of 0 on a parent that
holds a partition table, and MUST NOT extend past the parent's capacity. Both
conditions indicate a malformed table and MUST cause the entry to be skipped.

### 2.5 Lifetime

A partition device is valid only while its parent is. `anx_blk_dev_unregister()`
MUST therefore call `anx_part_forget_children()` before releasing a device that
is not itself a partition, and the partition layer MUST return the released
state to its pool so a drive that comes and goes does not exhaust it.

This was not in the first draft of this RFC. The test suite found it: a second
scan of a re-registered device failed because the names from the first scan
were still held, and the state behind them still pointed at a device that no
longer existed.

### 2.3 Names

`anx_blk_dev_register()` appends the lowest free index to a class name, which
gives `md0`, `nvme1`. Partition names MUST instead be derived from the parent so
the relationship is legible: parent name, `p`, GPT entry index.

```
nvme0        drive
nvme0p2      second GPT entry on that drive
md0p1        first GPT entry on an array
```

This needs a second registration entry point, `anx_blk_dev_register_named()`,
taking a full name rather than a class prefix. `ANX_BLK_NAME_MAX` is 16, which
holds `nvme0p2` and every plausible extension of it.

Partition numbering follows the GPT entry index, not registration order. A
partition's name is therefore stable as long as the table is, and does not shift
when an earlier partition is deleted. This is a deliberate difference from drive
naming, which follows PCI enumeration.

### 2.4 Registry capacity

`ANX_BLK_MAX_DEVS` is 16. jekyll registers 2 drives and 11 partitions, and an
array on top of two of those partitions makes 14. hyde after its rebuild reaches
10. A second array or a third drive overflows.

`ANX_BLK_MAX_DEVS` MUST be raised to 32. The registry is a static array of
`struct anx_blk_dev`; at 32 entries it costs roughly 1.5 KiB of BSS, which is
not a meaningful cost against the benefit of not silently dropping devices.

`anx_blk_dev_register()` already logs `blk: registry full, dropping ...` when
it refuses a registration, and `anx_blk_dev_register_named()` MUST do the same.
A device that silently fails to register is how a missing drive becomes a
mystery.

---

## 3. Scanning

### 3.1 Per-device GPT reads

`anx_gpt_read()` currently reads through `anx_blk_read()`, the whole-system API,
so it can only ever read the active device. It MUST take an explicit device:

```c
int anx_gpt_read_dev(struct anx_blk_dev *dev, struct anx_gpt_table *table);
```

`anx_gpt_read()` becomes a wrapper passing `anx_blk_active()`, so existing
installer callers are unaffected.

`crc32()` MUST move from `kernel/core/install/gpt.c` to `kernel/lib/crc32.c`
with a declaration in `kernel/include/anx/crc32.h`. The partition scanner runs
long before the installer and MUST NOT pull in installer code to validate a
header.

### 3.2 Validation

A scan MUST reject a table rather than guess. In order:

1. The header at LBA 1 carries signature `"EFI PART"`.
2. The header's own CRC32 matches, computed over `header_size` bytes with the
   CRC field zeroed.
3. `entry_size` is at least 128 and `num_entries` is at most 128.
4. The CRC32 over the entry array matches the header's entry CRC.

If the primary header fails, the implementation SHOULD read the backup header
from the last sector and retry from it. A drive whose primary GPT was damaged is
exactly the case a backup exists for.

An entry is skipped when its type GUID is all zero, when `first_lba` is zero,
when `last_lba` is below `first_lba`, or when either bound lies outside the
parent's capacity.

### 3.3 When scanning happens

`anx_drivers_probe()` probes storage drivers, each registering its drives.
Partition scanning MUST happen after all storage drivers have probed and before
array assembly, giving this boot order:

1. Storage drivers probe; drives register.
2. **Partition scan; partitions register.**
3. Array assembly reads member superblocks (RFC-0030 §4).
4. Active device selection (§4 below).
5. Object store mounts.

Scanning before assembly is what lets an array take partitions as members.

An array device registered in step 3 MAY itself carry a GPT. Rescanning after
assembly is permitted but MUST NOT recurse: a partition MUST NOT be scanned for
partitions.

### 3.4 Cost

A scan reads 33 sectors per drive, roughly 17 KiB, plus a second read per drive
that falls back to its backup header. Against the NVMe fixes in RFC-0030 this is
noise, and it happens once per boot.

---

## 4. Finding the Anunix partition

### 4.1 Why first-registered-wins fails

"The first device registered becomes the active device" (RFC-0030 §2.3) was
correct when every registered device was a whole drive and Anunix owned the
machine. With partitions it selects `nvme0p1` — an ESP belonging to Fedora — and
the object store would format it.

### 4.2 Selection order

After partition scanning and array assembly, the kernel MUST choose the active
device by this order, stopping at the first match:

1. A device whose sector 0 holds a valid object store superblock — magic
   `ANX_DISK_MAGIC` (`"ANXD"`) and a version the kernel knows. If several match,
   prefer an array over a partition, and a partition over a whole drive.
2. When the kernel booted with an explicit target (`root=nvme0p2` on the command
   line), that device, whether or not it carries a superblock. An explicit target
   that does not resolve MUST be a boot failure, not a silent fallback.
3. When installing, no device is active until the installer selects one.

Rule 1 makes an installed system find itself. The superblock is the only
authority on which partition is Anunix's, and it is authoritative precisely
because nothing else writes `"ANXD"` to sector 0.

A device carrying `ANX_BLK_F_MEMBER` MUST NOT be selected, as RFC-0030 §2.4
already requires.

### 4.3 What is not a signal

Selection MUST NOT key on the GPT type GUID or the partition label alone. The
reserve partitions on jekyll carry the generic Linux filesystem GUID
`0fc63daf-8483-4772-8e79-3d69d8477de4` and the labels `ANUNIX_RAID0_A` and
`ANUNIX_RAID0_B`, because Linux tooling cut them. A label is a hint for the
installer's candidate list (§6.2), never a reason to write.

Future installs SHOULD set `ANX_GPT_TYPE_ANX_*` on partitions they create, which
makes the candidate list exact. Existing partitions MUST keep working without it.

---

## 5. Arrays over partitions

RFC-0030 assembles arrays from block devices. Partitions are block devices, so
the mechanism carries over unchanged — but two properties are worth stating,
because jekyll's layout depends on both.

**Members may be partitions.** `raid create 0 64 nvme0p2 nvme1p2` builds the
striped array the `ANUNIX_RAID0_A`/`_B` labels anticipate. The member superblock
lands at sector 0 of each partition, which is 1 MiB into each drive's reserve
region and nowhere near the host's data.

**`tail` metadata is meaningless on a partition.** RFC-0030 offers `tail`
metadata so UEFI firmware can read a mirrored ESP without understanding the
array. That trick requires array sector 0 to coincide with drive sector 0. Inside
a partition it cannot, so `raid create` MUST reject `tail` when any member is a
partition rather than appear to accept it.

Members of one array SHOULD be equal in size. jekyll's reserves are both 25 GiB.
RAID 0 across unequal members wastes the difference; RAID 1 clamps to the
smallest.

---

## 6. Installer

### 6.1 Two target modes

The installer currently partitions a drive. It MUST gain the ability to install
into a partition that already exists.

| Target | Behavior |
| --- | --- |
| Whole drive (`nvme0`) | Unchanged: write protective MBR, GPT, ESP, data partition |
| Partition (`nvme0p2`) | Write no partition table; format the object store in place |
| Array (`md0`) | Write no partition table; format the object store in place |

In partition mode the installer MUST NOT call `anx_gpt_create_default()` or
otherwise write to the parent device. The parent's table, its ESP, and every
other partition on it are not the installer's to touch.

### 6.2 Candidate list and confirmation

The interactive installer lists candidates. For partitions it SHOULD show parent,
size, GPT label, and type, and SHOULD sort partitions whose label begins
`ANUNIX` or whose type is `ANX_GPT_TYPE_ANX_*` to the top.

The confirmation prompt currently reads "This will erase ALL data on the disk".
In partition mode it MUST name the partition and its label, and MUST NOT claim
the disk is being erased — the existing wording would be a lie that costs someone
their machine.

### 6.3 Provisioning config

`install.target` selects the device by name. It composes with the existing
`install.raid` block (RFC-0030), which builds an array first and installs to it:

```json
{
  "install": {
    "raid": {
      "level": "raid0",
      "chunk_kib": 64,
      "metadata": "head",
      "members": ["nvme0p2", "nvme1p2"]
    },
    "target": "md0"
  }
}
```

Absent `install.target`, the installer MUST prompt rather than guess. Guessing a
target is how a provisioning file erases the wrong drive.

---

## 7. Safety requirements

These are the properties that make it safe to run Anunix on a machine holding
another operating system. An implementation that does not hold all of them is not
conformant.

1. A partition device MUST reject any request outside its bounds (§2.2).
2. A partition MUST NOT be given write access to its parent's sector 0.
3. The installer in partition mode MUST NOT write outside the target partition.
4. Active-device selection MUST NOT choose a device on the strength of a label or
   type GUID alone (§4.3).
5. `raid create` MUST refuse `tail` metadata on partition members (§5).
6. A device already claimed as an array member MUST NOT be an install target.
7. Bounds arithmetic MUST NOT overflow (§2.2).
8. Unregistering a device MUST unregister the partitions sitting on it
   (§2.5). A partition that outlives its parent holds a dangling parent
   pointer and keeps its name reserved against a later rescan.
9. The kernel MUST NOT format a block device it was not told to format
   (§8). This is the requirement the others exist to serve: every bound
   above is worthless if the boot path writes a fresh object store over
   the disk before anyone asks it to.

Requirements 1 through 3 are the ones standing between this feature and a
destroyed Fedora install. They warrant tests that assert the failure, not just
tests that assert the success.

---

## 8. Formatting safety

### 8.1 What happens today

`kernel/core/main.c` formats the active block device at boot whenever it does
not find an object store on it:

```c
if (anx_blk_ready()) {
        int ds_ret = anx_disk_store_init();

        if (ds_ret != ANX_OK) {
                /* First boot on this disk — format automatically */
                kprintf("disk: no store found, formatting...\n");
                ds_ret = anx_disk_format("anunix");
```

`anx_disk_format()` carries no guard of any kind. It zeroes sectors 0 through
`ANX_DATA_START` and writes its superblock, without reading a single sector
first. It does not look for a partition table, a filesystem, or an array
member. There is no prompt and no undo.

Combined with "the first device registered becomes the active device"
(RFC-0030 §2.3), this means: **booting Anunix on a machine where any drive
binds reformats that drive.**

This is not hypothetical. Booting the 2026.9.4 ISO under UEFI against a disk
partitioned like jekyll left sector 0 reading `44 58 4e 41` — `"DXNA"`, the
little-endian `ANX_DISK_MAGIC` — where the protective MBR had been. `sgdisk`
reported `invalid main GPT header, but valid backup`. Had that been jekyll's
`nvme0n1`, the Fedora installation would have been destroyed.

Until recently this had not cost a machine only because of an unrelated
defect: the anxboot EFI stub identity-maps 4 GiB, UEFI firmware assigns NVMe a
BAR above it (`0xc000000000` under OVMF), and the driver page-faulted during
probe. No block device registered, so nothing was formatted.

That accident is gone. `anx_mmio_map()` now maps a BAR before its driver
touches it, and NVMe binds under UEFI. The storage stack reaches real
hardware, and the guard in this section is the only thing standing between a
boot and a reformatted disk. It is not a precaution against a future problem;
it is the replacement for a bug that was doing the job by accident.

### 8.2 Requirements

1. The kernel MUST NOT format a block device as a side effect of booting.
   Absent a mountable object store, it runs without one and says so.
2. `anx_disk_format()` MUST probe the target (§8.3) and MUST refuse a device
   holding content it did not write.
3. A forced format MUST be a separate, explicitly named entry point. Forcing
   MUST originate in an operator action — an installer confirmation, or a
   provisioning config naming that exact device — never in a fallback path.
4. A device carrying `ANX_BLK_F_MEMBER` MUST NOT be formatted: it belongs to
   an array, and formatting it corrupts the array (RFC-0030 §2.4).
5. A refusal MUST name the device and what was found on it. "Refused" without
   a reason sends the operator looking for a hardware fault.
6. A probe that cannot read the device MUST be treated as foreign. An
   unreadable disk is not an empty one.

### 8.3 Probing

`anx_blk_probe()` classifies a device by reading a small number of sectors:

| Result | Meaning |
| --- | --- |
| `ANX_CONTENT_BLANK` | Nothing recognisable; safe to format |
| `ANX_CONTENT_ANUNIX` | An Anunix object store superblock |
| `ANX_CONTENT_FOREIGN` | Someone else's partition table, filesystem or array |

The signatures below are the minimum. They are chosen to cover what the target
machines actually carry — GPT, mdadm arrays, XFS, and an EFI system partition
— plus the formats most likely to be met on a drive moved between machines.

| Structure | Offset | Signature |
| --- | --- | --- |
| Anunix object store | 0x0 | `ANXD` (`ANX_DISK_MAGIC`) |
| GPT | LBA 1 | `EFI PART` |
| MBR / protective MBR | 0x1FE | `0x55 0xAA` |
| Linux RAID 1.x | 0x1000 | `0xA92B4EFC` |
| Anunix RAID | 0x0 | `ANXR` (`ANX_MD_MAGIC`) |
| LUKS | 0x0 | `LUKS\xBA\xBE` |
| XFS | 0x0 | `XFSB` |
| ext2/3/4 | 0x438 | `0x53 0xEF` |
| btrfs | 0x10040 | `_BHRfS_M` |
| FAT | 0x36 / 0x52 | `FAT` / `FAT32` |
| NTFS | 0x3 | `NTFS␣␣␣␣` |

A probe MUST read no more than is needed to reach the furthest signature, and
MUST tolerate a device too small to contain one rather than failing the whole
probe.

An all-zero device is `ANX_CONTENT_BLANK`. So is one whose every probed sector
reads as zero: a wiped drive is the case a fresh install starts from.

### 8.4 Boot behaviour

The boot path becomes:

1. Select the active device (§4).
2. `anx_disk_store_init()`. On success, mount and continue.
3. On failure, log which device was examined and what the probe found, then
   continue **without** an object store. Anunix runs from memory, exactly as
   it does when no drive binds at all.

Formatting moves entirely into the installer, where a human or a provisioning
config has named the target.

This costs the "boots straight into a working store on a blank disk" property
on first boot. That property is worth less than a machine, and the installer
still provides it for anyone who asks for it by name.

---

## 9. Non-goals

- **MBR partition tables.** UEFI machines, GPT only. A protective MBR is read to
  confirm it is protective, never to find partitions.
- **Writing partition tables to foreign disks.** The installer creates tables
  only in whole-drive mode on a drive the operator named.
- **Resizing, moving, or deleting partitions.** Anunix consumes a partition
  layout; it does not edit one. Use the host's tools.
- **Logical volume management.** RAID (RFC-0030) plus partitions covers the
  target machines.
- **Nested partitions.** A partition is never scanned for a partition table.

---

## 10. Testing

Host-native tests under `tests/` using the existing mock block device:

| Test | Asserts |
| --- | --- |
| `test_gpt_parse` | Valid table parses; entry count, bounds, labels correct |
| `test_gpt_reject_crc` | Corrupt header CRC is refused |
| `test_gpt_backup` | Damaged primary falls back to the backup header |
| `test_gpt_reject_bounds` | Entries outside parent capacity are skipped |
| `test_part_offset` | A read at partition LBA 0 hits parent `start_lba` |
| `test_part_clamp_read` | A read crossing the end returns `ANX_ERANGE` |
| `test_part_clamp_write` | A write crossing the end returns `ANX_ERANGE`, and the parent sector past the end is unmodified |
| `test_part_overflow` | `lba` near `UINT64_MAX` does not wrap into a valid range |
| `test_part_naming` | Entry 2 on `nvme0` registers as `nvme0p2` |
| `test_active_select` | The device with an `ANXD` superblock wins over one registered earlier |
| `test_active_skip_member` | An array member is never selected |
| `test_probe_blank` | An all-zero device probes BLANK |
| `test_probe_gpt` | A device carrying a GPT probes FOREIGN |
| `test_probe_mbr` | A bare MBR signature probes FOREIGN |
| `test_probe_anunix` | An Anunix superblock probes ANUNIX, not FOREIGN |
| `test_probe_filesystems` | XFS, ext4, btrfs and LUKS each probe FOREIGN |
| `test_format_refuses_foreign` | `anx_disk_format()` on a GPT disk returns an error **and leaves sector 0 byte-identical** |
| `test_format_refuses_member` | A device flagged `ANX_BLK_F_MEMBER` is refused |
| `test_format_accepts_blank` | A blank device formats and mounts |
| `test_format_forced` | The forced entry point overwrites a foreign disk |

`test_part_clamp_write` MUST verify the parent's contents, not merely the return
code. A clamp that returns an error after writing is the bug this is looking for.

QEMU validation extends `make qemu-raid`, which already boots three emulated
NVMe drives. A new `make qemu-part` SHOULD partition one emulated drive with a
foreign filesystem in partition 1 and an Anunix partition in partition 2, install
to partition 2, reboot, and assert that partition 1 is byte-identical.

---

## 11. Compatibility

An Anunix installed on a whole drive keeps working. Its superblock sits at drive
sector 0, no valid GPT is found, no partitions register, and selection rule 1
finds the superblock on the drive exactly as before.

The on-disk object store format does not change. The array superblock format does
not change. No migration is required in either direction, and an object store
written on a whole drive is byte-identical to one written in a partition.

`ANX_BLK_MAX_DEVS` changing from 16 to 32 is a recompile, not a format change.

---

## 12. Implementation order

1. Add `ANX_ERANGE` to `kernel/include/anx/types.h`.
2. Move `crc32()` to `kernel/lib/crc32.c`; add `kernel/include/anx/crc32.h`.
3. Add `anx_gpt_read_dev()`; reduce `anx_gpt_read()` to a wrapper.
4. Raise `ANX_BLK_MAX_DEVS` to 32; log refused registrations.
5. Add `anx_blk_dev_register_named()`.
6. Add `kernel/drivers/storage/part.c` and `kernel/include/anx/part.h`.
7. Call the scan from `anx_drivers_probe()` after storage, before assembly.
8. Replace first-registered-wins with superblock-directed selection.
9. Teach the installer partition targets and fix the confirmation wording.
10. Add `anx_blk_probe()`; guard `anx_disk_format()`; split out a forced
    entry point; remove the auto-format from `main.c` (§8).
11. Tests, then `make qemu-part`.

Steps 1 through 7 are additive and change no existing behavior: with no GPT on
any drive, nothing registers and the system behaves as it does today. Step 8 is
the first step that changes how an existing installation boots, and is the one to
land behind the most test coverage.

Step 10 is the one to land first. It is the only step that removes a way to
lose a disk, and every other step in this RFC brings the storage stack closer
to binding on real hardware — which is what makes the current auto-format
reachable there.
