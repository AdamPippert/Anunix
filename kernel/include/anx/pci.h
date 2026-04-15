/*
 * anx/pci.h — PCI bus enumeration and configuration space access.
 *
 * Provides Type 1 PCI config access (ports 0xCF8/0xCFC) and a
 * simple bus scan that discovers all devices on bus 0.
 */

#ifndef ANX_PCI_H
#define ANX_PCI_H

#include <anx/types.h>
#include <anx/list.h>

struct anx_pci_device {
	uint8_t bus;
	uint8_t slot;
	uint8_t func;
	uint16_t vendor_id;
	uint16_t device_id;
	uint8_t class_code;
	uint8_t subclass;
	uint8_t prog_if;
	uint8_t revision;
	uint8_t irq_line;
	uint8_t irq_pin;
	uint32_t bar[6];
	struct anx_list_head link;
};

/* Scan PCI bus 0 and build the device list */
int anx_pci_init(void);

/* Read a 32-bit value from PCI configuration space */
uint32_t anx_pci_config_read(uint8_t bus, uint8_t slot,
			      uint8_t func, uint8_t offset);

/* Write a 32-bit value to PCI configuration space */
void anx_pci_config_write(uint8_t bus, uint8_t slot,
			   uint8_t func, uint8_t offset, uint32_t val);

/* Find a device by vendor and device ID (first match) */
struct anx_pci_device *anx_pci_find_device(uint16_t vendor, uint16_t device);

/* Get the global list of discovered PCI devices */
struct anx_list_head *anx_pci_device_list(void);

/* Enable PCI bus mastering for a device (required for DMA) */
void anx_pci_enable_bus_master(struct anx_pci_device *dev);

#endif /* ANX_PCI_H */
