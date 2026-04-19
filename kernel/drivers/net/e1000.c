/*
 * e1000.c — Intel 8254x/8256x/825xx Gigabit Ethernet driver.
 *
 * Supports MMIO mode (BAR0).  Tested against QEMU's e1000 emulation
 * (-device e1000) and intended for real Intel Ethernet controllers
 * on x86_64 hardware.
 *
 * Architecture:
 *   RX: 64-descriptor ring, each pointing to a pre-allocated 2 KiB buffer.
 *       anx_e1000_poll() drains completed descriptors and delivers frames
 *       to the network stack.
 *
 *   TX: 64-descriptor ring.  anx_e1000_tx() places a frame descriptor
 *       and kicks the tail register.  Completion is detected by polling
 *       the DD (descriptor done) status bit.
 *
 * Interrupt-driven RX is supported but the driver also works in pure
 * poll mode for simplicity.  IRQ handler calls anx_e1000_poll().
 */

#include <anx/types.h>
#include <anx/e1000.h>
#include <anx/pci.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/irq.h>
#include <anx/net.h>
#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* PCI device table                                                    */
/* ------------------------------------------------------------------ */

#define E1000_VENDOR	0x8086

static const uint16_t e1000_devids[] = {
	0x100E,		/* 82540EM — QEMU default */
	0x100F,		/* 82545EM — VMware default */
	0x10D3,		/* 82574L  — ICH10, common desktop */
	0x10EA,		/* 82577LM — mobile Calpella */
	0x1533,		/* I210-AT — embedded / appliances */
	0x1539,		/* I211-AT — consumer motherboards */
	0x15B7,		/* I219-LM — Skylake/Kaby Lake mobile */
	0x15B8,		/* I219-V  — Skylake/Kaby Lake desktop */
	0x15D7,		/* I219-LM — Kabylake mobile */
	0x15D8,		/* I219-V  — Kabylake desktop */
};

#define E1000_DEVID_COUNT (sizeof(e1000_devids) / sizeof(e1000_devids[0]))

/* ------------------------------------------------------------------ */
/* Register offsets (MMIO, BAR0)                                      */
/* ------------------------------------------------------------------ */

#define E1000_CTRL		0x0000	/* Device Control */
#define E1000_STATUS		0x0008	/* Device Status */
#define E1000_EECD		0x0010	/* EEPROM/Flash Control */
#define E1000_EERD		0x0014	/* EEPROM Read */
#define E1000_ICR		0x00C0	/* Interrupt Cause Read */
#define E1000_ITR		0x00C4	/* Interrupt Throttle Rate */
#define E1000_ICS		0x00C8	/* Interrupt Cause Set */
#define E1000_IMS		0x00D0	/* Interrupt Mask Set */
#define E1000_IMC		0x00D8	/* Interrupt Mask Clear */
#define E1000_RCTL		0x0100	/* Receive Control */
#define E1000_TCTL		0x0400	/* Transmit Control */
#define E1000_TIPG		0x0410	/* TX Inter-Packet Gap */
#define E1000_RDBAL		0x2800	/* RX Descriptor Base Lo */
#define E1000_RDBAH		0x2804	/* RX Descriptor Base Hi */
#define E1000_RDLEN		0x2808	/* RX Descriptor Ring Length (bytes) */
#define E1000_RDH		0x2810	/* RX Descriptor Head */
#define E1000_RDT		0x2818	/* RX Descriptor Tail */
#define E1000_TDBAL		0x3800	/* TX Descriptor Base Lo */
#define E1000_TDBAH		0x3804	/* TX Descriptor Base Hi */
#define E1000_TDLEN		0x3808	/* TX Descriptor Ring Length (bytes) */
#define E1000_TDH		0x3810	/* TX Descriptor Head */
#define E1000_TDT		0x3818	/* TX Descriptor Tail */
#define E1000_RAL		0x5400	/* Receive Address Low (MAC[3:0]) */
#define E1000_RAH		0x5404	/* Receive Address High (MAC[5:4] + AV) */
#define E1000_MTA		0x5200	/* Multicast Table Array (128 × 4 bytes) */

/* CTRL bits */
#define E1000_CTRL_RST		(1U << 26)	/* software reset */
#define E1000_CTRL_SLU		(1U << 6)	/* set link up */
#define E1000_CTRL_ASDE		(1U << 5)	/* auto-speed detect enable */
#define E1000_CTRL_FD		(1U << 0)	/* full-duplex */

/* RCTL bits */
#define E1000_RCTL_EN		(1U << 1)	/* enable RX */
#define E1000_RCTL_SBP		(1U << 2)	/* store bad packets */
#define E1000_RCTL_UPE		(1U << 3)	/* unicast promiscuous */
#define E1000_RCTL_MPE		(1U << 4)	/* multicast promiscuous */
#define E1000_RCTL_BAM		(1U << 15)	/* broadcast accept */
#define E1000_RCTL_BSIZE_2K	(0U << 16)	/* 2 KiB buffers (default) */
#define E1000_RCTL_SECRC	(1U << 26)	/* strip CRC */

/* TCTL bits */
#define E1000_TCTL_EN		(1U << 1)	/* enable TX */
#define E1000_TCTL_PSP		(1U << 3)	/* pad short packets */
#define E1000_TCTL_CT_SHIFT	4		/* collision threshold */
#define E1000_TCTL_COLD_SHIFT	12		/* collision distance */

/* TX descriptor CMD bits */
#define E1000_TXD_CMD_EOP	(1U << 0)	/* end of packet */
#define E1000_TXD_CMD_IFCS	(1U << 1)	/* insert FCS */
#define E1000_TXD_CMD_RS	(1U << 3)	/* report status */

/* Descriptor status bits */
#define E1000_RXD_STAT_DD	(1U << 0)	/* descriptor done */
#define E1000_RXD_STAT_EOP	(1U << 1)	/* end of packet */
#define E1000_TXD_STAT_DD	(1U << 0)	/* descriptor done */

/* EERD bits */
#define E1000_EERD_START	(1U << 0)
#define E1000_EERD_DONE		(1U << 4)

/* IMS interrupt sources */
#define E1000_IMS_RXT0		(1U << 7)	/* RX timer interrupt */
#define E1000_IMS_RXO		(1U << 6)	/* RX overrun */
#define E1000_IMS_RXDMT0	(1U << 4)	/* RX descriptor min threshold */
#define E1000_IMS_TXQE		(1U << 1)	/* TX queue empty */

/* ------------------------------------------------------------------ */
/* Descriptor structures                                               */
/* ------------------------------------------------------------------ */

#define E1000_RING_SIZE		64
#define E1000_BUF_SIZE		2048

struct e1000_rx_desc {
	uint64_t buf_addr;	/* physical address of buffer */
	uint16_t length;	/* bytes written by hardware */
	uint16_t checksum;
	uint8_t  status;
	uint8_t  errors;
	uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
	uint64_t buf_addr;	/* physical address of frame */
	uint16_t length;	/* frame length */
	uint8_t  cso;		/* checksum offset */
	uint8_t  cmd;		/* command bits */
	uint8_t  status;
	uint8_t  css;		/* checksum start */
	uint16_t special;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

static struct {
	uint16_t device_id;
	uint8_t  bus, slot, func;
	volatile uint8_t *mmio;		/* BAR0 virtual */
	uint8_t  mac[6];

	struct e1000_rx_desc *rx_ring;
	struct e1000_tx_desc *tx_ring;
	uint8_t *rx_bufs[E1000_RING_SIZE];

	uint32_t rx_tail;		/* next descriptor to check */
	uint32_t tx_head;		/* next descriptor to free */
	uint32_t tx_tail;		/* next descriptor to fill */

	bool ready;
} nic;

static bool e1000_found;

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint32_t e1000_read(uint32_t reg)
{
	return *(volatile uint32_t *)(nic.mmio + reg);
}

static inline void e1000_write(uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)(nic.mmio + reg) = val;
}

/* ------------------------------------------------------------------ */
/* EEPROM MAC address read                                             */
/* ------------------------------------------------------------------ */

static uint16_t eeprom_read(uint8_t addr)
{
	uint32_t val;
	uint32_t i;

	e1000_write(E1000_EERD, E1000_EERD_START | ((uint32_t)addr << 8));
	for (i = 0; i < 10000; i++) {
		val = e1000_read(E1000_EERD);
		if (val & E1000_EERD_DONE)
			return (uint16_t)(val >> 16);
	}
	return 0xFFFF;
}

/* ------------------------------------------------------------------ */
/* IRQ handler                                                         */
/* ------------------------------------------------------------------ */

static void e1000_irq(uint32_t irq, void *arg)
{
	uint32_t icr = e1000_read(E1000_ICR);	/* reading clears ICR */

	(void)irq; (void)arg;

	if (icr & (E1000_IMS_RXT0 | E1000_IMS_RXDMT0 | E1000_IMS_RXO))
		anx_e1000_poll();
}

/* ------------------------------------------------------------------ */
/* Receive ring setup                                                  */
/* ------------------------------------------------------------------ */

static int e1000_rx_init(void)
{
	uint64_t ring_phys;
	uint32_t i;

	nic.rx_ring = anx_alloc(
		sizeof(struct e1000_rx_desc) * E1000_RING_SIZE);
	if (!nic.rx_ring)
		return ANX_ENOMEM;

	anx_memset(nic.rx_ring, 0,
		   sizeof(struct e1000_rx_desc) * E1000_RING_SIZE);

	for (i = 0; i < E1000_RING_SIZE; i++) {
		nic.rx_bufs[i] = anx_alloc(E1000_BUF_SIZE);
		if (!nic.rx_bufs[i])
			return ANX_ENOMEM;
		nic.rx_ring[i].buf_addr =
			(uint64_t)(uintptr_t)nic.rx_bufs[i];
	}

	ring_phys = (uint64_t)(uintptr_t)nic.rx_ring;
	e1000_write(E1000_RDBAL, (uint32_t)ring_phys);
	e1000_write(E1000_RDBAH, (uint32_t)(ring_phys >> 32));
	e1000_write(E1000_RDLEN,
		    E1000_RING_SIZE * sizeof(struct e1000_rx_desc));
	e1000_write(E1000_RDH, 0);
	e1000_write(E1000_RDT, E1000_RING_SIZE - 1);

	nic.rx_tail = 0;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Transmit ring setup                                                 */
/* ------------------------------------------------------------------ */

static int e1000_tx_init(void)
{
	uint64_t ring_phys;

	nic.tx_ring = anx_alloc(
		sizeof(struct e1000_tx_desc) * E1000_RING_SIZE);
	if (!nic.tx_ring)
		return ANX_ENOMEM;

	anx_memset(nic.tx_ring, 0,
		   sizeof(struct e1000_tx_desc) * E1000_RING_SIZE);

	ring_phys = (uint64_t)(uintptr_t)nic.tx_ring;
	e1000_write(E1000_TDBAL, (uint32_t)ring_phys);
	e1000_write(E1000_TDBAH, (uint32_t)(ring_phys >> 32));
	e1000_write(E1000_TDLEN,
		    E1000_RING_SIZE * sizeof(struct e1000_tx_desc));
	e1000_write(E1000_TDH, 0);
	e1000_write(E1000_TDT, 0);

	nic.tx_head = 0;
	nic.tx_tail = 0;
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int anx_e1000_tx(const void *frame, uint16_t len)
{
	struct e1000_tx_desc *desc;
	uint32_t next_tail;

	if (!e1000_found || !nic.ready)
		return ANX_ENODEV;
	if (len > 1514)
		return ANX_EINVAL;

	next_tail = (nic.tx_tail + 1) % E1000_RING_SIZE;
	if (next_tail == nic.tx_head)
		return ANX_EBUSY;

	desc = &nic.tx_ring[nic.tx_tail];
	desc->buf_addr = (uint64_t)(uintptr_t)frame;
	desc->length   = len;
	desc->cmd      = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS |
	                 E1000_TXD_CMD_RS;
	desc->status   = 0;
	desc->cso      = 0;
	desc->css      = 0;

	nic.tx_tail = next_tail;
	e1000_write(E1000_TDT, nic.tx_tail);

	/* Reclaim completed descriptors */
	while (nic.tx_ring[nic.tx_head].status & E1000_TXD_STAT_DD) {
		nic.tx_ring[nic.tx_head].status = 0;
		nic.tx_head = (nic.tx_head + 1) % E1000_RING_SIZE;
		if (nic.tx_head == nic.tx_tail)
			break;
	}

	return ANX_OK;
}

void anx_e1000_poll(void)
{
	if (!e1000_found || !nic.ready)
		return;

	while (nic.rx_ring[nic.rx_tail].status & E1000_RXD_STAT_DD) {
		struct e1000_rx_desc *desc = &nic.rx_ring[nic.rx_tail];
		uint16_t len = desc->length;

		if ((desc->status & E1000_RXD_STAT_EOP) && len > 0) {
			/* Deliver frame to network stack */
			anx_eth_recv(nic.rx_bufs[nic.rx_tail], len);
		}

		/* Return descriptor to hardware */
		desc->status = 0;
		e1000_write(E1000_RDT, nic.rx_tail);
		nic.rx_tail = (nic.rx_tail + 1) % E1000_RING_SIZE;
	}
}

const uint8_t *anx_e1000_mac(void)
{
	return nic.mac;
}

bool anx_e1000_ready(void)
{
	return e1000_found && nic.ready;
}

void anx_e1000_info(void)
{
	uint32_t status;

	if (!e1000_found) {
		kprintf("e1000: no Intel NIC detected\n");
		return;
	}

	status = e1000_read(E1000_STATUS);
	kprintf("e1000: [%04x:%04x] %02x:%02x.%x\n",
		E1000_VENDOR, nic.device_id, nic.bus, nic.slot, nic.func);
	kprintf("  MAC    : %02x:%02x:%02x:%02x:%02x:%02x\n",
		nic.mac[0], nic.mac[1], nic.mac[2],
		nic.mac[3], nic.mac[4], nic.mac[5]);
	kprintf("  Link   : %s  Speed: %s  Duplex: %s\n",
		(status & (1U << 1)) ? "up" : "down",
		(status >> 6 & 3) == 3 ? "1000" :
		(status >> 6 & 3) == 1 ? "100"  : "10",
		(status & (1U << 0)) ? "full" : "half");
	kprintf("  RX tail: %u  TX h/t: %u/%u\n",
		nic.rx_tail, nic.tx_head, nic.tx_tail);
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */
/* ------------------------------------------------------------------ */

int anx_e1000_init(void)
{
	struct anx_pci_device *pci = NULL;
	uint16_t w0, w1, w2;
	uint32_t i;
	int ret;

	for (i = 0; i < E1000_DEVID_COUNT && !pci; i++)
		pci = anx_pci_find_device(E1000_VENDOR, e1000_devids[i]);

	if (!pci)
		return ANX_ENODEV;

	nic.device_id = pci->device_id;
	nic.bus       = pci->bus;
	nic.slot      = pci->slot;
	nic.func      = pci->func;

	/* Map BAR0 (MMIO, 128 KiB typical) */
	nic.mmio = (volatile uint8_t *)(uintptr_t)(pci->bar[0] & ~0xFULL);
	if ((uintptr_t)nic.mmio == 0) {
		kprintf("e1000: BAR0 not assigned\n");
		return ANX_EIO;
	}

	anx_pci_enable_bus_master(pci);

	/* Software reset */
	e1000_write(E1000_CTRL, E1000_CTRL_RST);
	for (i = 0; i < 1000; i++)
		__asm__ volatile("" ::: "memory");

	/* Set link up, auto-speed detect, full duplex */
	e1000_write(E1000_CTRL, E1000_CTRL_SLU | E1000_CTRL_ASDE);

	/* Clear multicast table */
	for (i = 0; i < 128; i++)
		e1000_write(E1000_MTA + i * 4, 0);

	/* Read MAC from EEPROM */
	w0 = eeprom_read(0);
	w1 = eeprom_read(1);
	w2 = eeprom_read(2);
	nic.mac[0] = (uint8_t)(w0);
	nic.mac[1] = (uint8_t)(w0 >> 8);
	nic.mac[2] = (uint8_t)(w1);
	nic.mac[3] = (uint8_t)(w1 >> 8);
	nic.mac[4] = (uint8_t)(w2);
	nic.mac[5] = (uint8_t)(w2 >> 8);

	/* Program MAC into receive address filter */
	e1000_write(E1000_RAL, (uint32_t)nic.mac[0] |
		    ((uint32_t)nic.mac[1] << 8) |
		    ((uint32_t)nic.mac[2] << 16) |
		    ((uint32_t)nic.mac[3] << 24));
	e1000_write(E1000_RAH, (uint32_t)nic.mac[4] |
		    ((uint32_t)nic.mac[5] << 8) |
		    (1U << 31));		/* AV = address valid */

	/* Transmit inter-packet gap (standard IPGT for 802.3) */
	e1000_write(E1000_TIPG, 0x0060200A);

	ret = e1000_rx_init();
	if (ret != ANX_OK) {
		kprintf("e1000: RX init failed (%d)\n", ret);
		return ret;
	}

	ret = e1000_tx_init();
	if (ret != ANX_OK) {
		kprintf("e1000: TX init failed (%d)\n", ret);
		return ret;
	}

	/* Enable RX: broadcast accept, strip CRC, 2 KiB buffers */
	e1000_write(E1000_RCTL,
		    E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC |
		    E1000_RCTL_BSIZE_2K);

	/* Enable TX: pad short packets, 0x10 collision threshold */
	e1000_write(E1000_TCTL,
		    E1000_TCTL_EN | E1000_TCTL_PSP |
		    (0x10U << E1000_TCTL_CT_SHIFT) |
		    (0x40U << E1000_TCTL_COLD_SHIFT));

	/* Enable interrupts: RX timer, overrun, min threshold */
	e1000_write(E1000_IMS,
		    E1000_IMS_RXT0 | E1000_IMS_RXO | E1000_IMS_RXDMT0);

	if (pci->irq_line > 0 && pci->irq_line < 16) {
		anx_irq_register((uint8_t)pci->irq_line, e1000_irq, NULL);
		anx_irq_unmask((uint8_t)pci->irq_line);
	}

	e1000_found = true;
	nic.ready   = true;

	kprintf("e1000: %02x:%02x:%02x:%02x:%02x:%02x  irq %u  [%04x:%04x]\n",
		nic.mac[0], nic.mac[1], nic.mac[2],
		nic.mac[3], nic.mac[4], nic.mac[5],
		pci->irq_line, E1000_VENDOR, pci->device_id);

	return ANX_OK;
}
