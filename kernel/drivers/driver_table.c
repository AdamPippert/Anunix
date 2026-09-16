/*
 * driver_table.c — Central driver registration table and probe logic.
 *
 * Defines the full set of supported hardware and probes it at boot. Adding
 * a new driver is one line in the table; no changes to main.c are required.
 *
 * PCI match rules:
 *   - vendor/device != 0: match by vendor + device ID (ignores class fields)
 *   - vendor/device == 0: match by class + subclass + prog_if
 *
 * Platform drivers (ANX_BUS_PLATFORM) are only probed on arm64 via the
 * device tree. On x86_64 they are silently skipped.
 *
 * Storage drivers bind every drive they find, not just the first. Each
 * driver walks its whole bus on the first init() call and returns early
 * on later calls, so probing stays one pass over the hardware.
 */

#include <anx/types.h>
#include <anx/driver_table.h>
#include <anx/blk.h>
#include <anx/pci.h>
#include <anx/list.h>
#include <anx/kprintf.h>
#include <anx/dt.h>
#include <anx/virtio_blk.h>
#include <anx/nvme.h>
#include <anx/ahci.h>
#include <anx/apple_ans.h>
#include <anx/virtio_net.h>
#include <anx/e1000.h>
#include <anx/mt7925.h>
#include <anx/xhci.h>
#include <anx/bootlog.h>

/* --- Driver table --- */

static const struct anx_driver driver_table[] = {
	/* Storage: virtio-blk (legacy and modern) */
	ANX_DRIVER_PCI_DEV("virtio-blk",        ANX_DRVCLS_STORAGE, 0x1AF4, 0x1001, anx_virtio_blk_init),
	ANX_DRIVER_PCI_DEV("virtio-blk-modern", ANX_DRVCLS_STORAGE, 0x1AF4, 0x1042, anx_virtio_blk_init),

	/* Storage: NVMe (class 0x01, subclass 0x08, prog_if 0x02) */
	ANX_DRIVER_PCI_CLASS("nvme",  ANX_DRVCLS_STORAGE, 0x01, 0x08, 0x02, anx_nvme_init),

	/* Storage: AHCI (class 0x01, subclass 0x06, prog_if 0x01) */
	ANX_DRIVER_PCI_CLASS("ahci",  ANX_DRVCLS_STORAGE, 0x01, 0x06, 0x01, anx_ahci_init),

	/* Storage: Apple ANS2/ANS3 — platform device, not on PCIe config space */
	ANX_DRIVER_PLATFORM("apple-ans",  ANX_DRVCLS_STORAGE, "apple,ans2", anx_apple_ans_init),
	ANX_DRIVER_PLATFORM("apple-ans3", ANX_DRVCLS_STORAGE, "apple,ans3", anx_apple_ans_init),

	/* Network: virtio-net (legacy and modern) */
	ANX_DRIVER_PCI_DEV("virtio-net",        ANX_DRVCLS_NET, 0x1AF4, 0x1000, anx_virtio_net_init),
	ANX_DRIVER_PCI_DEV("virtio-net-modern", ANX_DRVCLS_NET, 0x1AF4, 0x1041, anx_virtio_net_init),

	/* Network: Intel e1000 (QEMU default: 8086:100E) */
	ANX_DRIVER_PCI_DEV("e1000",   ANX_DRVCLS_NET, 0x8086, 0x100E, anx_e1000_init),

	/* Network: MediaTek MT7925 Wi-Fi 7 (Framework Laptop 16) */
	ANX_DRIVER_PCI_DEV("mt7925",  ANX_DRVCLS_NET, 0x14C3, 0x0717, anx_mt7925_init),

	/* Input: USB through xHCI (class 0x0C, subclass 0x03, prog_if 0x30) */
	ANX_DRIVER_PCI_CLASS("xhci",  ANX_DRVCLS_INPUT, 0x0C, 0x03, 0x30, anx_xhci_init),
};

#define DRIVER_TABLE_SIZE \
	(sizeof(driver_table) / sizeof(driver_table[0]))

/* --- State --- */

static bool net_probe_ok;

/* Returns true if any NET class driver succeeded during the last probe. */
bool anx_net_probe_ok(void)
{
	return net_probe_ok;
}

/* --- PCI match helper --- */

static bool pci_matches(const struct anx_driver *drv,
			const struct anx_pci_device *dev)
{
	if (drv->pci_vendor != 0 || drv->pci_device != 0) {
		/* Vendor/device match */
		return drv->pci_vendor == dev->vendor_id &&
		       drv->pci_device == dev->device_id;
	}
	/* Class/subclass/prog_if match */
	return drv->pci_class    == dev->class_code &&
	       drv->pci_subclass == dev->subclass &&
	       drv->pci_prog_if  == dev->prog_if;
}

/* --- Probe --- */

void anx_drivers_probe(void)
{
	uint32_t i;
	bool storage_done = false;

	net_probe_ok = false;

	for (i = 0; i < DRIVER_TABLE_SIZE; i++) {
		const struct anx_driver *drv = &driver_table[i];
		int r;

		/*
		 * Storage entries come first in the table. Once past them,
		 * bring up the store and the boot-log ring before anything
		 * else runs.
		 */
		if (!storage_done && drv->drv_class != ANX_DRVCLS_STORAGE) {
			storage_done = true;
			anx_drivers_storage_done();
		}

		if (drv->bus == ANX_BUS_PCI) {
			struct anx_list_head *list = anx_pci_device_list();
			struct anx_list_head *pos;

			ANX_LIST_FOR_EACH(pos, list) {
				struct anx_pci_device *dev =
					ANX_LIST_ENTRY(pos, struct anx_pci_device, link);

				if (!pci_matches(drv, dev))
					continue;

				/* Storage drivers enumerate their whole bus on
				 * the first call and register one block device
				 * per drive, so every controller is bound and
				 * the RAID layer can see them all. */

				/* Enable DMA before calling init (idempotent) */
				anx_pci_enable_bus_master(dev);

				/*
				 * Put the log on disk first, so a driver that
				 * faults or never returns still leaves a record
				 * ending at this line.
				 */
				if (storage_done) {
					kprintf("driver: probing %s\n", drv->name);
					(void)anx_bootlog_ring_flush();
				}

				r = drv->init();
				kprintf("driver: %s %s\n",
					drv->name, r == ANX_OK ? "ok" : "failed");

				if (r == ANX_OK &&
				    drv->drv_class == ANX_DRVCLS_NET)
					net_probe_ok = true;
			}
		} else if (drv->bus == ANX_BUS_PLATFORM) {
#ifdef __aarch64__
			if (!anx_dt_has_compatible(drv->compatible))
				continue;

			r = drv->init();
			kprintf("driver: %s %s\n",
				drv->name, r == ANX_OK ? "ok" : "failed");

			if (r == ANX_OK && drv->drv_class == ANX_DRVCLS_NET)
				net_probe_ok = true;
#endif
			/* On x86_64: platform drivers are silently skipped */
		}
	}
}
