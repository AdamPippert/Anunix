/*
 * anx/virtio_blk.h — Virtio block device driver.
 *
 * Provides sector-level read/write to a virtio-blk PCI device.
 * Used for persistent storage (object store, installer).
 *
 * The anx_blk_read/write/capacity/ready API is now defined in blk.h
 * and dispatched through the registered ops table. This header pulls
 * in blk.h so all existing callers need no include changes.
 */

#ifndef ANX_VIRTIO_BLK_H
#define ANX_VIRTIO_BLK_H

#include <anx/types.h>
#include <anx/blk.h>

/* Probe for a virtio-blk device and initialize it */
int anx_virtio_blk_init(void);

#endif /* ANX_VIRTIO_BLK_H */
