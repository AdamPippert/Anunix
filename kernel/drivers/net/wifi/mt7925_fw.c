/*
 * mt7925_fw.c — MT7925 firmware download and MCU boot.
 *
 * Downloads the patch (ROM patch) and WM RAM firmware to the chip via
 * the WFDMA FWDL TX ring (ring 3).  The chip's ROM MCU processes each
 * DMA transfer and ACKs via the MCU event RX ring (ring 4).
 *
 * Sequence:
 *   1. Reset WFDMA and set up FWDL + MCU event rings
 *   2. Send PATCH_SEM_CONTROL(GET) MCU command → patch semaphore
 *   3. Stream patch firmware in 4 KiB chunks over ring 3
 *   4. Send PATCH_FINISH_REQ → MCU applies patch, restarts
 *   5. Stream WM RAM firmware in chunks
 *   6. Set WM start address, trigger MCU start
 *   7. Poll MT_TOP_MISC2 for firmware running state
 */

#include <anx/types.h>
#include <anx/pci.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include "mt7925_reg.h"
#include "mt7925_drv.h"

/* ------------------------------------------------------------------ */
/* Firmware blobs (embedded via mt7925_fw_blobs.S)                    */
/* ------------------------------------------------------------------ */

extern const uint8_t mt7925_patch_fw[];
extern const uint8_t mt7925_patch_fw_end[];
extern const uint32_t mt7925_patch_fw_size;

extern const uint8_t mt7925_ram_fw[];
extern const uint8_t mt7925_ram_fw_end[];
extern const uint32_t mt7925_ram_fw_size;

/* ------------------------------------------------------------------ */
/* DMA descriptor ring management                                      */
/* ------------------------------------------------------------------ */

#define FWDL_RING_SIZE          16    /* number of TX descriptors */
#define MCU_EVT_RING_SIZE       16    /* number of RX descriptors */
#define MCU_EVT_BUF_SIZE        2048  /* bytes per RX buffer */
#define FW_CHUNK_SIZE           4096  /* bytes per firmware chunk */

/* 16-byte DMA descriptor (MT792x format) */
struct mt7925_dma_desc {
	uint32_t buf;    /* DMA buffer address (lower 32 bits) */
	uint32_t ctrl;   /* length[15:0], flags[31:19] */
	uint32_t buf1;   /* reserved / upper address */
	uint32_t info;   /* token[28:16], type[5] */
} __attribute__((packed));

/* MCU TXD header prepended to all MCU commands */
struct mt7925_mcu_txd {
	uint8_t  reserved[4];
	uint16_t length;      /* TXD + payload length */
	uint16_t seq_num;
	uint8_t  cid;         /* command ID */
	uint8_t  pkt_type;    /* 0x20 = MCU command */
	uint8_t  set_query;   /* 0x01 = set, 0x00 = query */
	uint8_t  reserved2;
	uint32_t reserved3;
	uint8_t  s2d_index;   /* source/dest: 0 = AP, 1 = WA, 2 = WM */
	uint8_t  ext_cid;     /* extended command ID */
	uint8_t  ext_cid_ack; /* 1 = ext_cid present */
	uint8_t  reserved4;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                        */
/* ------------------------------------------------------------------ */

static inline uint32_t fw_rd(const struct mt7925_dev *dev, uint32_t reg)
{
	return *(volatile uint32_t *)((uint8_t *)dev->bar0 + reg);
}

static inline void fw_wr(const struct mt7925_dev *dev,
			  uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)((uint8_t *)dev->bar0 + reg) = val;
}

/* Spin-poll until (reg & mask) == val, or timeout (iterations). */
static int fw_poll(const struct mt7925_dev *dev, uint32_t reg,
		   uint32_t mask, uint32_t val, uint32_t iters)
{
	for (uint32_t i = 0; i < iters; i++) {
		if ((fw_rd(dev, reg) & mask) == val)
			return 0;
		/* ~1 us delay via CPUID-style fence */
		__asm__ volatile("pause" ::: "memory");
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* FWDL TX ring (ring 3)                                               */
/* ------------------------------------------------------------------ */

static struct mt7925_dma_desc *fwdl_ring;
static uint32_t fwdl_cidx;      /* CPU write index */

static int fwdl_ring_init(struct mt7925_dev *dev)
{
	/* Allocate descriptor ring (page-aligned for DMA) */
	uintptr_t pa = anx_page_alloc(0); /* 1 page = 4096 bytes */
	if (!pa)
		return -1;

	anx_memset((void *)pa, 0, ANX_PAGE_SIZE);
	fwdl_ring = (struct mt7925_dma_desc *)pa;
	fwdl_cidx = 0;

	/* Program ring into HW */
	fw_wr(dev, MT_WFDMA0_TX_RING_CNT(MT_MCU_FWDL_TXRING),
	      FWDL_RING_SIZE);
	fw_wr(dev, MT_WFDMA0_TX_RING_ADDR(MT_MCU_FWDL_TXRING),
	      (uint32_t)pa);
	fw_wr(dev, MT_WFDMA0_TX_RING_CIDX(MT_MCU_FWDL_TXRING), 0);

	return 0;
}

/* Push one DMA descriptor (pointing to buf of len bytes) to the FWDL ring. */
static void fwdl_push(struct mt7925_dev *dev,
		       const void *buf, uint32_t len, bool last)
{
	uint32_t idx = fwdl_cidx % FWDL_RING_SIZE;
	struct mt7925_dma_desc *desc = &fwdl_ring[idx];

	desc->buf  = (uint32_t)(uintptr_t)buf;
	desc->ctrl = (len & MT_DMA_CTRL_SD_LEN0_MASK)
		   | MT_DMA_CTRL_FIRST_SEC0
		   | (last ? MT_DMA_CTRL_LAST_SEC0 : 0);
	desc->buf1 = 0;
	desc->info = MT_DMA_INFO_PKT_TYPE_CMD;

	fwdl_cidx++;
	/* Kick HW by writing new CPU index */
	fw_wr(dev, MT_WFDMA0_TX_RING_CIDX(MT_MCU_FWDL_TXRING),
	      fwdl_cidx % FWDL_RING_SIZE);
}

/* ------------------------------------------------------------------ */
/* MCU event RX ring (ring 4) — exported so mt7925_mcu.c can poll    */
/* ------------------------------------------------------------------ */

struct mt7925_dma_desc *mcu_evt_ring;
uint8_t *mcu_evt_bufs;
uint32_t mcu_evt_cidx;
uint32_t mcu_evt_ring_count;

static int mcu_evt_ring_init(struct mt7925_dev *dev)
{
	uint32_t ring_pages = 1; /* descriptor ring */
	uint32_t buf_pages  = (MCU_EVT_RING_SIZE * MCU_EVT_BUF_SIZE +
			       ANX_PAGE_SIZE - 1) >> ANX_PAGE_SHIFT;

	uintptr_t ring_pa = anx_page_alloc(0);
	if (!ring_pa) return -1;

	/* Allocate pages for RX buffers: order = ceil(log2(buf_pages)) */
	uint32_t order = 0;
	while ((1U << order) < buf_pages) order++;
	uintptr_t buf_pa = anx_page_alloc(order);
	if (!buf_pa) return -1;

	anx_memset((void *)ring_pa, 0, ANX_PAGE_SIZE);
	anx_memset((void *)buf_pa,  0, buf_pages << ANX_PAGE_SHIFT);

	mcu_evt_ring       = (struct mt7925_dma_desc *)ring_pa;
	mcu_evt_bufs       = (uint8_t *)buf_pa;
	mcu_evt_cidx       = 0;
	mcu_evt_ring_count = MCU_EVT_RING_SIZE;

	/* Fill descriptors with buffer addresses */
	for (uint32_t i = 0; i < MCU_EVT_RING_SIZE; i++) {
		uint32_t buf_addr = (uint32_t)(buf_pa + i * MCU_EVT_BUF_SIZE);
		mcu_evt_ring[i].buf  = buf_addr;
		mcu_evt_ring[i].ctrl = MCU_EVT_BUF_SIZE | MT_DMA_CTRL_DMA_DONE;
		mcu_evt_ring[i].buf1 = 0;
		mcu_evt_ring[i].info = 0;
	}

	fw_wr(dev, MT_WFDMA0_RX_RING_CNT(MT_MCU_EVENT_RXRING),
	      MCU_EVT_RING_SIZE);
	fw_wr(dev, MT_WFDMA0_RX_RING_ADDR(MT_MCU_EVENT_RXRING),
	      (uint32_t)ring_pa);
	fw_wr(dev, MT_WFDMA0_RX_RING_CIDX(MT_MCU_EVENT_RXRING), 0);

	(void)ring_pages;
	return 0;
}

/* Poll MCU event ring for one event.  Returns event buf ptr (valid until
 * next call) or NULL if no event available. */
static const uint8_t *mcu_evt_poll(struct mt7925_dev *dev, uint32_t *out_len)
{
	uint32_t didx = fw_rd(dev,
			      MT_WFDMA0_RX_RING_DIDX(MT_MCU_EVENT_RXRING));
	uint32_t cidx = mcu_evt_cidx % MCU_EVT_RING_SIZE;

	if (cidx == didx)
		return NULL; /* ring empty */

	struct mt7925_dma_desc *desc = &mcu_evt_ring[cidx];
	uint32_t len = desc->ctrl & MT_DMA_CTRL_SD_LEN0_MASK;
	const uint8_t *buf = mcu_evt_bufs + cidx * MCU_EVT_BUF_SIZE;

	/* Recycle descriptor */
	desc->ctrl = MCU_EVT_BUF_SIZE | MT_DMA_CTRL_DMA_DONE;

	mcu_evt_cidx++;
	fw_wr(dev, MT_WFDMA0_RX_RING_CIDX(MT_MCU_EVENT_RXRING),
	      mcu_evt_cidx % MCU_EVT_RING_SIZE);

	if (out_len) *out_len = len;
	return buf;
}

/* Wait for an MCU event with matching eid.  Times out after ~3 sec. */
static int mcu_wait_evt(struct mt7925_dev *dev, uint8_t eid)
{
	for (uint32_t i = 0; i < 3000000; i++) {
		uint32_t len;
		const uint8_t *evt = mcu_evt_poll(dev, &len);
		if (evt && len >= sizeof(struct mt7925_mcu_txd)) {
			const struct mt7925_mcu_txd *txd =
				(const struct mt7925_mcu_txd *)evt;
			if (txd->ext_cid == eid || txd->cid == eid)
				return 0;
		}
		__asm__ volatile("pause" ::: "memory");
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* MCU command helpers                                                 */
/* ------------------------------------------------------------------ */

static uint16_t mcu_seq;

/* Send a small MCU command via the FWDL ring.
 * cmd_buf: TXD header + payload, total_len bytes. */
static void mcu_send_cmd(struct mt7925_dev *dev,
			  void *cmd_buf, uint16_t total_len,
			  uint8_t cid, uint8_t ext_cid, bool is_ext)
{
	struct mt7925_mcu_txd *txd = (struct mt7925_mcu_txd *)cmd_buf;
	anx_memset(txd, 0, sizeof(*txd));
	txd->length    = total_len;
	txd->seq_num   = ++mcu_seq;
	txd->pkt_type  = MT_MCU_PKT_TYPE_CMD;
	txd->set_query = MT_MCU_SET;
	txd->s2d_index = 0x00; /* host → WM */

	if (is_ext) {
		txd->cid         = 0xED; /* ext cmd marker */
		txd->ext_cid     = ext_cid;
		txd->ext_cid_ack = 1;
	} else {
		txd->cid = cid;
	}

	fwdl_push(dev, cmd_buf, total_len, true);
}

/* PATCH_SEM_CONTROL command */
static int cmd_patch_sem(struct mt7925_dev *dev, uint8_t sem)
{
	struct {
		struct mt7925_mcu_txd txd;
		uint32_t              sem_val;
	} __attribute__((packed)) cmd;

	anx_memset(&cmd, 0, sizeof(cmd));
	cmd.sem_val = sem;
	mcu_send_cmd(dev, &cmd, sizeof(cmd),
		     0, MCU_EXT_CMD_PATCH_SEM_CONTROL, true);

	return mcu_wait_evt(dev, MCU_EXT_CMD_PATCH_SEM_CONTROL);
}

/* PATCH_FINISH_REQ command */
static int cmd_patch_finish(struct mt7925_dev *dev)
{
	struct mt7925_mcu_txd cmd;
	mcu_send_cmd(dev, &cmd, sizeof(cmd),
		     0, MCU_EXT_CMD_PATCH_FINISH_REQ, true);
	return mcu_wait_evt(dev, MCU_EXT_CMD_PATCH_FINISH_REQ);
}

/* ------------------------------------------------------------------ */
/* Firmware stream helpers                                             */
/* ------------------------------------------------------------------ */

/* Stream firmware bytes via the FWDL ring in FW_CHUNK_SIZE chunks.
 * Each chunk uses a DMA descriptor pointing into the embedded blob
 * (which lives in .rodata, so physical == virtual in identity mapping). */
static int fw_stream(struct mt7925_dev *dev,
		     const uint8_t *fw, uint32_t size,
		     const char *name)
{
	uint32_t offset = 0;
	uint32_t chunks = 0;

	kprintf("mt7925: streaming %s (%u bytes)\n", name, size);

	while (offset < size) {
		uint32_t len = size - offset;
		if (len > FW_CHUNK_SIZE)
			len = FW_CHUNK_SIZE;

		bool last = (offset + len >= size);
		fwdl_push(dev, fw + offset, len, last);

		/* Wait for DMA ring to drain before reusing descriptor slot */
		if (fw_poll(dev,
			    MT_WFDMA0_TX_RING_DIDX(MT_MCU_FWDL_TXRING),
			    0xffff,
			    fwdl_cidx % FWDL_RING_SIZE,
			    1000000) != 0) {
			kprintf("mt7925: fw stream timeout at offset %u\n",
				offset);
			return -1;
		}

		offset += len;
		chunks++;
	}

	kprintf("mt7925: %s done (%u chunks)\n", name, chunks);
	return 0;
}

/* ------------------------------------------------------------------ */
/* WFDMA initialization                                                */
/* ------------------------------------------------------------------ */

static int wfdma_init(struct mt7925_dev *dev)
{
	uint32_t val;

	/* Disable TX/RX DMA */
	val = fw_rd(dev, MT_WFDMA0_GLO_CFG);
	val &= ~(MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);
	fw_wr(dev, MT_WFDMA0_GLO_CFG, val);

	/* Wait for DMA to go idle */
	if (fw_poll(dev, MT_WFDMA0_GLO_CFG,
		    MT_WFDMA0_GLO_CFG_TX_DMA_BUSY |
		    MT_WFDMA0_GLO_CFG_RX_DMA_BUSY,
		    0, 100000) != 0) {
		kprintf("mt7925: WFDMA idle timeout\n");
		return -1;
	}

	/* Reset DMA pointers */
	fw_wr(dev, MT_WFDMA0_RST_DTX_PTR, 0xFFFFFFFF);
	fw_wr(dev, MT_WFDMA0_RST_DRX_PTR, 0xFFFFFFFF);
	fw_wr(dev, MT_WFDMA0_RST_DTX_PTR, 0);
	fw_wr(dev, MT_WFDMA0_RST_DRX_PTR, 0);

	/* Init FWDL TX ring and MCU event RX ring */
	if (fwdl_ring_init(dev) != 0)   { kprintf("mt7925: fwdl ring alloc failed\n"); return -1; }
	if (mcu_evt_ring_init(dev) != 0) { kprintf("mt7925: mcu evt ring alloc failed\n"); return -1; }

	/* Enable DMA: set OMIT flags to skip TXD info fields in firmware path */
	fw_wr(dev, MT_WFDMA0_GLO_CFG,
	      MT_WFDMA0_GLO_CFG_TX_DMA_EN |
	      MT_WFDMA0_GLO_CFG_RX_DMA_EN |
	      MT_WFDMA0_GLO_CFG_OMIT_TX_INFO |
	      MT_WFDMA0_GLO_CFG_OMIT_RX_INFO);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Public: download firmware and boot MCU                             */
/* ------------------------------------------------------------------ */

int mt7925_fw_download(struct mt7925_dev *dev)
{
	int ret;

	kprintf("mt7925: patch size=%u RAM size=%u\n",
		mt7925_patch_fw_size, mt7925_ram_fw_size);

	/* Step 1: init WFDMA rings */
	ret = wfdma_init(dev);
	if (ret) return ret;

	/* Step 2: acquire patch semaphore */
	kprintf("mt7925: acquiring patch semaphore\n");
	ret = cmd_patch_sem(dev, MT_PATCH_SEM_GET);
	if (ret) {
		kprintf("mt7925: patch sem GET failed (MCU not responding)\n");
		return ret;
	}

	/* Step 3: stream patch firmware */
	ret = fw_stream(dev, mt7925_patch_fw, mt7925_patch_fw_size, "patch");
	if (ret) return ret;

	/* Step 4: finish patch → MCU applies and restarts WM */
	kprintf("mt7925: sending PATCH_FINISH_REQ\n");
	ret = cmd_patch_finish(dev);
	if (ret) {
		kprintf("mt7925: patch finish failed\n");
		return ret;
	}

	/* Short delay for MCU restart (~100ms equiv in spin loops) */
	for (volatile uint32_t i = 0; i < 5000000; i++)
		__asm__ volatile("pause" ::: "memory");

	/* Step 5: stream WM RAM firmware */
	ret = fw_stream(dev, mt7925_ram_fw, mt7925_ram_fw_size, "WM RAM");
	if (ret) return ret;

	/* Step 6: release patch semaphore */
	cmd_patch_sem(dev, MT_PATCH_SEM_RELEASE);

	/* Step 7: poll for firmware running */
	kprintf("mt7925: waiting for MCU firmware ready\n");
	ret = fw_poll(dev, MT_TOP_MISC2,
		      MT_TOP_MISC2_FW_STATE, MT_FW_STATE_RUNNING,
		      10000000);
	if (ret) {
		uint32_t state = fw_rd(dev, MT_TOP_MISC2) & MT_TOP_MISC2_FW_STATE;
		kprintf("mt7925: firmware not running (state=%u)\n", state);
		return -1;
	}

	kprintf("mt7925: firmware running\n");
	return 0;
}
