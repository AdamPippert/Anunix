/*
 * anx/nvme.h — NVMe storage driver.
 *
 * Probes all NVMe controllers (PCI class 0x01, subclass 0x08, prog_if 0x02)
 * and registers the first usable namespace as the block device.
 */

#ifndef ANX_NVME_H
#define ANX_NVME_H

/* Probe all NVMe controllers and register the first as the block device */
int anx_nvme_init(void);

#endif /* ANX_NVME_H */
