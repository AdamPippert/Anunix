/*
 * nvme.c — Minimal polled NVMe 1.4 driver.
 *
 * Enumerates PCI devices with class=0x01, subclass=0x08, prog_if=0x02.
 * For each controller found, attempts to initialize admin queues, identify
 * the controller and namespace 1, create one I/O queue pair, and register
 * the device as the system block device via anx_blk_register().
 *
 * Polled only — no MSI/MSI-X, no interrupt handler.
 * Requires 512-byte LBA sectors (LBADS=9); rejects other sector sizes.
 */

#include <anx/types.h>
#include <anx/nvme.h>
#include <anx/blk.h>
#include <anx/pci.h>
#include <anx/page.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/list.h>
#include <anx/io.h>

/* PCI class identifiers for NVMe */
#define NVME_PCI_CLASS		0x01
#define NVME_PCI_SUBCLASS	0x08
#define NVME_PCI_PROGIF		0x02

/* NVMe MMIO register offsets from BAR0 */
#define NVME_REG_CAP		0x00	/* 8B controller capabilities */
#define NVME_REG_VS		0x08	/* 4B version */
#define NVME_REG_CC		0x14	/* 4B controller configuration */
#define NVME_REG_CSTS		0x1C	/* 4B controller status */
#define NVME_REG_AQA		0x24	/* 4B admin queue attributes */
#define NVME_REG_ASQ		0x28	/* 8B admin SQ base address */
#define NVME_REG_ACQ		0x30	/* 8B admin CQ base address */

/* CAP field extraction */
#define NVME_CAP_TO(cap)	(((cap) >> 24) & 0xFF)	  /* timeout (500ms units) */
#define NVME_CAP_DSTRD(cap)	(((cap) >> 32) & 0xF)	  /* doorbell stride */
#define NVME_CAP_MPSMIN(cap)	(((cap) >> 48) & 0xF)	  /* min page size (2^(12+MPSMIN)) */

/* CC register bits */
#define NVME_CC_EN		(1u << 0)
#define NVME_CC_CSS_NVM		(0u << 4)	/* I/O command set = NVM */
#define NVME_CC_MPS_4K		(0u << 7)	/* memory page size 4KB (2^(12+0)) */
#define NVME_CC_AMS_RR		(0u << 11)	/* arbitration = round-robin */
#define NVME_CC_IOSQES_64	(6u << 16)	/* SQ entry size = 2^6 = 64B */
#define NVME_CC_IOCQES_16	(4u << 20)	/* CQ entry size = 2^4 = 16B */

/* CSTS register bits */
#define NVME_CSTS_RDY		(1u << 0)
#define NVME_CSTS_CFS		(1u << 1)

/* Queue sizes */
#define NVME_AQ_SIZE		32	/* admin queue depth */
#define NVME_IOQ_SIZE		64	/* I/O queue depth */

/* NVMe admin command opcodes */
#define NVME_ADM_DELETE_SQ	0x00
#define NVME_ADM_CREATE_SQ	0x01
#define NVME_ADM_DELETE_CQ	0x04
#define NVME_ADM_CREATE_CQ	0x05
#define NVME_ADM_IDENTIFY	0x06

/* NVMe I/O command opcodes */
#define NVME_IO_FLUSH		0x00
#define NVME_IO_WRITE		0x01
#define NVME_IO_READ		0x02

/* Identify CNS values */
#define NVME_IDENTIFY_NSID	0x00	/* identify namespace */
#define NVME_IDENTIFY_CTRL	0x01	/* identify controller */

/* Submission queue entry (64 bytes) */
struct nvme_sqe {
	uint32_t cdw0;		/* opcode[7:0], FUSE[9:8], PSDT[15:14], CID[31:16] */
	uint32_t nsid;
	uint64_t cdw2_3;	/* reserved / metadata pointer */
	uint64_t mptr;
	uint64_t prp1;
	uint64_t prp2;
	uint32_t cdw10;
	uint32_t cdw11;
	uint32_t cdw12;
	uint32_t cdw13;
	uint32_t cdw14;
	uint32_t cdw15;
};

/* Completion queue entry (16 bytes) */
struct nvme_cqe {
	uint32_t result;
	uint32_t reserved;
	uint16_t sq_head;
	uint16_t sq_id;
	uint16_t cid;
	uint16_t status;	/* phase[0], SC[8:1], SCT[11:9] */
};

/* Per-queue state */
struct nvme_queue {
	struct nvme_sqe *sq;	/* submission queue (64B entries) */
	struct nvme_cqe *cq;	/* completion queue (16B entries) */
	uint32_t sq_tail;	/* next SQE slot to fill */
	uint32_t cq_head;	/* next CQE to consume */
	uint8_t  phase;		/* expected phase bit (alternates each wrap) */
	uint32_t depth;		/* number of entries */
	volatile uint32_t *sq_db;	/* SQ doorbell register */
	volatile uint32_t *cq_db;	/* CQ doorbell register */
};

/* Per-controller state — static singleton (first controller found) */
static struct {
	volatile uint8_t *bar;		/* MMIO base */
	uint32_t dstrd;			/* doorbell stride (encoded) */
	struct nvme_queue adm;		/* admin queue pair */
	struct nvme_queue ioq;		/* I/O queue pair (QID=1) */
	uint64_t ns_sectors;		/* NSZE from identify namespace */
	uint16_t next_cid;		/* rolling command ID */
	bool ready;
} nvme;

/* --- MMIO accessors -------------------------------------------------- */

static uint32_t nvme_read32(uint32_t off)
{
	volatile uint32_t *p = (volatile uint32_t *)(nvme.bar + off);
	return *p;
}

static void nvme_write32(uint32_t off, uint32_t val)
{
	volatile uint32_t *p = (volatile uint32_t *)(nvme.bar + off);
	*p = val;
}

static uint64_t nvme_read64(uint32_t off)
{
	volatile uint64_t *p = (volatile uint64_t *)(nvme.bar + off);
	return *p;
}

static void nvme_write64(uint32_t off, uint64_t val)
{
	volatile uint64_t *p = (volatile uint64_t *)(nvme.bar + off);
	*p = val;
}

/* Return doorbell address for queue qid, type t (0=SQ, 1=CQ) */
static volatile uint32_t *nvme_doorbell(uint32_t qid, uint32_t t)
{
	uint32_t stride = 4u << nvme.dstrd;
	uint32_t off = 0x1000 + (2 * qid + t) * stride;

	return (volatile uint32_t *)(nvme.bar + off);
}

/* --- Busy-wait helpers ----------------------------------------------- */

/*
 * Spin until CSTS.RDY == want or iterations exhausted.
 * Returns 0 on success, -1 on timeout.
 */
static int nvme_wait_rdy(uint32_t want, uint32_t iters)
{
	uint32_t i;

	for (i = 0; i < iters; i++) {
		uint32_t csts = nvme_read32(NVME_REG_CSTS);

		if (csts & NVME_CSTS_CFS)
			return -1;	/* fatal status */
		if (((csts & NVME_CSTS_RDY) ? 1u : 0u) == (want ? 1u : 0u))
			return 0;
		/* ~1ms busy delay — each iteration costs a handful of cycles */
		{
			volatile uint32_t spin;
			for (spin = 0; spin < 10000; spin++)
				;
		}
	}
	return -1;
}

/* --- Command submission / completion --------------------------------- */

/*
 * Submit an SQE to the queue and ring the doorbell.
 * Returns the CID assigned to this command.
 */
static uint16_t nvme_submit(struct nvme_queue *q, struct nvme_sqe *sqe)
{
	uint16_t cid = ++nvme.next_cid;

	/* Encode opcode (already in cdw0[7:0]) and CID */
	sqe->cdw0 = (sqe->cdw0 & 0xFFFFu) | ((uint32_t)cid << 16);

	anx_memcpy(&q->sq[q->sq_tail], sqe, sizeof(*sqe));
	q->sq_tail = (q->sq_tail + 1) % q->depth;
	*q->sq_db = q->sq_tail;
	return cid;
}

/*
 * Poll the CQ until a CQE with matching CID and expected phase appears.
 * Returns 0 on success (SC=0), negative on error.
 * Rings the CQ doorbell after consuming the entry.
 */
static int nvme_poll(struct nvme_queue *q, uint16_t cid)
{
	uint32_t iters = 500000;

	while (iters--) {
		volatile struct nvme_cqe *cqe = &q->cq[q->cq_head];
		uint16_t status = cqe->status;

		/* Phase bit is in bit 0 of the status word */
		if ((status & 1u) != q->phase)
			continue;	/* not yet valid */

		/* Only consume entries for our CID */
		if (cqe->cid != cid)
			continue;

		/* Advance CQ head and flip phase on wrap */
		q->cq_head = (q->cq_head + 1) % q->depth;
		if (q->cq_head == 0)
			q->phase ^= 1;

		/* Ring CQ doorbell */
		*q->cq_db = q->cq_head;

		/* Update SQ head from CQE (tells us how many are consumed) */
		/* (not strictly needed for single-outstanding polling) */

		/* Check status: bits [8:1] = SC, [11:9] = SCT */
		if ((status >> 1) & 0x7FF)
			return ANX_EIO;
		return ANX_OK;
	}
	return ANX_ETIMEDOUT;
}

/* Send a single command and wait for completion */
static int nvme_exec(struct nvme_queue *q, struct nvme_sqe *sqe)
{
	uint16_t cid = nvme_submit(q, sqe);

	return nvme_poll(q, cid);
}

/* --- Admin commands -------------------------------------------------- */

/* Identify: CNS=1 → controller, CNS=0 → namespace */
static int nvme_identify(uint32_t nsid, uint32_t cns, void *buf)
{
	struct nvme_sqe sqe;

	anx_memset(&sqe, 0, sizeof(sqe));
	sqe.cdw0  = NVME_ADM_IDENTIFY;
	sqe.nsid  = nsid;
	sqe.prp1  = (uint64_t)(uintptr_t)buf;
	sqe.cdw10 = cns;
	return nvme_exec(&nvme.adm, &sqe);
}

/* Create I/O Completion Queue (QID=1, size=NVME_IOQ_SIZE, contiguous) */
static int nvme_create_iocq(void)
{
	struct nvme_sqe sqe;

	anx_memset(&sqe, 0, sizeof(sqe));
	sqe.cdw0  = NVME_ADM_CREATE_CQ;
	sqe.prp1  = (uint64_t)(uintptr_t)nvme.ioq.cq;
	/* CDW10: QSIZE[31:16] (0-based) | QID[15:0] */
	sqe.cdw10 = ((uint32_t)(NVME_IOQ_SIZE - 1) << 16) | 1u;
	/* CDW11: IEN=0 (no interrupt), PC=1 (physically contiguous) */
	sqe.cdw11 = 1u;
	return nvme_exec(&nvme.adm, &sqe);
}

/* Create I/O Submission Queue (QID=1, CQID=1, size=NVME_IOQ_SIZE) */
static int nvme_create_iosq(void)
{
	struct nvme_sqe sqe;

	anx_memset(&sqe, 0, sizeof(sqe));
	sqe.cdw0  = NVME_ADM_CREATE_SQ;
	sqe.prp1  = (uint64_t)(uintptr_t)nvme.ioq.sq;
	/* CDW10: QSIZE[31:16] (0-based) | QID[15:0] */
	sqe.cdw10 = ((uint32_t)(NVME_IOQ_SIZE - 1) << 16) | 1u;
	/* CDW11: CQID[31:16] | QPRIO[2:1]=0 (urgent) | PC=1 */
	sqe.cdw11 = (1u << 16) | 1u;
	return nvme_exec(&nvme.adm, &sqe);
}

/* --- I/O commands ---------------------------------------------------- */

static int nvme_io(uint32_t opc, uint64_t lba, uint32_t count, void *buf)
{
	struct nvme_sqe sqe;

	anx_memset(&sqe, 0, sizeof(sqe));
	sqe.cdw0  = opc;
	sqe.nsid  = 1;
	sqe.prp1  = (uint64_t)(uintptr_t)buf;
	/* PRP2 only needed if transfer crosses a page boundary;
	 * for ≤4KB single-sector ops it is always 0. Multi-sector
	 * callers must ensure buf is page-aligned and ≤4KB per call.
	 * The disk_store layer issues 512B sector ops so this is fine. */
	sqe.prp2  = 0;
	sqe.cdw10 = (uint32_t)(lba & 0xFFFFFFFFu);
	sqe.cdw11 = (uint32_t)(lba >> 32);
	sqe.cdw12 = (count - 1) & 0xFFFF;	/* NLB field (0-based) */
	return nvme_exec(&nvme.ioq, &sqe);
}

/* --- Block ops callbacks --------------------------------------------- */

static int nvme_blk_read(uint64_t lba, uint32_t count, void *buf)
{
	uint32_t i;

	/* Issue one sector at a time to stay within the single-PRP limit */
	for (i = 0; i < count; i++) {
		int ret = nvme_io(NVME_IO_READ, lba + i, 1,
				  (uint8_t *)buf + (uint64_t)i * 512);
		if (ret != ANX_OK)
			return ret;
	}
	return ANX_OK;
}

static int nvme_blk_write(uint64_t lba, uint32_t count, const void *buf)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		int ret = nvme_io(NVME_IO_WRITE, lba + i, 1,
				  (uint8_t *)(uintptr_t)buf + (uint64_t)i * 512);
		if (ret != ANX_OK)
			return ret;
	}
	return ANX_OK;
}

static uint64_t nvme_blk_capacity(void)
{
	return nvme.ns_sectors;
}

static const struct anx_blk_ops nvme_ops = {
	.read     = nvme_blk_read,
	.write    = nvme_blk_write,
	.capacity = nvme_blk_capacity,
	.name     = "nvme",
};

/* --- Controller init ------------------------------------------------- */

static int nvme_init_ctrl(struct anx_pci_device *pci)
{
	uint64_t bar0;
	uint64_t cap;
	uint32_t to_iters;
	uint8_t *aq_mem;
	uint8_t *ioq_mem;
	uint8_t *id_buf;
	int ret;

	/* Decode BAR0 (64-bit) — clear type/prefetch bits [3:0] */
	bar0 = ((uint64_t)pci->bar[0] & ~0xFULL) |
	       ((uint64_t)pci->bar[1] << 32);
	nvme.bar = (volatile uint8_t *)(uintptr_t)bar0;

	anx_pci_enable_bus_master(pci);

	cap = nvme_read64(NVME_REG_CAP);
	nvme.dstrd = (uint32_t)NVME_CAP_DSTRD(cap);

	/* TO is in 500ms units; use 1000 * TO iterations of ~0.5ms */
	to_iters = (NVME_CAP_TO(cap) + 1) * 1000;
	if (to_iters < 2000)
		to_iters = 2000;	/* minimum 1s */

	/* 1. Disable controller */
	nvme_write32(NVME_REG_CC, 0);
	ret = nvme_wait_rdy(0, to_iters);
	if (ret != 0) {
		kprintf("nvme: timeout waiting for disable\n");
		return ANX_ETIMEDOUT;
	}

	/* 2. Allocate admin queue memory (two pages: SQ + CQ) */
	aq_mem = (uint8_t *)(uintptr_t)anx_page_alloc(0);
	if (!aq_mem)
		return ANX_ENOMEM;
	anx_memset(aq_mem, 0, 4096);

	nvme.adm.sq    = (struct nvme_sqe *)aq_mem;
	nvme.adm.cq    = (struct nvme_cqe *)(aq_mem + NVME_AQ_SIZE * sizeof(struct nvme_sqe));
	nvme.adm.depth = NVME_AQ_SIZE;
	nvme.adm.sq_tail = 0;
	nvme.adm.cq_head = 0;
	nvme.adm.phase   = 1;	/* initial expected phase = 1 */
	nvme.adm.sq_db   = nvme_doorbell(0, 0);
	nvme.adm.cq_db   = nvme_doorbell(0, 1);

	/* 3. Program AQA, ASQ, ACQ */
	/* AQA: ACQS[27:16] and ASQS[11:0], both 0-based */
	nvme_write32(NVME_REG_AQA,
		((uint32_t)(NVME_AQ_SIZE - 1) << 16) |
		(uint32_t)(NVME_AQ_SIZE - 1));
	nvme_write64(NVME_REG_ASQ, (uint64_t)(uintptr_t)nvme.adm.sq);
	nvme_write64(NVME_REG_ACQ, (uint64_t)(uintptr_t)nvme.adm.cq);

	/* 4. Enable controller */
	nvme_write32(NVME_REG_CC,
		NVME_CC_EN |
		NVME_CC_CSS_NVM |
		NVME_CC_MPS_4K |
		NVME_CC_AMS_RR |
		NVME_CC_IOSQES_64 |
		NVME_CC_IOCQES_16);

	ret = nvme_wait_rdy(1, to_iters);
	if (ret != 0) {
		kprintf("nvme: timeout waiting for ready\n");
		return ANX_ETIMEDOUT;
	}

	/* 5. Identify controller (sanity check) */
	id_buf = (uint8_t *)(uintptr_t)anx_page_alloc(0);
	if (!id_buf)
		return ANX_ENOMEM;
	anx_memset(id_buf, 0, 4096);

	ret = nvme_identify(0, NVME_IDENTIFY_CTRL, id_buf);
	if (ret != ANX_OK) {
		kprintf("nvme: identify controller failed\n");
		return ret;
	}

	/* 6. Identify namespace 1 */
	anx_memset(id_buf, 0, 4096);
	ret = nvme_identify(1, NVME_IDENTIFY_NSID, id_buf);
	if (ret != ANX_OK) {
		kprintf("nvme: identify namespace failed\n");
		return ret;
	}

	/*
	 * NSZE is at offset 0 (8 bytes).
	 * FLBAS is at offset 26 (1 byte) — bits [3:0] = index into LBAF[].
	 * LBAF[] starts at offset 128, each entry 4 bytes.
	 * LBADS (LBA data size, log2 bytes) is in LBAF bits [23:16].
	 */
	{
		uint64_t nsze;
		uint8_t flbas;
		uint8_t lbaf_idx;
		uint32_t lbaf;
		uint8_t lbads;

		anx_memcpy(&nsze, id_buf + 0, sizeof(nsze));
		flbas   = id_buf[26];
		lbaf_idx = flbas & 0x0F;
		anx_memcpy(&lbaf, id_buf + 128 + lbaf_idx * 4, sizeof(lbaf));
		lbads = (uint8_t)((lbaf >> 16) & 0xFF);

		if (lbads != 9) {
			kprintf("nvme: unsupported LBA size (LBADS=%u, need 9)\n",
				(uint32_t)lbads);
			return ANX_ENOTSUP;
		}
		nvme.ns_sectors = nsze;
		kprintf("nvme: %llu sectors (%llu MiB)\n",
			(unsigned long long)nsze,
			(unsigned long long)(nsze / 2048));
	}

	/* 7. Allocate I/O queue memory */
	ioq_mem = (uint8_t *)(uintptr_t)anx_page_alloc(0);
	if (!ioq_mem)
		return ANX_ENOMEM;
	anx_memset(ioq_mem, 0, 4096);

	nvme.ioq.sq    = (struct nvme_sqe *)ioq_mem;
	nvme.ioq.cq    = (struct nvme_cqe *)(ioq_mem + NVME_IOQ_SIZE * sizeof(struct nvme_sqe));
	nvme.ioq.depth = NVME_IOQ_SIZE;
	nvme.ioq.sq_tail = 0;
	nvme.ioq.cq_head = 0;
	nvme.ioq.phase   = 1;
	nvme.ioq.sq_db   = nvme_doorbell(1, 0);
	nvme.ioq.cq_db   = nvme_doorbell(1, 1);

	/* 8. Create I/O CQ and SQ */
	ret = nvme_create_iocq();
	if (ret != ANX_OK) {
		kprintf("nvme: create I/O CQ failed\n");
		return ret;
	}
	ret = nvme_create_iosq();
	if (ret != ANX_OK) {
		kprintf("nvme: create I/O SQ failed\n");
		return ret;
	}

	nvme.ready = true;
	return ANX_OK;
}

int anx_nvme_init(void)
{
	struct anx_list_head *devlist;
	struct anx_list_head *pos;

	devlist = anx_pci_device_list();
	ANX_LIST_FOR_EACH(pos, devlist) {
		struct anx_pci_device *pci =
			ANX_LIST_ENTRY(pos, struct anx_pci_device, link);

		if (pci->class_code != NVME_PCI_CLASS ||
		    pci->subclass   != NVME_PCI_SUBCLASS ||
		    pci->prog_if    != NVME_PCI_PROGIF)
			continue;

		kprintf("nvme: found controller %04x:%04x at %02x:%02x.%x\n",
			(uint32_t)pci->vendor_id, (uint32_t)pci->device_id,
			(uint32_t)pci->bus, (uint32_t)pci->slot,
			(uint32_t)pci->func);

		if (nvme_init_ctrl(pci) == ANX_OK) {
			anx_blk_register(&nvme_ops);
			return ANX_OK;
		}
	}
	return ANX_ENOENT;
}
