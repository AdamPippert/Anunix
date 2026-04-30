/*
 * anx/ahci.h — AHCI (Serial ATA) storage driver.
 *
 * Probes PCI devices with class=0x01, subclass=0x06, prog_if=0x01 (AHCI 1.0).
 * Finds the first port with a device attached and registers it as the
 * system block device via anx_blk_register().
 */

#ifndef ANX_AHCI_H
#define ANX_AHCI_H

/* Probe for an AHCI controller and register the first attached drive */
int anx_ahci_init(void);

#endif /* ANX_AHCI_H */
