/*
 * ahci.c — Minimal polled AHCI driver.
 *
 * Supports PCI class=0x01, subclass=0x06, prog_if=0x01 (AHCI 1.0).
 * Locates the first port with SSTS.DET=3 and SSTS.IPM=1, issues ATA
 * IDENTIFY DEVICE to read capacity, then exposes the drive via
 * anx_blk_register().
 *
 * All DMA buffers are allocated via anx_page_alloc(). Polled only.
 * 512-byte sectors only. LBA48 for READ/WRITE DMA EXT.
 */

#include <anx/types.h>
#include <anx/ahci.h>
#include <anx/blk.h>
#include <anx/pci.h>
#include <anx/page.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/list.h>

/* PCI class identifiers for AHCI */
#define AHCI_PCI_CLASS		0x01
#define AHCI_PCI_SUBCLASS	0x06
#define AHCI_PCI_PROGIF		0x01

/* HBA generic registers (offset from BAR5) */
#define AHCI_HBA_CAP		0x00
#define AHCI_HBA_GHC		0x04
#define AHCI_HBA_PI		0x0C	/* ports implemented */

/* GHC bits */
#define AHCI_GHC_AE		(1u << 31)	/* AHCI enable */
#define AHCI_GHC_IE		(1u << 1)	/* interrupt enable — keep clear */

/* Port register offsets within a port block (0x100 + port*0x80) */
#define AHCI_PORT_CLB		0x00	/* command list base (low 32) */
#define AHCI_PORT_CLBU		0x04	/* command list base (high 32) */
#define AHCI_PORT_FB		0x08	/* FIS receive buffer (low 32) */
#define AHCI_PORT_FBU		0x0C	/* FIS receive buffer (high 32) */
#define AHCI_PORT_IS		0x10	/* interrupt status */
#define AHCI_PORT_IE		0x14	/* interrupt enable (keep 0) */
#define AHCI_PORT_CMD		0x18	/* command and status */
#define AHCI_PORT_TFD		0x20	/* task file data */
#define AHCI_PORT_SSTS		0x28	/* SATA status */
#define AHCI_PORT_SERR		0x30	/* SATA error */
#define AHCI_PORT_CI		0x38	/* command issue */

/* CMD bits */
#define AHCI_CMD_ST		(1u << 0)	/* start DMA engine */
#define AHCI_CMD_FRE		(1u << 4)	/* FIS receive enable */
#define AHCI_CMD_FR		(1u << 14)	/* FIS receive running */
#define AHCI_CMD_CR		(1u << 15)	/* command list running */

/* TFD bits */
#define AHCI_TFD_BSY		(1u << 7)
#define AHCI_TFD_DRQ		(1u << 3)
#define AHCI_TFD_ERR		(1u << 0)

/* SSTS fields */
#define AHCI_SSTS_DET_MASK	0x0F
#define AHCI_SSTS_DET_PRESENT	0x03
#define AHCI_SSTS_IPM_MASK	0xF00
#define AHCI_SSTS_IPM_ACTIVE	0x100

/* FIS types */
#define FIS_TYPE_H2D		0x27

/* ATA commands */
#define ATA_CMD_IDENTIFY	0xEC
#define ATA_CMD_READ_DMA_EXT	0x25
#define ATA_CMD_WRITE_DMA_EXT	0x35

/* Device register: LBA mode */
#define ATA_DEV_LBA		0x40

/*
 * Command list header (32 bytes).
 * The spec defines a 1KB-aligned array of up to 32 such entries.
 */
struct ahci_cmd_hdr {
	uint16_t flags;		/* CFL[4:0]=5, W[6], A[5], ATAPI[5] */
	uint16_t prdtl;		/* PRDT length (number of entries) */
	uint32_t prdbc;		/* bytes transferred (written by HBA) */
	uint32_t ctba;		/* command table base (low, 128B aligned) */
	uint32_t ctbau;		/* command table base (high) — zero on 32-bit */
	uint32_t reserved[4];
};

/* H2D register FIS (20 bytes) */
struct ahci_fis_h2d {
	uint8_t  fis_type;	/* 0x27 */
	uint8_t  flags;		/* C=bit7 (command reg update) */
	uint8_t  command;	/* ATA command byte */
	uint8_t  features;
	uint8_t  lba0;		/* LBA[7:0] */
	uint8_t  lba1;		/* LBA[15:8] */
	uint8_t  lba2;		/* LBA[23:16] */
	uint8_t  device;	/* 0x40 = LBA mode */
	uint8_t  lba3;		/* LBA[31:24] */
	uint8_t  lba4;		/* LBA[39:32] */
	uint8_t  lba5;		/* LBA[47:40] */
	uint8_t  featuresex;
	uint8_t  countl;	/* sector count low */
	uint8_t  counth;	/* sector count high */
	uint8_t  icc;
	uint8_t  control;
	uint8_t  reserved[4];
};

/* PRDT entry (16 bytes) */
struct ahci_prdt {
	uint32_t dba;		/* data buffer address (low) */
	uint32_t dbau;		/* data buffer address (high) — zero */
	uint32_t reserved;
	uint32_t dbc;		/* byte count - 1 (bit31 = interrupt on completion = 0) */
};

/*
 * Command table layout (must be 128B aligned):
 *   Bytes   0-63:   CFIS (H2D FIS, first 20B used)
 *   Bytes  64-127:  ACMD (ATAPI, unused)
 *   Bytes 128-159:  reserved
 *   Bytes 160+:     PRDT entries (16B each)
 */
struct ahci_cmd_table {
	uint8_t       cfis[64];
	uint8_t       acmd[16];
	uint8_t       reserved[48];
	struct ahci_prdt prdt[1];	/* one entry sufficient for ≤4MB */
};

/* Per-controller state — static singleton */
static struct {
	volatile uint8_t *bar;		/* BAR5 MMIO base */
	uint32_t port;			/* active port index */
	volatile uint8_t *preg;		/* port register base */
	struct ahci_cmd_hdr *clb;	/* command list (32 headers) */
	uint8_t *fb;			/* FIS receive buffer */
	struct ahci_cmd_table *ct;	/* command table for slot 0 */
	uint64_t capacity;		/* sectors from IDENTIFY */
	bool ready;
} ahci;

/* --- MMIO helpers ---------------------------------------------------- */

static uint32_t hba_read(uint32_t off)
{
	volatile uint32_t *p = (volatile uint32_t *)(ahci.bar + off);
	return *p;
}

static void hba_write(uint32_t off, uint32_t val)
{
	volatile uint32_t *p = (volatile uint32_t *)(ahci.bar + off);
	*p = val;
}

static uint32_t port_read(uint32_t off)
{
	volatile uint32_t *p = (volatile uint32_t *)(ahci.preg + off);
	return *p;
}

static void port_write(uint32_t off, uint32_t val)
{
	volatile uint32_t *p = (volatile uint32_t *)(ahci.preg + off);
	*p = val;
}

/* Busy spin for N iterations (~1 per inner loop body) */
static void ahci_spin(uint32_t n)
{
	volatile uint32_t i;
	for (i = 0; i < n; i++)
		;
}

/* Wait until (port_read(off) & mask) == want, or timeout */
static int ahci_port_wait(uint32_t off, uint32_t mask, uint32_t want,
			   uint32_t iters)
{
	uint32_t i;

	for (i = 0; i < iters; i++) {
		if ((port_read(off) & mask) == want)
			return ANX_OK;
		ahci_spin(10000);
	}
	return ANX_ETIMEDOUT;
}

/* --- Port start/stop ------------------------------------------------- */

static int ahci_port_stop(void)
{
	uint32_t cmd;
	int ret;

	cmd = port_read(AHCI_PORT_CMD);
	cmd &= ~(AHCI_CMD_ST | AHCI_CMD_FRE);
	port_write(AHCI_PORT_CMD, cmd);

	/* Wait for CR and FR to clear */
	ret = ahci_port_wait(AHCI_PORT_CMD,
			     AHCI_CMD_CR | AHCI_CMD_FR, 0, 2000);
	if (ret != ANX_OK)
		kprintf("ahci: port stop timeout\n");
	return ret;
}

static void ahci_port_start(void)
{
	uint32_t cmd;

	cmd = port_read(AHCI_PORT_CMD);
	cmd |= AHCI_CMD_FRE | AHCI_CMD_ST;
	port_write(AHCI_PORT_CMD, cmd);
}

/* --- Command issue --------------------------------------------------- */

/*
 * Fill slot 0 of the command list and issue it.
 * cmd — ATA command byte
 * lba — 48-bit LBA (ignored for IDENTIFY)
 * count — sector count (0 = 256 for IDENTIFY convention; use 0 for IDENTIFY)
 * buf — data buffer (physical = virtual)
 * write — true for writes
 */
static int ahci_issue(uint8_t cmd, uint64_t lba, uint16_t count,
		      void *buf, bool write)
{
	struct ahci_fis_h2d *fis;
	struct ahci_cmd_hdr *hdr;
	uint32_t byte_count;
	int ret;

	/* Wait until port is idle */
	ret = ahci_port_wait(AHCI_PORT_TFD,
			     AHCI_TFD_BSY | AHCI_TFD_DRQ, 0, 1000);
	if (ret != ANX_OK) {
		kprintf("ahci: port busy before command\n");
		return ret;
	}

	/* Clear any pending errors */
	port_write(AHCI_PORT_SERR, 0xFFFFFFFFu);
	port_write(AHCI_PORT_IS,   0xFFFFFFFFu);

	/* Build command table: zero first */
	anx_memset(ahci.ct, 0, sizeof(*ahci.ct));

	/* H2D FIS */
	fis = (struct ahci_fis_h2d *)ahci.ct->cfis;
	fis->fis_type = FIS_TYPE_H2D;
	fis->flags    = 0x80;		/* C=1: command register update */
	fis->command  = cmd;
	fis->features = 0;

	if (cmd == ATA_CMD_IDENTIFY) {
		/* No LBA, no count for IDENTIFY */
		fis->device = 0;
	} else {
		/* LBA48 addressing */
		fis->device = ATA_DEV_LBA;
		fis->lba0   = (uint8_t)(lba >> 0);
		fis->lba1   = (uint8_t)(lba >> 8);
		fis->lba2   = (uint8_t)(lba >> 16);
		fis->lba3   = (uint8_t)(lba >> 24);
		fis->lba4   = (uint8_t)(lba >> 32);
		fis->lba5   = (uint8_t)(lba >> 40);
		fis->countl = (uint8_t)(count >> 0);
		fis->counth = (uint8_t)(count >> 8);
	}

	/* PRDT: one entry */
	byte_count = (cmd == ATA_CMD_IDENTIFY) ? 512u : (uint32_t)count * 512u;
	ahci.ct->prdt[0].dba      = (uint32_t)(uintptr_t)buf;
	ahci.ct->prdt[0].dbau     = 0;
	ahci.ct->prdt[0].reserved = 0;
	ahci.ct->prdt[0].dbc      = byte_count - 1;	/* bit31=0: no IRQ */

	/* Command list header[0] */
	hdr = &ahci.clb[0];
	anx_memset(hdr, 0, sizeof(*hdr));
	/* CFL = 5 (H2D FIS is 5 DWORDs), W bit for writes */
	hdr->flags = (uint16_t)(5u | (write ? (1u << 6) : 0u));
	hdr->prdtl = 1;
	hdr->ctba  = (uint32_t)(uintptr_t)ahci.ct;
	hdr->ctbau = 0;

	/* Issue slot 0 */
	port_write(AHCI_PORT_CI, 1u);

	/* Poll until slot 0 clears */
	ret = ahci_port_wait(AHCI_PORT_CI, 1u, 0, 100000);
	if (ret != ANX_OK) {
		kprintf("ahci: command timeout (cmd=0x%02x)\n",
			(uint32_t)cmd);
		return ret;
	}

	/* Check error */
	if (port_read(AHCI_PORT_TFD) & AHCI_TFD_ERR) {
		kprintf("ahci: TFD error after cmd 0x%02x\n",
			(uint32_t)cmd);
		return ANX_EIO;
	}
	return ANX_OK;
}

/* --- Block ops callbacks --------------------------------------------- */

static int ahci_blk_read(uint64_t lba, uint32_t count, void *buf)
{
	uint32_t i;

	/* One sector at a time to keep within single-PRDT-entry limit */
	for (i = 0; i < count; i++) {
		int ret = ahci_issue(ATA_CMD_READ_DMA_EXT,
				     lba + i, 1,
				     (uint8_t *)buf + (uint64_t)i * 512,
				     false);
		if (ret != ANX_OK)
			return ret;
	}
	return ANX_OK;
}

static int ahci_blk_write(uint64_t lba, uint32_t count, const void *buf)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		int ret = ahci_issue(ATA_CMD_WRITE_DMA_EXT,
				     lba + i, 1,
				     (uint8_t *)(uintptr_t)buf + (uint64_t)i * 512,
				     true);
		if (ret != ANX_OK)
			return ret;
	}
	return ANX_OK;
}

static uint64_t ahci_blk_capacity(void)
{
	return ahci.capacity;
}

static const struct anx_blk_ops ahci_ops = {
	.read     = ahci_blk_read,
	.write    = ahci_blk_write,
	.capacity = ahci_blk_capacity,
	.name     = "ahci",
};

/* --- Controller init ------------------------------------------------- */

static int ahci_init_port(uint32_t port_idx)
{
	uint8_t *clb_page;
	uint8_t *fb_page;
	uint8_t *ct_page;
	uint8_t identify_buf[512];
	int ret;

	ahci.port = port_idx;
	ahci.preg = ahci.bar + 0x100 + port_idx * 0x80;

	/* a. Stop port */
	ret = ahci_port_stop();
	if (ret != ANX_OK)
		return ret;

	/* b. Allocate command list (32 × 32B = 1KB; one page is enough) */
	clb_page = (uint8_t *)(uintptr_t)anx_page_alloc(0);
	if (!clb_page)
		return ANX_ENOMEM;
	anx_memset(clb_page, 0, 4096);
	ahci.clb = (struct ahci_cmd_hdr *)clb_page;

	/* c. Allocate FIS receive buffer (256B, full page) */
	fb_page = (uint8_t *)(uintptr_t)anx_page_alloc(0);
	if (!fb_page)
		return ANX_ENOMEM;
	anx_memset(fb_page, 0, 4096);
	ahci.fb = fb_page;

	/* d. Allocate command table (128B aligned — page is fine) */
	ct_page = (uint8_t *)(uintptr_t)anx_page_alloc(0);
	if (!ct_page)
		return ANX_ENOMEM;
	anx_memset(ct_page, 0, 4096);
	ahci.ct = (struct ahci_cmd_table *)ct_page;

	/* Write CLB and FB to port registers */
	port_write(AHCI_PORT_CLB,  (uint32_t)(uintptr_t)ahci.clb);
	port_write(AHCI_PORT_CLBU, 0);
	port_write(AHCI_PORT_FB,   (uint32_t)(uintptr_t)ahci.fb);
	port_write(AHCI_PORT_FBU,  0);

	/* e. Clear SERR */
	port_write(AHCI_PORT_SERR, 0xFFFFFFFFu);

	/* f. Start port */
	ahci_port_start();

	/* Short settle wait */
	ahci_spin(50000);

	/* g. Send IDENTIFY to get capacity */
	anx_memset(identify_buf, 0, sizeof(identify_buf));
	ret = ahci_issue(ATA_CMD_IDENTIFY, 0, 0, identify_buf, false);
	if (ret != ANX_OK) {
		kprintf("ahci: IDENTIFY failed on port %u\n", port_idx);
		return ret;
	}

	/*
	 * IDENTIFY words 100-103 (bytes 200-207) = LBA48 addressable sectors.
	 * Each word is little-endian 16-bit; reconstruct 64-bit value.
	 */
	{
		const uint16_t *words = (const uint16_t *)(const void *)identify_buf;
		uint64_t sectors;

		sectors = (uint64_t)words[100] |
			  ((uint64_t)words[101] << 16) |
			  ((uint64_t)words[102] << 32) |
			  ((uint64_t)words[103] << 48);
		ahci.capacity = sectors;
		kprintf("ahci: port %u: %llu sectors (%llu MiB)\n",
			port_idx,
			(unsigned long long)sectors,
			(unsigned long long)(sectors / 2048));
	}

	ahci.ready = true;
	return ANX_OK;
}

int anx_ahci_init(void)
{
	struct anx_list_head *devlist;
	struct anx_list_head *pos;

	devlist = anx_pci_device_list();
	ANX_LIST_FOR_EACH(pos, devlist) {
		struct anx_pci_device *pci =
			ANX_LIST_ENTRY(pos, struct anx_pci_device, link);
		uint32_t bar5;
		uint32_t pi;
		uint32_t port_idx;

		if (pci->class_code != AHCI_PCI_CLASS ||
		    pci->subclass   != AHCI_PCI_SUBCLASS ||
		    pci->prog_if    != AHCI_PCI_PROGIF)
			continue;

		kprintf("ahci: found controller %04x:%04x at %02x:%02x.%x\n",
			(uint32_t)pci->vendor_id, (uint32_t)pci->device_id,
			(uint32_t)pci->bus, (uint32_t)pci->slot,
			(uint32_t)pci->func);

		/* BAR5 = AHCI HBA base (32-bit BAR) */
		bar5 = pci->bar[5] & ~0xFu;
		if (!bar5)
			continue;

		ahci.bar = (volatile uint8_t *)(uintptr_t)bar5;
		anx_pci_enable_bus_master(pci);

		/* Enable AHCI mode, disable interrupts */
		hba_write(AHCI_HBA_GHC,
			  AHCI_GHC_AE | (hba_read(AHCI_HBA_GHC) & ~AHCI_GHC_IE));

		pi = hba_read(AHCI_HBA_PI);

		for (port_idx = 0; port_idx < 32; port_idx++) {
			uint32_t ssts;

			if (!(pi & (1u << port_idx)))
				continue;

			/* Set port register base for the SSTS check */
			{
				volatile uint8_t *preg =
					ahci.bar + 0x100 + port_idx * 0x80;
				volatile uint32_t *p =
					(volatile uint32_t *)(preg +
						AHCI_PORT_SSTS);
				ssts = *p;
			}

			if ((ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_PRESENT)
				continue;
			if ((ssts & AHCI_SSTS_IPM_MASK) != AHCI_SSTS_IPM_ACTIVE)
				continue;

			if (ahci_init_port(port_idx) == ANX_OK) {
				anx_blk_register(&ahci_ops);
				return ANX_OK;
			}
		}
	}
	return ANX_ENOENT;
}
