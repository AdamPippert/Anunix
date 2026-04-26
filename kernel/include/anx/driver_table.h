/*
 * anx/driver_table.h — Central driver registration table.
 *
 * Replaces the hard-coded driver init calls in main.c with a declarative
 * table. Adding a driver is one line here; no changes to main.c required.
 */

#ifndef ANX_DRIVER_TABLE_H
#define ANX_DRIVER_TABLE_H

#include <anx/types.h>

enum anx_driver_bus {
	ANX_BUS_PCI,		/* matched by class/subclass/prog_if OR vendor/device */
	ANX_BUS_PLATFORM,	/* matched by compatible string; MMIO addr from device tree */
};

enum anx_driver_class {
	ANX_DRVCLS_STORAGE,
	ANX_DRVCLS_NET,
	ANX_DRVCLS_ACCEL,
	ANX_DRVCLS_INPUT,
	ANX_DRVCLS_DISPLAY,
};

struct anx_driver {
	const char		*name;
	enum anx_driver_bus	 bus;
	enum anx_driver_class	 drv_class;

	/* PCI match — used when bus == ANX_BUS_PCI.
	 * Set vendor/device to 0 to match by class/sub/prog_if only.
	 * Set class_code to 0 to match by vendor/device only. */
	uint8_t  pci_class;
	uint8_t  pci_subclass;
	uint8_t  pci_prog_if;
	uint16_t pci_vendor;
	uint16_t pci_device;

	/* Platform match — used when bus == ANX_BUS_PLATFORM */
	const char *compatible;	/* e.g. "apple,ans2" */

	/* Called once per matching device found during probe.
	 * For platform drivers on x86_64 this is never called (no DT). */
	int (*init)(void);
};

/* Probe all registered drivers against discovered hardware.
 * On x86_64: scans PCI list for ANX_BUS_PCI drivers.
 * On arm64:  scans PCI list + calls platform drivers if arch supports DT.
 * Storage class: stops after first successful registration (anx_blk_ready()).
 * Net class: calls all matches. */
void anx_drivers_probe(void);

/* Returns true if any NET class driver succeeded during the last probe. */
bool anx_net_probe_ok(void);

/* Convenience macros for declaring driver table entries */
#define ANX_DRIVER_PCI_CLASS(_n, _dcls, _cc, _sub, _pi, _fn) \
	{ .name=(_n), .bus=ANX_BUS_PCI, .drv_class=(_dcls), \
	  .pci_class=(_cc), .pci_subclass=(_sub), .pci_prog_if=(_pi), \
	  .pci_vendor=0, .pci_device=0, .compatible=NULL, .init=(_fn) }

#define ANX_DRIVER_PCI_DEV(_n, _dcls, _vid, _did, _fn) \
	{ .name=(_n), .bus=ANX_BUS_PCI, .drv_class=(_dcls), \
	  .pci_class=0, .pci_subclass=0, .pci_prog_if=0, \
	  .pci_vendor=(_vid), .pci_device=(_did), .compatible=NULL, .init=(_fn) }

#define ANX_DRIVER_PLATFORM(_n, _dcls, _compat, _fn) \
	{ .name=(_n), .bus=ANX_BUS_PLATFORM, .drv_class=(_dcls), \
	  .pci_class=0, .pci_subclass=0, .pci_prog_if=0, \
	  .pci_vendor=0, .pci_device=0, .compatible=(_compat), .init=(_fn) }

#endif /* ANX_DRIVER_TABLE_H */
