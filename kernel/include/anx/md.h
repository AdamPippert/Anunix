/*
 * anx/md.h — Software RAID (multiple-device) layer.
 *
 * Combines several registered block devices into one array device that
 * the object store, the installer, and the shell use like any other
 * drive. Two levels are supported:
 *
 *   RAID 0 — striping. Array capacity is the sum of the members. Each
 *            chunk_sectors run of array LBAs lands on the next member,
 *            so sequential I/O spreads across every NVMe queue. Losing
 *            one member loses the array.
 *   RAID 1 — mirroring. Array capacity is that of the smallest member.
 *            Writes go to every healthy member, reads come from one, and
 *            the array keeps serving I/O while at least one member lives.
 *
 * An array is described by a 512-byte superblock written to every member.
 * anx_md_init() scans the block registry at boot, groups members by array
 * UUID, and starts every array it can. Members carry the same UUID and
 * differ only in member_index.
 *
 * Metadata placement is a property of the array:
 *   head — superblock at sector ANX_MD_SB_SECTOR_HEAD, data starts at
 *          ANX_MD_DATA_OFFSET. The default.
 *   tail — superblock ANX_MD_SB_TAIL_BACK sectors before the end of the
 *          member, data starts at sector 0. A RAID 1 member laid out this
 *          way is byte-identical to a plain drive at the start, so UEFI
 *          firmware can still read a mirrored EFI system partition.
 */

#ifndef ANX_MD_H
#define ANX_MD_H

#include <anx/types.h>
#include <anx/blk.h>

/* On-disk superblock magic: "ANXR" */
#define ANX_MD_MAGIC		0x414E5852u
#define ANX_MD_SB_VERSION	1

#define ANX_MD_MAX_MEMBERS	8
#define ANX_MD_MAX_ARRAYS	4

/* Head placement: superblock sector and first data sector */
#define ANX_MD_SB_SECTOR_HEAD	8
#define ANX_MD_DATA_OFFSET	2048	/* 1 MiB, keeps members 4K-aligned */

/* Tail placement: sectors back from the end of the member */
#define ANX_MD_SB_TAIL_BACK	8

/* Default stripe unit: 64 KiB */
#define ANX_MD_DEFAULT_CHUNK	128

/* Superblock flags */
#define ANX_MD_SB_TAIL		(1u << 0)	/* metadata at end of member */

/* RAID levels */
#define ANX_MD_LEVEL_RAID0	0
#define ANX_MD_LEVEL_RAID1	1

/* Member states */
#define ANX_MD_MEMBER_EMPTY	0
#define ANX_MD_MEMBER_ACTIVE	1	/* in sync, serves reads */
#define ANX_MD_MEMBER_FAULTY	2	/* failed, no I/O issued */
#define ANX_MD_MEMBER_SPARE	3	/* rebuilding, writes only */

/* Array states, reported by anx_md_state() */
#define ANX_MD_ARRAY_CLEAN	0	/* every member active */
#define ANX_MD_ARRAY_DEGRADED	1	/* RAID 1 running short a member */
#define ANX_MD_ARRAY_REBUILDING	2	/* a spare is catching up */
#define ANX_MD_ARRAY_FAILED	3	/* too few members to serve I/O */

/* On-disk array descriptor — one 512-byte sector per member */
struct anx_md_super {
	uint32_t magic;			/* ANX_MD_MAGIC */
	uint32_t version;		/* ANX_MD_SB_VERSION */
	uint32_t level;			/* ANX_MD_LEVEL_* */
	uint32_t chunk_sectors;		/* stripe unit; 0 for RAID 1 */
	uint8_t  uuid[16];		/* array identity, same on all members */
	char     name[16];		/* name at creation, for diagnostics */
	uint32_t member_count;		/* members in the full array */
	uint32_t member_index;		/* role of this member, 0-based */
	uint64_t data_offset;		/* first data sector on this member */
	uint64_t member_sectors;	/* usable data sectors per member */
	uint64_t array_sectors;		/* capacity of the whole array */
	uint64_t events;		/* bumped on every metadata update */
	uint32_t member_state;		/* ANX_MD_MEMBER_* */
	uint32_t sb_flags;		/* ANX_MD_SB_* */
	uint64_t resync_offset;		/* rebuild progress, array sectors */
	uint64_t created;		/* creation time, nanoseconds */
	uint32_t checksum;		/* FNV-1a over the sector, this field 0 */
	uint8_t  _pad[512 - 116];
} __attribute__((packed));

struct anx_md_member {
	struct anx_blk_dev *dev;	/* NULL when the slot is missing */
	uint64_t data_offset;
	uint32_t state;			/* ANX_MD_MEMBER_* */
	uint64_t events;
};

struct anx_md_array {
	bool     used;
	char     name[ANX_BLK_NAME_MAX];
	uint8_t  uuid[16];
	uint32_t level;
	uint32_t chunk_sectors;
	uint32_t member_count;
	uint64_t member_sectors;	/* usable data sectors per member */
	uint64_t array_sectors;
	uint64_t events;
	uint64_t created;
	bool     tail_meta;
	uint32_t read_cursor;		/* RAID 1 round-robin read cursor */
	uint64_t resync_offset;		/* array sectors already rebuilt */
	struct anx_md_member members[ANX_MD_MAX_MEMBERS];
	struct anx_blk_dev *bdev;	/* the array's own registry entry */
};

/* Scan every registered block device for array superblocks and start the
 * arrays that can run. Called once at boot, after the driver probe. */
void anx_md_init(void);

/* Build a new array over the given members and write superblocks to them.
 * Destroys whatever the members held. chunk_sectors is ignored for RAID 1;
 * pass 0 to take ANX_MD_DEFAULT_CHUNK for RAID 0. */
int anx_md_create(const char *name, uint32_t level, uint32_t chunk_sectors,
		  struct anx_blk_dev **members, uint32_t count,
		  bool tail_meta, struct anx_md_array **out);

/* Scan the block registry and start every array whose members are present.
 * Returns the number of arrays started. */
int anx_md_assemble(void);

/* Stop an array and release its members. Fails while the array is the
 * active block device. */
int anx_md_stop(struct anx_md_array *a);

/* Mark one member faulty. RAID 1 keeps running; RAID 0 fails the array. */
int anx_md_fail(struct anx_md_array *a, uint32_t member_index);

/* Add a replacement member to a degraded RAID 1 array as a spare.
 * The array stays degraded until anx_md_resync() completes. */
int anx_md_add(struct anx_md_array *a, struct anx_blk_dev *dev);

/* Copy up to max_sectors of rebuild work onto the spare.
 * Returns the number of sectors copied, or a negative error. */
int anx_md_resync_step(struct anx_md_array *a, uint32_t max_sectors);

/* Run the rebuild to completion. */
int anx_md_resync(struct anx_md_array *a);

/* Erase the array superblock from a device so it stops being a member. */
int anx_md_zero_super(struct anx_blk_dev *dev);

/* Number of started arrays */
uint32_t anx_md_count(void);

/* Nth started array, or NULL */
struct anx_md_array *anx_md_at(uint32_t index);

/* Look up a started array by name, or NULL */
struct anx_md_array *anx_md_find(const char *name);

/* Current array state — ANX_MD_ARRAY_* */
uint32_t anx_md_state(const struct anx_md_array *a);

/* Human-readable names for reporting */
const char *anx_md_state_name(uint32_t state);
const char *anx_md_level_name(uint32_t level);
const char *anx_md_member_state_name(uint32_t state);

/* Parse "raid0"/"0"/"raid1"/"1" into ANX_MD_LEVEL_*. Negative on error. */
int anx_md_parse_level(const char *s);

/* --- Internals shared between the md source files --------------------- */

/* Claim a free array slot, or NULL when the table is full. */
struct anx_md_array *anx_md_alloc_array(void);

/* Return an array slot to the table without touching its members. */
void anx_md_free_array(struct anx_md_array *a);

/* Register a filled-in array as a block device and claim its members. */
int anx_md_start_array(struct anx_md_array *a);

/* Read/write one member's data area, translating to its data_offset. */
int anx_md_member_read(struct anx_md_array *a, uint32_t idx, uint64_t lba,
		       uint32_t count, void *buf);
int anx_md_member_write(struct anx_md_array *a, uint32_t idx, uint64_t lba,
			uint32_t count, const void *buf);

/* Mark a member faulty after an I/O error, without touching its disk. */
void anx_md_member_fault(struct anx_md_array *a, uint32_t idx);

/* Write the superblock of one member from the in-memory array state. */
int anx_md_super_write(struct anx_md_array *a, uint32_t idx);

/* Write superblocks to every present member. */
int anx_md_super_sync(struct anx_md_array *a);

/* Level-specific I/O, implemented in md_raid0.c and md_raid1.c. */
int anx_md_raid0_read(struct anx_md_array *a, uint64_t lba,
		      uint32_t count, void *buf);
int anx_md_raid0_write(struct anx_md_array *a, uint64_t lba,
		       uint32_t count, const void *buf);
int anx_md_raid1_read(struct anx_md_array *a, uint64_t lba,
		      uint32_t count, void *buf);
int anx_md_raid1_write(struct anx_md_array *a, uint64_t lba,
		       uint32_t count, const void *buf);

#endif /* ANX_MD_H */
