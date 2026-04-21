/*
 * pci.c — PCI bus enumeration via Type 1 configuration mechanism.
 *
 * Scans all 256 buses using I/O ports 0xCF8 (address) and 0xCFC
 * (data). UEFI firmware pre-assigns bus numbers; we just walk them.
 * Discovered devices are stored in a linked list for driver lookup.
 */

#include <anx/types.h>
#include <anx/pci.h>
#include <anx/io.h>
#include <anx/alloc.h>
#include <anx/kprintf.h>

#define PCI_CONFIG_ADDR		0xCF8
#define PCI_CONFIG_DATA		0xCFC

#define PCI_MAX_BUS		256
#define PCI_MAX_SLOT		32
#define PCI_MAX_FUNC		8

/* Config space register offsets */
#define PCI_VENDOR_ID		0x00
#define PCI_DEVICE_ID		0x02
#define PCI_COMMAND		0x04
#define PCI_CLASS_REVISION	0x08
#define PCI_HEADER_TYPE		0x0E
#define PCI_BAR0		0x10
#define PCI_INTERRUPT		0x3C

/* Command register bits */
#define PCI_CMD_BUS_MASTER	(1 << 2)

static struct anx_list_head pci_devices = ANX_LIST_HEAD_INIT(pci_devices);

uint32_t anx_pci_config_read(uint8_t bus, uint8_t slot,
			      uint8_t func, uint8_t offset)
{
	uint32_t addr;

	addr = (1u << 31)		/* enable bit */
	     | ((uint32_t)bus << 16)
	     | ((uint32_t)slot << 11)
	     | ((uint32_t)func << 8)
	     | (offset & 0xFC);

	anx_outl(addr, PCI_CONFIG_ADDR);
	return anx_inl(PCI_CONFIG_DATA);
}

void anx_pci_config_write(uint8_t bus, uint8_t slot,
			   uint8_t func, uint8_t offset, uint32_t val)
{
	uint32_t addr;

	addr = (1u << 31)
	     | ((uint32_t)bus << 16)
	     | ((uint32_t)slot << 11)
	     | ((uint32_t)func << 8)
	     | (offset & 0xFC);

	anx_outl(addr, PCI_CONFIG_ADDR);
	anx_outl(val, PCI_CONFIG_DATA);
}

static void pci_scan_func(uint8_t bus, uint8_t slot, uint8_t func)
{
	struct anx_pci_device *dev;
	uint32_t reg;
	uint16_t vendor, device;
	int i;

	reg = anx_pci_config_read(bus, slot, func, PCI_VENDOR_ID);
	vendor = reg & 0xFFFF;
	device = (reg >> 16) & 0xFFFF;

	if (vendor == 0xFFFF || vendor == 0x0000)
		return;

	dev = anx_zalloc(sizeof(*dev));
	if (!dev)
		return;

	dev->bus = bus;
	dev->slot = slot;
	dev->func = func;
	dev->vendor_id = vendor;
	dev->device_id = device;

	reg = anx_pci_config_read(bus, slot, func, PCI_CLASS_REVISION);
	dev->revision = reg & 0xFF;
	dev->prog_if = (reg >> 8) & 0xFF;
	dev->subclass = (reg >> 16) & 0xFF;
	dev->class_code = (reg >> 24) & 0xFF;

	for (i = 0; i < 6; i++)
		dev->bar[i] = anx_pci_config_read(bus, slot, func,
						   PCI_BAR0 + i * 4);

	reg = anx_pci_config_read(bus, slot, func, PCI_INTERRUPT);
	dev->irq_line = reg & 0xFF;
	dev->irq_pin = (reg >> 8) & 0xFF;

	anx_list_init(&dev->link);
	anx_list_add_tail(&dev->link, &pci_devices);
}

static void pci_scan_slot(uint8_t bus, uint8_t slot)
{
	uint32_t reg;
	uint8_t header_type;
	uint8_t func;

	reg = anx_pci_config_read(bus, slot, 0, PCI_VENDOR_ID);
	if ((reg & 0xFFFF) == 0xFFFF)
		return;

	/* Check if multifunction device */
	header_type = (anx_pci_config_read(bus, slot, 0, PCI_HEADER_TYPE)
		       >> 16) & 0xFF;

	if (header_type & 0x80) {
		/* Multifunction — scan all 8 functions */
		for (func = 0; func < PCI_MAX_FUNC; func++)
			pci_scan_func(bus, slot, func);
	} else {
		pci_scan_func(bus, slot, 0);
	}
}

int anx_pci_init(void)
{
	uint16_t bus;
	uint8_t slot;
	uint32_t count = 0;
	struct anx_list_head *pos;

	for (bus = 0; bus < PCI_MAX_BUS; bus++)
		for (slot = 0; slot < PCI_MAX_SLOT; slot++)
			pci_scan_slot((uint8_t)bus, slot);

	ANX_LIST_FOR_EACH(pos, &pci_devices)
		count++;

	kprintf("pci: %u devices found\n", count);
	return ANX_OK;
}

struct anx_pci_device *anx_pci_find_device(uint16_t vendor, uint16_t device)
{
	struct anx_list_head *pos;

	ANX_LIST_FOR_EACH(pos, &pci_devices) {
		struct anx_pci_device *dev;

		dev = ANX_LIST_ENTRY(pos, struct anx_pci_device, link);
		if (dev->vendor_id == vendor && dev->device_id == device)
			return dev;
	}
	return NULL;
}

struct anx_list_head *anx_pci_device_list(void)
{
	return &pci_devices;
}

const char *anx_pci_class_name(uint8_t class_code, uint8_t subclass)
{
	switch (class_code) {
	case 0x00:
		return "unclassified";
	case 0x01:
		switch (subclass) {
		case 0x00: return "SCSI storage";
		case 0x01: return "IDE controller";
		case 0x05: return "ATA controller";
		case 0x06: return "AHCI controller";
		case 0x08: return "NVMe controller";
		default:   return "storage controller";
		}
	case 0x02:
		switch (subclass) {
		case 0x00: return "Ethernet controller";
		case 0x80: return "network controller";
		default:   return "network controller";
		}
	case 0x03:
		switch (subclass) {
		case 0x00: return "VGA controller";
		case 0x02: return "3D controller";
		default:   return "display controller";
		}
	case 0x04: return "multimedia";
	case 0x05: return "memory controller";
	case 0x06:
		switch (subclass) {
		case 0x00: return "host bridge";
		case 0x01: return "ISA bridge";
		case 0x04: return "PCI bridge";
		case 0x80: return "system peripheral";
		default:   return "bridge";
		}
	case 0x07: return "communication controller";
	case 0x08: return "system peripheral";
	case 0x0C:
		switch (subclass) {
		case 0x03: return "USB controller";
		case 0x05: return "SMBus controller";
		default:   return "serial bus";
		}
	case 0x0D: return "wireless controller";
	default:   return "unknown";
	}
}

void anx_pci_enable_bus_master(struct anx_pci_device *dev)
{
	uint32_t cmd;

	cmd = anx_pci_config_read(dev->bus, dev->slot, dev->func,
				  PCI_COMMAND);
	cmd |= PCI_CMD_BUS_MASTER;
	anx_pci_config_write(dev->bus, dev->slot, dev->func,
			     PCI_COMMAND, cmd);
}
