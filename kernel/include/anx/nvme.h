/*
 * anx/nvme.h — NVMe storage driver.
 *
 * Probes all NVMe controllers (PCI class 0x01, subclass 0x08, prog_if 0x02)
 * and registers namespace 1 of each as its own block device.
 */

#ifndef ANX_NVME_H
#define ANX_NVME_H

/* Probe all NVMe controllers and register one block device per controller */
int anx_nvme_init(void);

#endif /* ANX_NVME_H */
