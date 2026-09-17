/*
 * mt7925_fw.c — MT7925 DMA bring-up and firmware download.
 *
 * A port of the Linux v6.12 mt76 firmware path, not a reinvention of it.
 * The previous version of this file guessed at the protocol and got most of
 * it wrong: the command rings were at the wrong indices and the wrong
 * register spacing, commands were framed as extended commands with the wrong
 * packet type and no hardware descriptor, and the firmware images were sent
 * as one raw stream, headers and all, with no per-section download request.
 * The ROM could not have accepted any of it.
 *
 * Sources, all Linux v6.12 drivers/net/wireless/mediatek/mt76:
 *
 *   ring setup ........... mt7925/pci.c      mt7925_dma_init()
 *   DMA disable/enable ... mt792x_dma.c      mt792x_dma_disable(),
 *                                             mt792x_dma_enable(),
 *                                             mt792x_dma_prefetch()
 *   descriptors .......... dma.c             mt76_dma_add_buf(),
 *                                             mt76_dma_dequeue(),
 *                                             mt76_dma_rx_fill_buf()
 *   command framing ...... mt7925/mcu.c      mt7925_mcu_fill_message()
 *   response parsing ..... mt7925/mcu.c      mt7925_mcu_parse_response()
 *   patch download ....... mt76_connac_mcu.c mt76_connac2_load_patch()
 *   RAM download ......... mt76_connac_mcu.c mt76_connac2_load_ram()
 *   firmware ready ....... mt792x_core.c     mt792x_load_firmware()
 *
 * Anunix drives the rings by polling. Interrupts stay masked; the DMA_DONE
 * bit and the ring indices carry all the state the protocol needs.
 */

#include <anx/types.h>
#include <anx/pci.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/delay.h>
#include <anx/mt7925.h>
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
/* Descriptors and rings                                               */
/* ------------------------------------------------------------------ */

/* struct mt76_desc, dma.h lines 44-49 */
struct mt7925_dma_desc {
	uint32_t buf;
	uint32_t ctrl;
	uint32_t buf1;
	uint32_t info;
} __attribute__((packed));

#define TX_RING_SIZE		32
#define EVT_RING_SIZE		32
#define DATA_TX_RING_SIZE	64
#define DATA_RX_RING_SIZE	64
#define RX_BUF_SIZE		2048		/* MT_RX_BUF_SIZE, mt76.h */
#define FW_CHUNK_SIZE		4096		/* PCIe max_len, connac_mcu.c */

/* 32 or 64 buffers of 2 KiB need 16 or 32 pages. */
#define EVT_BUF_ORDER		4
#define DATA_RX_BUF_ORDER	5

/* Command bodies reach a few KiB once a CLC segment is attached. */
#define CMD_BUF_ORDER		2
#define CMD_BUF_SIZE		(ANX_PAGE_SIZE << CMD_BUF_ORDER)

struct fw_tx_ring {
	uint32_t                ring;		/* ring index */
	uint32_t                count;
	struct mt7925_dma_desc *desc;
	uint32_t                head;		/* next slot the host fills */
};

struct fw_rx_ring {
	uint32_t                ring;
	uint32_t                count;
	uint32_t                order;		/* buffer pages */
	struct mt7925_dma_desc *desc;
	uint8_t                *bufs;
	uint32_t                tail;		/* next slot to read */
};

static struct fw_tx_ring g_wm   = { .ring = MT_MCU_WM_TXRING,   .count = TX_RING_SIZE };
static struct fw_tx_ring g_fwdl = { .ring = MT_MCU_FWDL_TXRING, .count = TX_RING_SIZE };
static struct fw_tx_ring g_data_tx = { .ring = MT_DATA_TXRING, .count = DATA_TX_RING_SIZE };
static struct fw_rx_ring g_evt  = { .ring = MT_MCU_EVENT_RXRING, .count = EVT_RING_SIZE,
				    .order = EVT_BUF_ORDER };
static struct fw_rx_ring g_data_rx = { .ring = MT_DATA_RXRING, .count = DATA_RX_RING_SIZE,
				       .order = DATA_RX_BUF_ORDER };

/* One page for outgoing command headers and payloads. */
static uint8_t *g_cmd_buf;

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */

/* Both take a chip address; offsets below the BAR size pass through. */
static inline uint32_t rr(uint32_t reg)
{
	return anx_mt7925_rr(reg);
}

static inline void wr(uint32_t reg, uint32_t val)
{
	anx_mt7925_wr(reg, val);
}

static inline void set_bits(uint32_t reg, uint32_t bits)
{
	wr(reg, rr(reg) | bits);
}

static inline void clear_bits(uint32_t reg, uint32_t bits)
{
	wr(reg, rr(reg) & ~bits);
}

/* Poll (reg & mask) == val for up to ms milliseconds. */
static bool poll_ms(uint32_t reg, uint32_t mask, uint32_t val, uint32_t ms)
{
	uint32_t i;

	for (i = 0; i <= ms; i++) {
		if ((rr(reg) & mask) == val)
			return true;
		anx_delay_ms(1);
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* DMA registers used only here                                        */
/* mt792x_regs.h lines 263-387, 406-408, 460-463                       */
/* ------------------------------------------------------------------ */

#define WFDMA0_RST			(MT_WFDMA0_BASE + 0x100)
#define WFDMA0_RST_LOGIC_RST		(1U << 4)
#define WFDMA0_RST_DMASHDL_ALL_RST	(1U << 5)

#define GLO_TX_DMA_EN			(1U << 0)
#define GLO_TX_DMA_BUSY			(1U << 1)
#define GLO_RX_DMA_EN			(1U << 2)
#define GLO_RX_DMA_BUSY			(1U << 3)
#define GLO_DMA_SIZE_3			(3U << 4)
#define GLO_TX_WB_DDONE			(1U << 6)
#define GLO_FIFO_DIS_CHECK		(1U << 11)
#define GLO_FIFO_LITTLE_ENDIAN		(1U << 12)
#define GLO_RX_WB_DDONE			(1U << 13)
#define GLO_CSR_DISP_BASE_PTR_CHAIN_EN	(1U << 15)
#define GLO_OMIT_RX_INFO_PFET2		(1U << 21)
#define GLO_OMIT_RX_INFO		(1U << 27)
#define GLO_OMIT_TX_INFO		(1U << 28)
#define GLO_CLK_GAT_DIS			(1U << 30)

#define WFDMA0_INT_RX_PRI		(MT_WFDMA0_BASE + 0x298)
#define WFDMA0_INT_TX_PRI		(MT_WFDMA0_BASE + 0x29c)
#define WFDMA0_GLO_CFG_EXT0		(MT_WFDMA0_BASE + 0x2b0)
#define WFDMA0_CSR_TX_DMASHDL_ENABLE	(1U << 6)
#define WFDMA0_PRI_DLY_INT_CFG0		(MT_WFDMA0_BASE + 0x2f0)

#define TX_RING_EXT_CTRL(n)		(MT_WFDMA0_BASE + 0x600 + (n) * 4)
#define RX_RING_EXT_CTRL(n)		(MT_WFDMA0_BASE + 0x680 + (n) * 4)
#define PREFETCH(base, depth)		(((base) << 16) | (depth))

#define WFDMA_DUMMY_CR			0x54000120u	/* chip address */
#define WFDMA_NEED_REINIT		(1U << 1)
#define DMASHDL_SW_CONTROL		0x7c026004u	/* chip address */
#define DMASHDL_BYPASS			(1U << 28)
#define UWFDMA0_GLO_CFG_EXT1		0x7c0242b4u	/* chip address */

/* ------------------------------------------------------------------ */
/* DMA disable, ring setup, enable                                     */
/* ------------------------------------------------------------------ */

/* mt792x_dma_disable(dev, force = true) */
static int dma_disable(void)
{
	clear_bits(MT_WFDMA0_GLO_CFG,
		   GLO_TX_DMA_EN | GLO_RX_DMA_EN |
		   GLO_CSR_DISP_BASE_PTR_CHAIN_EN |
		   GLO_OMIT_TX_INFO | GLO_OMIT_RX_INFO |
		   GLO_OMIT_RX_INFO_PFET2);

	if (!poll_ms(MT_WFDMA0_GLO_CFG, GLO_TX_DMA_BUSY | GLO_RX_DMA_BUSY,
		     0, 100)) {
		kprintf("mt7925: DMA did not go idle, GLO_CFG=0x%08x\n",
			rr(MT_WFDMA0_GLO_CFG));
		return ANX_ETIMEDOUT;
	}

	clear_bits(WFDMA0_GLO_CFG_EXT0, WFDMA0_CSR_TX_DMASHDL_ENABLE);
	set_bits(DMASHDL_SW_CONTROL, DMASHDL_BYPASS);

	clear_bits(WFDMA0_RST, WFDMA0_RST_DMASHDL_ALL_RST |
			       WFDMA0_RST_LOGIC_RST);
	set_bits(WFDMA0_RST, WFDMA0_RST_DMASHDL_ALL_RST |
			     WFDMA0_RST_LOGIC_RST);
	return ANX_OK;
}

/* mt792x_dma_prefetch(), mt7925 branch */
static void dma_prefetch(void)
{
	wr(RX_RING_EXT_CTRL(0), PREFETCH(0x0000, 0x4));
	wr(RX_RING_EXT_CTRL(1), PREFETCH(0x0040, 0x4));
	wr(RX_RING_EXT_CTRL(2), PREFETCH(0x0080, 0x4));
	wr(RX_RING_EXT_CTRL(3), PREFETCH(0x00c0, 0x4));
	wr(TX_RING_EXT_CTRL(0), PREFETCH(0x0100, 0x10));
	wr(TX_RING_EXT_CTRL(1), PREFETCH(0x0200, 0x10));
	wr(TX_RING_EXT_CTRL(2), PREFETCH(0x0300, 0x10));
	wr(TX_RING_EXT_CTRL(3), PREFETCH(0x0400, 0x10));
	wr(TX_RING_EXT_CTRL(15), PREFETCH(0x0500, 0x4));
	wr(TX_RING_EXT_CTRL(16), PREFETCH(0x0540, 0x4));
}

/*
 * __mt76_dma_queue_reset() then mt76_dma_sync_idx(): every descriptor
 * starts with DMA_DONE set, both indices are zeroed, then the base and
 * size are programmed.
 */
static int tx_ring_setup(struct fw_tx_ring *r)
{
	uintptr_t pa = anx_page_alloc(0);
	uint32_t i;

	if (!pa)
		return ANX_ENOMEM;
	anx_memset((void *)pa, 0, ANX_PAGE_SIZE);
	r->desc = (struct mt7925_dma_desc *)pa;
	for (i = 0; i < r->count; i++)
		r->desc[i].ctrl = MT_DMA_CTRL_DMA_DONE;

	wr(MT_WFDMA0_TX_RING_CIDX(r->ring), 0);
	wr(MT_WFDMA0_TX_RING_DIDX(r->ring), 0);
	wr(MT_WFDMA0_TX_RING_ADDR(r->ring), (uint32_t)pa);
	wr(MT_WFDMA0_TX_RING_CNT(r->ring), r->count);
	r->head = rr(MT_WFDMA0_TX_RING_DIDX(r->ring)) % r->count;
	return ANX_OK;
}

static int rx_ring_setup(struct fw_rx_ring *r)
{
	uintptr_t ring = anx_page_alloc(0);
	uintptr_t bufs = anx_page_alloc(r->order);
	uint32_t i;

	if (!ring || !bufs)
		return ANX_ENOMEM;
	anx_memset((void *)ring, 0, ANX_PAGE_SIZE);
	r->desc = (struct mt7925_dma_desc *)ring;
	r->bufs = (uint8_t *)bufs;

	for (i = 0; i < r->count; i++)
		r->desc[i].ctrl = MT_DMA_CTRL_DMA_DONE;

	wr(MT_WFDMA0_RX_RING_CIDX(r->ring), 0);
	wr(MT_WFDMA0_RX_RING_DIDX(r->ring), 0);
	wr(MT_WFDMA0_RX_RING_ADDR(r->ring), (uint32_t)ring);
	wr(MT_WFDMA0_RX_RING_CNT(r->ring), r->count);
	return ANX_OK;
}

/*
 * mt76_dma_rx_fill_buf(): hand the device buffers. Each slot is armed
 * with its own buffer and DMA_DONE clear; the device sets DMA_DONE when
 * it has written one.
 *
 * cpu_idx trails the read position by one: the device fills from its
 * dma_idx up to, not including, cpu_idx. With the reader at slot 0 that is
 * slot count-1, matching mt76 leaving one slot unfilled.
 */
static void rx_ring_fill(struct fw_rx_ring *r)
{
	uint32_t i, n = r->count;

	for (i = 0; i < n; i++) {
		r->desc[i].buf  = (uint32_t)(uintptr_t)(r->bufs + i * RX_BUF_SIZE);
		r->desc[i].buf1 = 0;
		r->desc[i].info = 0;
		r->desc[i].ctrl = MT_DMA_CTRL_LEN0(RX_BUF_SIZE);
	}
	r->tail = rr(MT_WFDMA0_RX_RING_DIDX(r->ring)) % n;
	wr(MT_WFDMA0_RX_RING_CIDX(r->ring), (r->tail + n - 1) % n);
}

/* mt792x_dma_enable(), interrupts left masked */
static void dma_enable(void)
{
	dma_prefetch();

	wr(MT_WFDMA0_RST_DTX_PTR, 0xffffffffu);
	wr(MT_WFDMA0_RST_DRX_PTR, 0xffffffffu);

	wr(WFDMA0_PRI_DLY_INT_CFG0, 0);

	set_bits(MT_WFDMA0_GLO_CFG,
		 GLO_TX_WB_DDONE | GLO_FIFO_LITTLE_ENDIAN | GLO_CLK_GAT_DIS |
		 GLO_OMIT_TX_INFO | GLO_DMA_SIZE_3 | GLO_FIFO_DIS_CHECK |
		 GLO_RX_WB_DDONE | GLO_CSR_DISP_BASE_PTR_CHAIN_EN |
		 GLO_OMIT_RX_INFO_PFET2);
	set_bits(MT_WFDMA0_GLO_CFG, GLO_TX_DMA_EN | GLO_RX_DMA_EN);

	set_bits(UWFDMA0_GLO_CFG_EXT1, 1U << 28);
	set_bits(WFDMA0_INT_RX_PRI, 0x0F00);
	set_bits(WFDMA0_INT_TX_PRI, 0x7F00);
	set_bits(WFDMA_DUMMY_CR, WFDMA_NEED_REINIT);
}

/*
 * mt7925_dma_init(): every ring, data rings included, is programmed while
 * the DMA is disabled, and only then is the DMA enabled. The index reset in
 * dma_enable() covers all of them.
 */
static int dma_init(void)
{
	int ret;

	ret = dma_disable();
	if (ret)
		return ret;

	if (!g_cmd_buf) {
		uintptr_t pa = anx_page_alloc(CMD_BUF_ORDER);

		if (!pa)
			return ANX_ENOMEM;
		g_cmd_buf = (uint8_t *)pa;
	}

	if (tx_ring_setup(&g_data_tx) || tx_ring_setup(&g_wm) ||
	    tx_ring_setup(&g_fwdl) || rx_ring_setup(&g_evt) ||
	    rx_ring_setup(&g_data_rx))
		return ANX_ENOMEM;
	wr(TX_RING_EXT_CTRL(0), 0x4);	/* MT_WFDMA0_TX_RING0_EXT_CTRL */

	dma_enable();
	rx_ring_fill(&g_evt);
	rx_ring_fill(&g_data_rx);

	kprintf("mt7925: DMA up, GLO_CFG=0x%08x, rings tx %u/%u/%u rx %u/%u\n",
		rr(MT_WFDMA0_GLO_CFG), g_data_tx.ring, g_wm.ring, g_fwdl.ring,
		g_evt.ring, g_data_rx.ring);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Ring I/O                                                            */
/* ------------------------------------------------------------------ */

static void tx_fill(struct fw_tx_ring *r, uint32_t phys, uint32_t len)
{
	struct mt7925_dma_desc *d = &r->desc[r->head];

	d->buf  = phys;
	d->buf1 = 0;
	d->info = 0;
	d->ctrl = MT_DMA_CTRL_LEN0(len) | MT_DMA_CTRL_LAST_SEC0;

	r->head = (r->head + 1) % r->count;
	wr(MT_WFDMA0_TX_RING_CIDX(r->ring), r->head);
}

/*
 * mt76_dma_add_buf() and mt76_dma_kick_queue(), then wait for the device
 * to take the descriptor. Waiting gives serial flow control: a slot is
 * never reused before the device has consumed it.
 */
static int tx_push(struct fw_tx_ring *r, uint32_t phys, uint32_t len)
{
	tx_fill(r, phys, len);

	if (!poll_ms(MT_WFDMA0_TX_RING_DIDX(r->ring), 0xfff, r->head, 3000)) {
		kprintf("mt7925: ring %u stuck, cpu_idx=%u dma_idx=%u\n",
			r->ring, r->head,
			rr(MT_WFDMA0_TX_RING_DIDX(r->ring)));
		return ANX_ETIMEDOUT;
	}
	return ANX_OK;
}

uint8_t *mt7925_cmd_buf(uint32_t *cap)
{
	if (cap)
		*cap = g_cmd_buf ? CMD_BUF_SIZE : 0;
	return g_cmd_buf;
}

/*
 * Queue a command already framed in the command buffer on the WM ring.
 * The buffer is the ring's only storage, and tx_push() waits until the
 * device has taken it before the next command may overwrite it.
 */
int mt7925_wm_send(uint32_t len)
{
	if (!g_cmd_buf || !g_wm.desc || len > CMD_BUF_SIZE)
		return ANX_EINVAL;
	return tx_push(&g_wm, (uint32_t)(uintptr_t)g_cmd_buf, len);
}

/*
 * Data frames do not wait: the frame's storage is owned by its token until
 * the firmware reports it free, so the descriptor only has to be taken
 * eventually. A full ring is reported and the frame dropped.
 */
int mt7925_data_tx_push(uint32_t txwi_phys, uint32_t len)
{
	struct fw_tx_ring *r = &g_data_tx;
	uint32_t didx;

	if (!r->desc)
		return ANX_EIO;
	didx = rr(MT_WFDMA0_TX_RING_DIDX(r->ring)) % r->count;
	if ((r->head + 1) % r->count == didx)
		return ANX_EBUSY;
	tx_fill(r, txwi_phys, len);
	return ANX_OK;
}

/* mt76_dma_dequeue(). NULL when nothing is ready. */
static const uint8_t *rx_poll(struct fw_rx_ring *r, uint32_t *out_len)
{
	struct mt7925_dma_desc *d;
	uint32_t slot;

	if (!r->desc)
		return NULL;

	slot = r->tail % r->count;
	d = &r->desc[slot];
	if (!(d->ctrl & MT_DMA_CTRL_DMA_DONE))
		return NULL;

	if (out_len) {
		uint32_t len = MT_DMA_CTRL_GET_LEN0(d->ctrl);

		*out_len = len > RX_BUF_SIZE ? RX_BUF_SIZE : len;
	}

	/* Re-arm this slot and let the device fill up to it. */
	d->ctrl = MT_DMA_CTRL_LEN0(RX_BUF_SIZE);
	r->tail = (slot + 1) % r->count;
	wr(MT_WFDMA0_RX_RING_CIDX(r->ring), slot);
	return r->bufs + slot * RX_BUF_SIZE;
}

const uint8_t *mt7925_evt_poll(uint32_t *out_len)
{
	return rx_poll(&g_evt, out_len);
}

const uint8_t *mt7925_data_rx_poll(uint32_t *out_len)
{
	return rx_poll(&g_data_rx, out_len);
}

/* ------------------------------------------------------------------ */
/* MCU commands                                                        */
/* ------------------------------------------------------------------ */

/* mt76_connac_mcu.h lines 44-45, 1068-1089, 1300-1309, 1336-1343 */
#define CMD_TARGET_ADDRESS_LEN_REQ	0x01
#define CMD_FW_START_REQ		0x02
#define CMD_PATCH_START_REQ		0x05
#define CMD_PATCH_FINISH_REQ		0x07
#define CMD_PATCH_SEM_CONTROL		0x10

#define MCU_PKT_ID			0xa0
#define MCU_Q_NA			3
#define MCU_S2D_H2N			0

#define PATCH_SEM_RELEASE		0
#define PATCH_SEM_GET			1

#define PATCH_NOT_DL_SEM_FAIL		0
#define PATCH_IS_DL			1
#define PATCH_NOT_DL_SEM_SUCCESS	2
#define PATCH_REL_SEM_SUCCESS		3

/*
 * mt76_connac3_mac.h: mt7925/mcu.c includes mac.h, which pulls in the
 * connac3 layout, so HDR_FORMAT sits at bits 15:14 for every command,
 * firmware download included.
 */
#define TXD0_TX_BYTES_MASK		0xffffU
#define TXD0_PKT_FMT_SHIFT		23
#define TXD0_Q_IDX_SHIFT		25
#define TX_TYPE_CMD			2
#define TX_MCU_PORT_RX_Q0		0x20
#define TX_PORT_IDX_MCU			1
#define TXD1_HDR_FORMAT_SHIFT		14
#define HDR_FORMAT_CMD			1
#define MCU_PQ_ID(p, q)			((((p) << 15) | ((q) << 10)) & 0xffff)

/* struct mt76_connac2_mcu_txd, mt76_connac_mcu.h lines 47-64 */
struct mcu_txd {
	uint32_t txd[8];
	uint16_t len;
	uint16_t pq_id;
	uint8_t  cid;
	uint8_t  pkt_type;
	uint8_t  set_query;
	uint8_t  seq;
	uint8_t  uc_d2b0_rev;
	uint8_t  ext_cid;
	uint8_t  s2d_index;
	uint8_t  ext_cid_ack;
	uint32_t rsv[5];
} __attribute__((packed));

/*
 * struct mt7925_mcu_rxd, mt7925/mcu.h lines 26-42: 32 bytes of rxd, then
 * len, pkt_type_id, eid, seq. parse_response() pulls sizeof(rxd) - 4 = 40
 * bytes and returns the next byte for the patch commands.
 */
#define RXD_SEQ_OFFSET			37
#define RXD_STATUS_OFFSET		40

static uint32_t g_msg_seq;

/* 4-bit sequence, never zero (mt7925_mcu_fill_message) */
uint8_t mt7925_mcu_next_seq(void)
{
	uint8_t seq = ++g_msg_seq & 0xf;

	if (!seq)
		seq = ++g_msg_seq & 0xf;
	return seq;
}

/*
 * Send one legacy MCU command on the WM ring and, when asked, wait for the
 * event carrying the same sequence number. *status receives the byte
 * mt7925_mcu_parse_response() returns for PATCH_SEM_CONTROL and
 * PATCH_FINISH_REQ.
 */
static int mcu_cmd(uint8_t cid, const void *payload, uint32_t plen,
		   bool wait, int *status)
{
	struct mcu_txd *t = (struct mcu_txd *)g_cmd_buf;
	uint32_t total = sizeof(*t) + plen;
	uint32_t i;
	uint8_t seq;
	int ret;

	if (total > CMD_BUF_SIZE)
		return ANX_EINVAL;

	seq = mt7925_mcu_next_seq();
	anx_memset(t, 0, sizeof(*t));
	t->txd[0] = (total & TXD0_TX_BYTES_MASK) |
		    ((uint32_t)TX_TYPE_CMD << TXD0_PKT_FMT_SHIFT) |
		    ((uint32_t)TX_MCU_PORT_RX_Q0 << TXD0_Q_IDX_SHIFT);
	t->txd[1] = (uint32_t)HDR_FORMAT_CMD << TXD1_HDR_FORMAT_SHIFT;
	t->len       = (uint16_t)(total - sizeof(t->txd));
	t->pq_id     = (uint16_t)MCU_PQ_ID(TX_PORT_IDX_MCU, TX_MCU_PORT_RX_Q0);
	t->cid       = cid;
	t->pkt_type  = MCU_PKT_ID;
	t->set_query = MCU_Q_NA;
	t->seq       = seq;
	t->s2d_index = MCU_S2D_H2N;
	if (plen)
		anx_memcpy(g_cmd_buf + sizeof(*t), payload, plen);

	ret = tx_push(&g_wm, (uint32_t)(uintptr_t)g_cmd_buf, total);
	if (ret) {
		kprintf("mt7925: cmd 0x%02x seq %u not taken by the device\n",
			cid, seq);
		return ret;
	}
	if (!wait)
		return ANX_OK;

	/* mt76 allows 3 s per message (mt7925_mcu_send_message). */
	for (i = 0; i < 3000; i++) {
		uint32_t len;
		const uint8_t *e;

		while ((e = mt7925_evt_poll(&len)) != NULL) {
			if (len <= RXD_STATUS_OFFSET || e[RXD_SEQ_OFFSET] != seq)
				continue;
			if (status)
				*status = e[RXD_STATUS_OFFSET];
			return ANX_OK;
		}
		anx_delay_ms(1);
	}
	kprintf("mt7925: cmd 0x%02x seq %u: no response, event ring "
		"dma_idx=%u read=%u\n", cid, seq,
		rr(MT_WFDMA0_RX_RING_DIDX(MT_MCU_EVENT_RXRING)), g_evt.tail);
	return ANX_ETIMEDOUT;
}

/*
 * FW_SCATTER carries no header: mt7925_mcu_fill_message() skips framing
 * for it, and it goes on the FWDL ring. The descriptor points straight at
 * the embedded image, which lives in the kernel below 4 GiB.
 */
static int send_firmware(const uint8_t *data, uint32_t len)
{
	uint32_t off = 0;
	int ret;

	while (off < len) {
		uint32_t n = len - off;

		if (n > FW_CHUNK_SIZE)
			n = FW_CHUNK_SIZE;
		ret = tx_push(&g_fwdl, (uint32_t)(uintptr_t)(data + off), n);
		if (ret) {
			kprintf("mt7925: scatter stalled at 0x%x of 0x%x\n",
				off, len);
			return ret;
		}
		off += n;
	}
	return ANX_OK;
}

static int patch_sem(bool get, int *status)
{
	uint32_t op = get ? PATCH_SEM_GET : PATCH_SEM_RELEASE;

	return mcu_cmd(CMD_PATCH_SEM_CONTROL, &op, sizeof(op), true, status);
}

/* mt76_connac_mcu_init_download(), mt7925 branch */
static int init_download(uint32_t addr, uint32_t len, uint32_t mode)
{
	struct {
		uint32_t addr;
		uint32_t len;
		uint32_t mode;
	} __attribute__((packed)) req = { addr, len, mode };
	uint8_t cid;

	if (addr == 0x200000 || addr == 0x900000 || addr == 0xe0002800)
		cid = CMD_PATCH_START_REQ;
	else
		cid = CMD_TARGET_ADDRESS_LEN_REQ;

	return mcu_cmd(cid, &req, sizeof(req), true, NULL);
}

/* ------------------------------------------------------------------ */
/* Firmware image formats                                              */
/* ------------------------------------------------------------------ */

/* mt76_connac_mcu.h lines 9-36 */
#define FW_FEATURE_SET_ENCRYPT		(1U << 0)
#define FW_FEATURE_SET_KEY_IDX		(3U << 1)
#define FW_FEATURE_ENCRY_MODE		(1U << 4)
#define FW_FEATURE_OVERRIDE_ADDR	(1U << 5)
#define FW_FEATURE_NON_DL		(1U << 6)

#define DL_MODE_ENCRYPT			(1U << 0)
#define DL_MODE_KEY_IDX_SHIFT		1
#define DL_MODE_KEY_IDX			(3U << 1)
#define DL_MODE_RESET_SEC_IV		(1U << 3)
#define DL_CONFIG_ENCRY_MODE_SEL	(1U << 6)
#define DL_MODE_NEED_RSP		(1U << 31)

#define FW_START_OVERRIDE		(1U << 0)

#define PATCH_SEC_NOT_SUPPORT		0xffffffffU
#define PATCH_SEC_TYPE_MASK		0xffffU
#define PATCH_SEC_TYPE_INFO		0x2
#define PATCH_SEC_ENC_TYPE_SHIFT	24
#define PATCH_SEC_ENC_TYPE_PLAIN	0x00
#define PATCH_SEC_ENC_TYPE_AES		0x01
#define PATCH_SEC_ENC_TYPE_SCRAMBLE	0x02
#define PATCH_SEC_ENC_AES_KEY_MASK	0xffU

/* On-disk layouts, mt76_connac_mcu.h lines 139-194 */
#define PATCH_HDR_SIZE			96
#define PATCH_SEC_SIZE			64
#define FW_TRAILER_SIZE			36
#define FW_REGION_SIZE			40

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* mt76_connac2_get_data_mode() */
static uint32_t patch_data_mode(uint32_t info)
{
	uint32_t mode = DL_MODE_NEED_RSP;

	if (info == PATCH_SEC_NOT_SUPPORT)
		return mode;

	switch (info >> PATCH_SEC_ENC_TYPE_SHIFT) {
	case PATCH_SEC_ENC_TYPE_PLAIN:
		break;
	case PATCH_SEC_ENC_TYPE_AES:
		mode |= DL_MODE_ENCRYPT;
		mode |= ((info & PATCH_SEC_ENC_AES_KEY_MASK)
			 << DL_MODE_KEY_IDX_SHIFT) & DL_MODE_KEY_IDX;
		mode |= DL_MODE_RESET_SEC_IV;
		break;
	case PATCH_SEC_ENC_TYPE_SCRAMBLE:
		mode |= DL_MODE_ENCRYPT;
		mode |= DL_CONFIG_ENCRY_MODE_SEL;
		mode |= DL_MODE_RESET_SEC_IV;
		break;
	default:
		kprintf("mt7925: unsupported patch encryption 0x%x\n", info);
	}
	return mode;
}

/* mt76_connac_mcu_gen_dl_mode() for the WM image */
static uint32_t ram_data_mode(uint8_t fs)
{
	uint32_t mode = DL_MODE_NEED_RSP;

	if (fs & FW_FEATURE_SET_ENCRYPT)
		mode |= DL_MODE_ENCRYPT | DL_MODE_RESET_SEC_IV;
	if (fs & FW_FEATURE_ENCRY_MODE)
		mode |= DL_CONFIG_ENCRY_MODE_SEL;
	/* FW_FEATURE_SET_KEY_IDX and DL_MODE_KEY_IDX are the same bits. */
	mode |= fs & FW_FEATURE_SET_KEY_IDX;
	return mode;
}

/* mt76_connac2_load_patch() */
static int load_patch(void)
{
	const uint8_t *fw = mt7925_patch_fw;
	uint32_t size = mt7925_patch_fw_size;
	uint32_t n_region, i;
	int ret = ANX_OK, sem = -1, rel = -1;

	if (size < PATCH_HDR_SIZE)
		return ANX_EINVAL;

	if (patch_sem(true, &sem) != ANX_OK)
		return ANX_ETIMEDOUT;

	kprintf("mt7925: patch semaphore status %d\n", sem);
	if (sem == PATCH_IS_DL) {
		kprintf("mt7925: patch already applied\n");
		return ANX_OK;
	}
	if (sem != PATCH_NOT_DL_SEM_SUCCESS) {
		kprintf("mt7925: could not take the patch semaphore\n");
		return ANX_EBUSY;
	}

	n_region = be32(fw + 32 + 12);
	kprintf("mt7925: patch hw/sw 0x%08x, %u sections\n",
		be32(fw + 20), n_region);

	for (i = 0; i < n_region; i++) {
		const uint8_t *sec = fw + PATCH_HDR_SIZE + i * PATCH_SEC_SIZE;
		uint32_t type, offs, addr, len, info, mode;

		if (sec + PATCH_SEC_SIZE > fw + size) {
			ret = ANX_EINVAL;
			goto out;
		}
		type = be32(sec);
		offs = be32(sec + 4);
		addr = be32(sec + 12);
		len  = be32(sec + 16);
		info = be32(sec + 20);

		if ((type & PATCH_SEC_TYPE_MASK) != PATCH_SEC_TYPE_INFO ||
		    offs + len > size) {
			ret = ANX_EINVAL;
			goto out;
		}

		mode = patch_data_mode(info);
		kprintf("mt7925: patch section %u: 0x%x bytes to 0x%08x, "
			"mode 0x%08x\n", i, len, addr, mode);

		ret = init_download(addr, len, mode);
		if (ret) {
			kprintf("mt7925: download request failed\n");
			goto out;
		}
		ret = send_firmware(fw + offs, len);
		if (ret)
			goto out;
	}

	{
		struct { uint8_t check_crc; uint8_t rsv[3]; } req = { 0, {0} };
		int st = -1;

		ret = mcu_cmd(CMD_PATCH_FINISH_REQ, &req, sizeof(req), true, &st);
		kprintf("mt7925: patch finish status %d\n", st);
	}

out:
	if (patch_sem(false, &rel) != ANX_OK || rel != PATCH_REL_SEM_SUCCESS) {
		kprintf("mt7925: patch semaphore release status %d\n", rel);
		if (ret == ANX_OK)
			ret = ANX_EBUSY;
	}
	return ret;
}

/* mt76_connac2_load_ram() and mt76_connac_mcu_send_ram_firmware() */
static int load_ram(void)
{
	const uint8_t *fw = mt7925_ram_fw;
	uint32_t size = mt7925_ram_fw_size;
	const uint8_t *trailer;
	uint32_t n_region, i, offset = 0, override = 0, option = 0;
	int ret;

	if (size < FW_TRAILER_SIZE)
		return ANX_EINVAL;
	trailer  = fw + size - FW_TRAILER_SIZE;
	n_region = trailer[2];
	kprintf("mt7925: WM image, %u regions\n", n_region);

	for (i = 0; i < n_region; i++) {
		const uint8_t *reg = trailer - (n_region - i) * FW_REGION_SIZE;
		uint32_t addr = le32(reg + 16);
		uint32_t len  = le32(reg + 20);
		uint8_t  fs   = reg[24];

		if (fs & FW_FEATURE_NON_DL) {
			kprintf("mt7925: WM region %u not downloaded\n", i);
			goto next;
		}
		if (fs & FW_FEATURE_OVERRIDE_ADDR)
			override = addr;
		if (offset + len > size) {
			kprintf("mt7925: WM region %u overruns the image\n", i);
			return ANX_EINVAL;
		}

		kprintf("mt7925: WM region %u: 0x%x bytes to 0x%08x, "
			"mode 0x%08x\n", i, len, addr, ram_data_mode(fs));
		ret = init_download(addr, len, ram_data_mode(fs));
		if (ret)
			return ret;
		ret = send_firmware(fw + offset, len);
		if (ret)
			return ret;
next:
		offset += len;
	}

	if (override)
		option |= FW_START_OVERRIDE;
	{
		struct { uint32_t option; uint32_t addr; }
			__attribute__((packed)) req = { option, override };

		kprintf("mt7925: starting WM at 0x%08x, option 0x%x\n",
			override, option);
		return mcu_cmd(CMD_FW_START_REQ, &req, sizeof(req), true, NULL);
	}
}

/* ------------------------------------------------------------------ */
/* Public: bring up DMA, download firmware, wait for it to run         */
/* ------------------------------------------------------------------ */

int mt7925_fw_download(struct mt7925_dev *dev)
{
	int ret;

	(void)dev;
	kprintf("mt7925: patch size=%u RAM size=%u\n",
		mt7925_patch_fw_size, mt7925_ram_fw_size);
	kprintf("mt7925: TOP_MISC=0x%08x (fw_state=%u) CONN_ON_MISC=0x%08x\n",
		rr(MT_TOP_MISC2), rr(MT_TOP_MISC2) & MT_TOP_MISC2_FW_STATE,
		rr(MT_CONN_ON_MISC));

	ret = dma_init();
	if (ret)
		return ret;

	kprintf("mt7925: acquiring patch semaphore\n");
	ret = load_patch();
	if (ret) {
		kprintf("mt7925: patch download failed (%d)\n", ret);
		return ret;
	}

	ret = load_ram();
	if (ret) {
		kprintf("mt7925: WM download failed (%d)\n", ret);
		return ret;
	}

	/* mt792x_load_firmware(): both N9 ready bits within 1.5 s. */
	if (!poll_ms(MT_CONN_ON_MISC, MT_TOP_MISC2_FW_N9_RDY,
		     MT_TOP_MISC2_FW_N9_RDY, 1500)) {
		kprintf("mt7925: firmware did not come up, "
			"CONN_ON_MISC=0x%08x\n", rr(MT_CONN_ON_MISC));
		return ANX_ETIMEDOUT;
	}

	kprintf("mt7925: firmware running, CONN_ON_MISC=0x%08x\n",
		rr(MT_CONN_ON_MISC));
	return ANX_OK;
}
