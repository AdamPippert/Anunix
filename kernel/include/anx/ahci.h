/*
 * anx/ahci.h — AHCI (Serial ATA) storage driver.
 *
 * Probes PCI devices with class=0x01, subclass=0x06, prog_if=0x01 (AHCI 1.0).
 * Registers every port that has a drive attached as its own block device
 * via anx_blk_dev_register().
 */

#ifndef ANX_AHCI_H
#define ANX_AHCI_H

/* Probe every AHCI controller and register one block device per attached drive */
int anx_ahci_init(void);

#endif /* ANX_AHCI_H */
