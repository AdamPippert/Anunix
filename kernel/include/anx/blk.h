/*
 * anx/blk.h — Block device abstraction layer.
 *
 * Holds a registry of block devices. Every storage driver registers one
 * device per drive it finds (per NVMe controller, per AHCI port, per
 * virtio-blk function), so the software RAID layer in kernel/core/md/
 * can stripe or mirror across several of them.
 *
 * One device at a time is the "active" device. The legacy whole-system
 * API — anx_blk_read/write/capacity/ready — dispatches to it. The first
 * device registered becomes active; assembling a RAID array over that
 * device moves the active pointer to the array.
 */

#ifndef ANX_BLK_H
#define ANX_BLK_H

#include <anx/types.h>

#define ANX_BLK_MAX_DEVS	16
#define ANX_BLK_NAME_MAX	16

/* Device flags */
#define ANX_BLK_F_ARRAY		(1u << 0)  /* synthetic device backed by md */
#define ANX_BLK_F_MEMBER	(1u << 1)  /* claimed as a RAID array member */

struct anx_blk_dev;

struct anx_blk_ops {
	int      (*read)(struct anx_blk_dev *dev, uint64_t lba,
			 uint32_t count, void *buf);
	int      (*write)(struct anx_blk_dev *dev, uint64_t lba,
			  uint32_t count, const void *buf);
	uint64_t (*capacity)(struct anx_blk_dev *dev);
	const char *name;	/* driver name, e.g. "nvme" */
};

struct anx_blk_dev {
	const struct anx_blk_ops *ops;
	void       *priv;			/* driver instance state */
	char        name[ANX_BLK_NAME_MAX];	/* unique, e.g. "nvme1" */
	uint32_t    flags;
	bool        used;
};

/* Register one drive. base names the device class ("nvme"); the registry
 * appends the next free index. Returns NULL when the registry is full. */
struct anx_blk_dev *anx_blk_dev_register(const struct anx_blk_ops *ops,
					 void *priv, const char *base);

/* Release a registry slot. Clears the active pointer if it named dev. */
void anx_blk_dev_unregister(struct anx_blk_dev *dev);

/* Number of registry slots in use */
uint32_t anx_blk_dev_count(void);

/* Nth registered device, or NULL when index is out of range */
struct anx_blk_dev *anx_blk_dev_at(uint32_t index);

/* Look up a device by its unique name, or NULL */
struct anx_blk_dev *anx_blk_dev_find(const char *name);

/* Read sectors from one device */
int anx_blk_dev_read(struct anx_blk_dev *dev, uint64_t lba,
		     uint32_t count, void *buf);

/* Write sectors to one device */
int anx_blk_dev_write(struct anx_blk_dev *dev, uint64_t lba,
		      uint32_t count, const void *buf);

/* Capacity of one device in 512-byte sectors */
uint64_t anx_blk_dev_capacity(struct anx_blk_dev *dev);

/* Mark a device as a RAID member so it is skipped by active selection */
void anx_blk_dev_claim(struct anx_blk_dev *dev);

/* Drop the RAID member mark */
void anx_blk_dev_release(struct anx_blk_dev *dev);

/* True when the device is claimed by a RAID array */
bool anx_blk_dev_is_member(const struct anx_blk_dev *dev);

/* Make dev the device the whole-system API dispatches to */
void anx_blk_set_active(struct anx_blk_dev *dev);

/* The device the whole-system API dispatches to, or NULL */
struct anx_blk_dev *anx_blk_active(void);

/* Name of the active device, or "none" */
const char *anx_blk_active_name(void);

/* Read sectors from the active block device */
int anx_blk_read(uint64_t lba, uint32_t count, void *buf);

/* Write sectors to the active block device */
int anx_blk_write(uint64_t lba, uint32_t count, const void *buf);

/* Get total capacity in 512-byte sectors */
uint64_t anx_blk_capacity(void);

/* Check if a block device has been registered */
bool anx_blk_ready(void);

#endif /* ANX_BLK_H */
