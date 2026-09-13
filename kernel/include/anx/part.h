/*
 * anx/part.h — Partition devices backed by GPT entries (RFC-0031).
 *
 * A partition is a block device that forwards reads and writes to its
 * parent with an LBA offset added and the length clamped to the partition.
 * Everything above the block layer — the object store, the RAID layer, the
 * installer — works unchanged, because a partition presents exactly the
 * interface a drive presents.
 *
 * The clamp is the safety boundary. It is what keeps Anunix inside its own
 * partition and off the host operating system's filesystems.
 */

#ifndef ANX_PART_H
#define ANX_PART_H

#include <anx/types.h>
#include <anx/blk.h>

#define ANX_PART_LABEL_MAX	36

struct anx_part {
	struct anx_blk_dev *parent;	/* drive or array this sits on */
	uint64_t            start_lba;	/* first sector, absolute on parent */
	uint64_t            sectors;	/* length; the clamp bound */
	uint32_t            index;	/* GPT entry index, 1-based */
	uint64_t            type_lo, type_hi;
	char                label[ANX_PART_LABEL_MAX];
};

/*
 * Scan dev for a GPT and register a block device per usable entry, named
 * "<parent>p<index>" — nvme0p2. Returns the number registered, or a
 * negative error. A device that already is a partition is never scanned:
 * partitions do not nest.
 */
int anx_part_scan(struct anx_blk_dev *dev);

/*
 * Scan every registered drive that is not itself a partition. Called from
 * the driver probe after storage drivers have bound and before RAID
 * assembly, so an array can take partitions as members.
 */
void anx_part_scan_all(void);

/*
 * Unregister every partition sitting on parent and release its state.
 * Called from anx_blk_dev_unregister(): a partition outliving its parent
 * would hold a dangling parent pointer and keep its name reserved.
 */
void anx_part_forget_children(struct anx_blk_dev *parent);

/* True when dev was registered by the partition layer. */
bool anx_part_is_partition(const struct anx_blk_dev *dev);

/* Partition state behind dev, or NULL when dev is not a partition. */
const struct anx_part *anx_part_of(const struct anx_blk_dev *dev);

#endif /* ANX_PART_H */
