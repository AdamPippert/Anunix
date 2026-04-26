/*
 * anx/blk.h — Block device abstraction layer.
 *
 * Defines a minimal ops table that any block driver (virtio-blk, NVMe, AHCI)
 * can register against. The first successful driver wins via anx_blk_register.
 * Callers use the standard anx_blk_read/write/capacity/ready API unchanged.
 */

#ifndef ANX_BLK_H
#define ANX_BLK_H

#include <anx/types.h>

struct anx_blk_ops {
	int      (*read)(uint64_t lba, uint32_t count, void *buf);
	int      (*write)(uint64_t lba, uint32_t count, const void *buf);
	uint64_t (*capacity)(void);
	const char *name;
};

/* Register the active block device (first caller wins) */
void anx_blk_register(const struct anx_blk_ops *ops);

/* Read sectors from the active block device */
int anx_blk_read(uint64_t lba, uint32_t count, void *buf);

/* Write sectors to the active block device */
int anx_blk_write(uint64_t lba, uint32_t count, const void *buf);

/* Get total capacity in 512-byte sectors */
uint64_t anx_blk_capacity(void);

/* Check if a block device has been registered */
bool anx_blk_ready(void);

#endif /* ANX_BLK_H */
