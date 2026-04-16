/*
 * anx/virtio_blk.h — Virtio block device driver.
 *
 * Provides sector-level read/write to a virtio-blk PCI device.
 * Used for persistent storage (object store, installer).
 */

#ifndef ANX_VIRTIO_BLK_H
#define ANX_VIRTIO_BLK_H

#include <anx/types.h>

/* Probe for a virtio-blk device and initialize it */
int anx_virtio_blk_init(void);

/* Read sectors from the block device */
int anx_blk_read(uint64_t sector, uint32_t count, void *buf);

/* Write sectors to the block device */
int anx_blk_write(uint64_t sector, uint32_t count, const void *buf);

/* Get total capacity in 512-byte sectors */
uint64_t anx_blk_capacity(void);

/* Check if block device is initialized */
bool anx_blk_ready(void);

#endif /* ANX_VIRTIO_BLK_H */
