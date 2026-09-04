# RFC-0030: Software RAID — Striped and Mirrored Block Devices

| Field      | Value                                                       |
|------------|-------------------------------------------------------------|
| RFC        | 0030                                                        |
| Title      | Software RAID — Striped and Mirrored Block Devices          |
| Author     | Adam Pippert                                                |
| Status     | Draft                                                       |
| Created    | 2026-09-03                                                  |
| Depends On | RFC-0002 (State Object Model), RFC-0014 (Hardware Platform) |
| Blocks     | —                                                           |

This RFC uses the RFC 2119 requirement keywords `MUST`, `MUST NOT`, `SHOULD`,
and `MAY`, as RFC 8174 amends them.

---

## Executive Summary

Anunix combines two or more block devices into one array device. RAID 0 stripes
across the members for throughput. RAID 1 mirrors across them for redundancy.
The object store (RFC-0002) mounts from the array device, so the operating
system runs on the array without knowing an array exists.

Three pieces make that work:

1. **A block device registry.** Each storage driver registers one device per
   drive. Before this RFC the first drive to bind won, and the rest of the
   hardware stayed invisible.
2. **An array superblock.** Each member carries a 512-byte descriptor. The
   kernel scans the registry at boot, groups members by array UUID, and starts
   every array whose members are present.
3. **A level dispatch layer.** The array device presents the standard block
   operations. Requests reach the stripe map for RAID 0, or the mirror map for
   RAID 1.

---

## 1. Motivation

NVMe throughput on one drive is bounded by one controller and one queue pair. A
workstation with two drives can double sequential bandwidth by striping, and can
survive a drive failure by mirroring. Neither was reachable, for two reasons.

The block layer held exactly one device. `anx_blk_register()` kept the first
driver that called it and discarded the rest. The driver probe in
`kernel/drivers/driver_table.c` stopped probing storage as soon as one device
registered.

The drivers themselves were singletons. `kernel/drivers/storage/nvme.c` kept one
static controller state and bound the first controller it found. AHCI kept one
port. Two NVMe drives in one machine yielded one usable drive.

## 2. Block device registry

### 2.1 Structure

`kernel/include/anx/blk.h` defines the registry.

```c
struct anx_blk_dev {
	const struct anx_blk_ops *ops;
	void       *priv;			/* driver instance state */
	char        name[ANX_BLK_NAME_MAX];	/* unique, e.g. "nvme1" */
	uint32_t    flags;
	bool        used;
};
```

Every operation in `struct anx_blk_ops` takes the device as its first argument,
so one driver serves many drives from one operations table. The driver keeps its
per-drive state in `priv`.

`ANX_BLK_MAX_DEVS` is 16. `ANX_BLK_NAME_MAX` is 16 bytes. Neither can change at
runtime.

### 2.2 Names

`anx_blk_dev_register()` takes a class name and appends the lowest free index:
`nvme0`, `nvme1`, `ahci0`, `vblk0`, `md0`. The name is the handle the shell, the
installer, and the array superblock use.

Names follow registration order, which follows PCI enumeration order. A name is
therefore stable for a fixed machine and unstable across a hardware change. An
array does not depend on names: assembly matches members by the UUID in their
superblocks.

### 2.3 The active device

One device at a time answers the whole-system API — `anx_blk_read()`,
`anx_blk_write()`, `anx_blk_capacity()`, and `anx_blk_ready()`. The first device
registered becomes the active device, which preserves single-drive behavior.

`anx_md_start_array()` moves the active pointer to the array when a member held
it. The object store then mounts from the array.

### 2.4 Flags

| Flag | Meaning |
| --- | --- |
| `ANX_BLK_F_ARRAY` | The device is an array, not a drive |
| `ANX_BLK_F_MEMBER` | An array claimed the device as a member |

A device carrying `ANX_BLK_F_MEMBER` MUST NOT join a second array, and MUST NOT
be written through the whole-system API.

## 3. Array superblock

### 3.1 Layout

`struct anx_md_super` in `kernel/include/anx/md.h` occupies one 512-byte sector.
The fields every member shares are the level, the chunk size, the UUID, the
member count, the per-member capacity, and the array capacity. The fields that
differ per member are `member_index`, `member_state`, and `events`.

Magic is `ANX_MD_MAGIC` (`0x414E5852`, "ANXR"). Version is `ANX_MD_SB_VERSION`,
currently 1. A reader MUST reject a superblock whose version it does not know.

The `checksum` field holds an FNV-1a 32-bit hash over the whole sector with the
checksum field set to zero. Array metadata never leaves the machine, so the
checksum guards against a torn or stale sector, not against an attacker.

### 3.2 Placement

An array uses one of two metadata placements, fixed at creation.

| Placement | Superblock sector | First data sector | Use |
| --- | --- | --- | --- |
| head | 8 | 2048 | The default |
| tail | capacity − 8 | 0 | A mirror the firmware must read |

Head placement leaves 1 MiB of slack before the data area, which keeps every
member aligned to 4 KiB.

Tail placement puts array sector 0 at member sector 0. A RAID 1 member is then
byte-identical to a plain drive at the start. UEFI firmware reads a mirrored EFI
system partition without understanding the array.

`anx_md_assemble()` probes both placements, so an array assembles without being
told which placement it used.

### 3.3 Event counter

`anx_md_super_sync()` increments `events` and writes the superblock to every
present member. A member whose superblock lags the newest one is stale.

**IF** a member's `events` value is below the highest value found,
**THEN** assembly drops that member.

**IF** every member's `events` value matches,
**THEN** assembly keeps every member.

RAID 0 has no redundancy, so a dropped member fails the whole array. RAID 1
starts degraded on the members that remain.

## 4. RAID 0

### 4.1 Stripe map

The array cuts its LBA space into chunks of `chunk_sectors`. Chunk N lives on
member `N mod member_count` at chunk offset `N div member_count`.

```
array LBA:   | chunk 0 | chunk 1 | chunk 2 | chunk 3 |
member 0:    | chunk 0 |         | chunk 2 |
member 1:              | chunk 1 |         | chunk 3 |
```

`chunk_sectors` MUST be a power of two. The default is `ANX_MD_DEFAULT_CHUNK`,
128 sectors (64 KiB). The value is fixed at creation and cannot change at
runtime.

Array capacity is `member_sectors × member_count`, where `member_sectors` is the
smallest member's usable capacity rounded down to a whole number of chunks.

### 4.2 Request splitting

`anx_md_raid0_read()` and `anx_md_raid0_write()` walk a request one chunk at a
time. A request that crosses a chunk boundary becomes one member request per
chunk. No member request exceeds `chunk_sectors`.

### 4.3 Failure behavior

A RAID 0 array has no redundancy. A failed member marks the array
`ANX_MD_ARRAY_FAILED`, and the request that found the failure returns the
driver's error code. There is no recovery. Restore the array's contents from a
backup.

## 5. RAID 1

### 5.1 Mirror map

Every member holds identical contents, so array LBA and member LBA are the same
number. Array capacity is the smallest member's usable capacity.

A read comes from one active member, chosen round-robin from `read_cursor`, so
two readers can occupy two drives. A write goes to every member that is not
faulty.

### 5.2 Failure behavior

**IF** a member read fails,
**THEN** the array marks that member faulty and reads the next active member.

**IF** every active member fails a read,
**THEN** the read returns the last driver error.

**IF** a member write fails,
**THEN** the array marks that member faulty.

**IF** no active member accepts a write,
**THEN** the write returns the last driver error.

One I/O error faults a member. The array does not retry the drive first.

### 5.3 Rebuild

`anx_md_add()` puts a replacement drive in a failed member's slot with state
`ANX_MD_MEMBER_SPARE`. A spare takes writes below `resync_offset` and serves no
reads.

`anx_md_resync()` copies the array onto the spare in 128-sector passes, reading
through the normal mirror path so a second failure falls over to another member.
The last pass promotes the spare to `ANX_MD_MEMBER_ACTIVE` and writes the
superblocks.

The rebuild runs to completion in the calling thread and prints progress every
64 MiB. `anx_md_resync_step()` exposes one pass for a caller that wants to
interleave other work.

**Limitation:** a write that lands between the read and the write of one rebuild
pass is not reflected on the spare. The kernel issues block I/O from one thread
and the drivers poll, so no such write occurs today. A future asynchronous block
path MUST add a write-intent bitmap over the region being rebuilt.

## 6. Array states

| State | Meaning |
| --- | --- |
| `ANX_MD_ARRAY_CLEAN` | Every member is active |
| `ANX_MD_ARRAY_DEGRADED` | RAID 1 is running short a member |
| `ANX_MD_ARRAY_REBUILDING` | A spare is catching up |
| `ANX_MD_ARRAY_FAILED` | Too few members remain to serve I/O |

`ANX_MD_MAX_ARRAYS` is 4. `ANX_MD_MAX_MEMBERS` is 8 per array. Neither can
change at runtime.

## 7. Boot sequence

`anx_md_init()` runs in `kernel/core/main.c` after the driver probe and before
the object store mounts.

1. Read the superblock from every registered device that no array claims.
2. Group the devices that carry a superblock by array UUID.
3. Drop each member whose event counter lags the group's highest value.
4. Start each array that has enough members for its level.
5. Claim the members and register the array as a block device.
6. Set the array active **IF** a member held the active device.

An array does not carry the boot loader. UEFI firmware loads `anxboot` and the
kernel from a plain EFI system partition, and the kernel assembles the array
afterwards. A RAID 1 array with tail metadata MAY hold the EFI system partition,
because each member reads as a plain drive at sector 0.

## 8. Interfaces

### 8.1 Shell

`raid` administers arrays from `ansh`.

| Command | Effect |
| --- | --- |
| `raid list` | Print one line per array |
| `raid devs` | Print every block device and its role |
| `raid detail <name>` | Print an array's members and geometry |
| `raid create <0\|1> <chunkKiB\|-> <dev>... [tail] [force]` | Build an array |
| `raid stop <name>` | Stop an array and release its members |
| `raid assemble` | Scan the registry for arrays to start |
| `raid fail <name> <index>` | Mark a member faulty |
| `raid add <name> <dev>` | Add a replacement member to a RAID 1 array |
| `raid resync <name>` | Rebuild onto the spare |
| `raid zero <dev>` | Erase a device's array superblock |

> **Warning:** `raid create` erases every listed device. There is no reversal.
> Back up the contents first.

`force` lets `raid create` take the device that holds the mounted object store.
The store does not survive. Run `store format` after the array starts.

### 8.2 Provisioning

The installer builds an array from the `install.raid` object in the provisioning
config, before it partitions the disk.

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

| Key | Default | Meaning |
| --- | --- | --- |
| `level` | none, required | `raid0`, `raid1`, `0`, or `1` |
| `chunk_kib` | 64 | Stripe unit in KiB, RAID 0 only |
| `metadata` | `head` | `head` or `tail` |
| `members` | none, required | 2 to 8 block device names |

**IF** `install.raid` is absent,
**THEN** the installer partitions the active device as before.

## 9. Tests

`tests/test_md_raid.c` runs the array code over the RAM-backed mock devices in
`tests/harness/mock_arch.c`. Run it with `make test`.

| Case | Checks |
| --- | --- |
| `test_raid0_stripe` | Chunks alternate between members; a request crossing a chunk boundary round-trips; a failed member fails the array |
| `test_raid1_mirror` | Both members hold identical bytes; a degraded array still reads and writes; a rebuild brings a replacement in sync |
| `test_raid1_tail_metadata` | Array sector 0 is member sector 0 |
| `test_md_assemble` | An array reassembles from superblocks alone; erasing a superblock removes the member |
| `test_md_rejects_bad_arrays` | Duplicate members, one-member arrays, non-power-of-two chunks, unknown levels, and a claimed member are all rejected |

Observed on 2026-09-03, `make test`: `=== Results: 60 passed, 0 failed ===`.

## 10. Hardware validation

`make qemu-raid` boots the kernel with three emulated NVMe drives of 256 MiB
each. Build an array from the shell, then reboot to watch it assemble.

Observed on 2026-09-03, QEMU 11.1.0, `qemu-system-x86_64` with three
`-device nvme` drives:

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
blk: md0 registered (md, 510 MiB)
md: md0: raid0, 2 members, 510 MiB, clean
blk: active device is md0
md: 1 array(s) assembled
disk: mounted 'anunix' (0 objects)
anx> cat posix:/raidtest.txt
hello-from-raid0
```

The RAID 1 lifecycle, observed on the same date and the same three drives:

```
anx> raid create 1 - nvme0 nvme1 force
md: md0: raid1, 2 members, 255 MiB, clean
anx> raid fail md0 0
md: md0: member 0 (nvme0) failed
md0     raid1   2 members  255 MiB  degraded
anx> cat posix:/mirror.txt
mirrored-payload
anx> raid add md0 nvme2
md: md0: nvme2 added as spare, run "raid resync md0"
anx> raid resync md0
md: md0: rebuilding 255 MiB
md: md0: rebuilt 64/255 MiB
md: md0: rebuilt 128/255 MiB
md: md0: rebuilt 192/255 MiB
md: md0: rebuild complete, member 0 in sync
md0     raid1   2 members  255 MiB  clean
  [0] nvme2    active
  [1] nvme1    active
anx> cat posix:/mirror.txt
mirrored-payload
```

## 11. Driver changes

Three drivers gained per-drive instances. Each walks its bus on the first
`init()` call and returns early afterwards, so the probe stays one pass over the
hardware.

| Driver | Instance | Limit |
| --- | --- | --- |
| `nvme.c` | One per controller, namespace 1 | 8 |
| `ahci.c` | One per port with a drive attached | 8 |
| `virtio_blk.c` | One per virtio-blk PCI function | 8 |

Building an array over two NVMe drives found three defects in `nvme.c`. Each
one is independent of RAID and affects any NVMe drive.

### 11.1 Queue pages overlapped

**Observed:** the driver placed the admin completion queue 2048 bytes into the
page holding the admin submission queue. It placed the I/O completion queue 4096
bytes into the page holding the I/O submission queue. The controller requires a
page-aligned base for each queue, and the I/O completion queue fell outside its
page entirely. `CSTS.RDY` never reached 1, and the probe reported
`nvme: timeout waiting for ready` for every controller.

**Fix:** each queue gets its own page.

**Observed after the fix,** same QEMU command line:

```
nvme: found controller 1b36:0010 at 00:04.0
nvme: 524288 sectors (256 MiB)
blk: nvme0 registered (nvme, 256 MiB)
```

### 11.2 An unclaimed completion stalled the queue forever

**Observed:** `nvme_poll()` skipped a completion queue entry whose command ID
did not match, without consuming the entry. One timed-out command left its
completion at the queue head. Every later command on that queue then spun its
whole budget against an entry nobody would claim. Debug output from a RAID 1
rebuild, 2026-09-03:

```
nvme: DBG poll timeout cid=10950 cq_head=0 phase=0 cq0_status=0 cq_cid=10949 sq_tail=2
```

**Expected:** one timeout affects one command.

**Fix:** the driver keeps one command outstanding, so the entry at the queue
head belongs to the command being polled. `nvme_poll()` consumes the entry
whichever command ID it carries, and returns `ANX_EIO` on a mismatch.

### 11.3 The completion poll budget was too short

**Observed:** the budget was 500000 loop iterations. Metadata I/O completed
inside it. A RAID 1 rebuild, which reads every sector of a 255 MiB member,
timed out and faulted the healthy member:

```
md: md0: rebuilding 255 MiB
md: md0: member 1 (nvme1) failed
raid: resync failed (-9)
```

**Fix:** `NVME_POLL_ITERS` is 20000000 iterations. An iteration reads memory
the controller writes by DMA, so the cost of a larger budget falls only on a
command that has already failed.

## 12. Limitations

- RAID levels other than 0 and 1 are not implemented. RAID 5 and RAID 6 need
  parity and a read-modify-write path.
- An array has no write-intent bitmap. A rebuild copies every sector, and an
  unclean shutdown of a degraded RAID 1 array needs a full rebuild.
- The rebuild blocks its caller.
- A member is a whole block device. Partition members are not supported,
  because the block layer exposes no partition devices.
- A member I/O error faults the member immediately. The array does not retry.
- Metadata placement cannot change after creation.

## 13. Decision Summary

| Decision | Rationale |
| --- | --- |
| One block device per drive, not per driver | RAID needs to address members individually |
| The first registered device stays active | A single-drive machine behaves as it did before |
| Members match by UUID, not by name | Device names follow PCI order and move when hardware moves |
| Two metadata placements | Head keeps alignment; tail keeps a mirrored member readable by firmware |
| FNV-1a checksum, not CRC-32 | The superblock never leaves the machine, and a third CRC-32 table in the kernel earns nothing |
| A power-of-two chunk | The stripe map stays a shift and a mask |
| The event counter drops stale members | A member left behind by a failed write must not serve old data |
| A synchronous rebuild | The block path is polled and single-threaded; a background rebuild needs a write-intent bitmap first |
