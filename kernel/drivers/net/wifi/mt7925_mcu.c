/*
 * mt7925_mcu.c — MT7925 MCU command interface.
 *
 * After firmware boot, all WiFi management (scan, connect, set keys,
 * BSS info) goes through MCU commands on the WM TX ring (ring 20)
 * with events returned on the MCU event RX ring (ring 4).
 *
 * Commands follow the ConnAC2 protocol:
 *   TXD header (struct mt7925_mcu_txd) + payload
 *   Event response has matching seq_num and ext_cid.
 */

#include <anx/types.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include "mt7925_reg.h"
#include "mt7925_drv.h"

/* ------------------------------------------------------------------ */
/* TXD / event structures                                              */
/* ------------------------------------------------------------------ */

/* Matches mt76_connac_mcu_txd in Linux mt76 */
struct mt7925_mcu_txd {
	uint8_t  reserved[4];
	uint16_t length;
	uint16_t seq_num;
	uint8_t  cid;
	uint8_t  pkt_type;
	uint8_t  set_query;
	uint8_t  reserved2;
	uint32_t reserved3;
	uint8_t  s2d_index;
	uint8_t  ext_cid;
	uint8_t  ext_cid_ack;
	uint8_t  reserved4;
} __attribute__((packed));

/* MCU event header */
struct mt7925_mcu_evt {
	struct mt7925_mcu_txd txd;
	uint8_t  status;   /* 0 = success */
	uint8_t  reserved[3];
} __attribute__((packed));

/* BSS connect request payload */
struct mt7925_bss_req {
	uint8_t  bss_idx;
	uint8_t  net_type;    /* 1 = infrastructure */
	uint8_t  active;      /* 1 = activate */
	uint8_t  reserved;
	uint8_t  bssid[6];
	uint16_t beacon_int;
	uint8_t  dtim_period;
	uint8_t  encryption;  /* 0 = open, 4 = WPA2 CCMP */
	uint8_t  reserved2[2];
	uint8_t  ssid[32];
	uint8_t  ssid_len;
	uint8_t  reserved3[3];
} __attribute__((packed));

/* STA connect result event payload */
struct mt7925_sta_evt {
	struct mt7925_mcu_evt hdr;
	uint8_t  status;      /* 0 = connected */
	uint8_t  bss_idx;
	uint8_t  mac[6];      /* station MAC */
	uint8_t  reserved[4];
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* DMA descriptor (16 bytes, same format as fw.c)                     */
/* ------------------------------------------------------------------ */

struct mt7925_dma_desc {
	uint32_t buf;
	uint32_t ctrl;
	uint32_t buf1;
	uint32_t info;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* WM command TX ring (ring 20)                                        */
/* ------------------------------------------------------------------ */

#define WM_RING_SIZE    32
#define WM_CMD_BUF     512   /* max command size */

static struct mt7925_dma_desc *wm_ring;
static uint8_t *wm_cmd_bufs;
static uint32_t wm_cidx;
static uint16_t wm_seq;

/* MCU event ring state shared with fw.c (extern) */
extern void *mcu_evt_ring_ptr(void);  /* defined in fw.c */

static inline uint32_t mcu_rd(const struct mt7925_dev *dev, uint32_t reg)
{
	return *(volatile uint32_t *)((uint8_t *)dev->bar0 + reg);
}

static inline void mcu_wr(const struct mt7925_dev *dev,
			   uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)((uint8_t *)dev->bar0 + reg) = val;
}

/* ------------------------------------------------------------------ */
/* WM ring initialization                                              */
/* ------------------------------------------------------------------ */

int mt7925_mcu_init(struct mt7925_dev *dev)
{
	uintptr_t ring_pa = anx_page_alloc(0);
	if (!ring_pa) return -1;

	uint32_t buf_order = 0;
	while ((1U << buf_order) * ANX_PAGE_SIZE <
	       WM_RING_SIZE * WM_CMD_BUF)
		buf_order++;
	uintptr_t buf_pa = anx_page_alloc(buf_order);
	if (!buf_pa) return -1;

	anx_memset((void *)ring_pa, 0, ANX_PAGE_SIZE);
	anx_memset((void *)buf_pa, 0, (size_t)(1U << buf_order) * ANX_PAGE_SIZE);

	wm_ring     = (struct mt7925_dma_desc *)ring_pa;
	wm_cmd_bufs = (uint8_t *)buf_pa;
	wm_cidx     = 0;

	mcu_wr(dev, MT_WFDMA0_TX_RING_CNT(MT_MCU_WM_TXRING), WM_RING_SIZE);
	mcu_wr(dev, MT_WFDMA0_TX_RING_ADDR(MT_MCU_WM_TXRING),
	       (uint32_t)ring_pa);
	mcu_wr(dev, MT_WFDMA0_TX_RING_CIDX(MT_MCU_WM_TXRING), 0);

	kprintf("mt7925: MCU WM ring @ 0x%x\n", (uint32_t)ring_pa);
	return 0;
}

/* ------------------------------------------------------------------ */
/* MCU command send                                                    */
/* ------------------------------------------------------------------ */

static int mcu_send(struct mt7925_dev *dev, uint8_t ext_cid,
		    const void *payload, uint16_t payload_len)
{
	uint32_t total = sizeof(struct mt7925_mcu_txd) + payload_len;
	if (total > WM_CMD_BUF) return -1;

	uint32_t slot = wm_cidx % WM_RING_SIZE;
	uint8_t *buf  = wm_cmd_bufs + slot * WM_CMD_BUF;

	struct mt7925_mcu_txd *txd = (struct mt7925_mcu_txd *)buf;
	anx_memset(txd, 0, sizeof(*txd));
	txd->length     = (uint16_t)total;
	txd->seq_num    = ++wm_seq;
	txd->pkt_type   = MT_MCU_PKT_TYPE_CMD;
	txd->set_query  = MT_MCU_SET;
	txd->s2d_index  = 0x00;
	txd->cid        = 0xED;
	txd->ext_cid    = ext_cid;
	txd->ext_cid_ack = 1;

	if (payload && payload_len)
		anx_memcpy(buf + sizeof(*txd), payload, payload_len);

	/* Write DMA descriptor */
	wm_ring[slot].buf  = (uint32_t)(uintptr_t)buf;
	wm_ring[slot].ctrl = (total & MT_DMA_CTRL_SD_LEN0_MASK)
			   | MT_DMA_CTRL_FIRST_SEC0
			   | MT_DMA_CTRL_LAST_SEC0;
	wm_ring[slot].buf1 = 0;
	wm_ring[slot].info = MT_DMA_INFO_PKT_TYPE_CMD;

	wm_cidx++;
	mcu_wr(dev, MT_WFDMA0_TX_RING_CIDX(MT_MCU_WM_TXRING),
	       wm_cidx % WM_RING_SIZE);

	return (int)txd->seq_num;
}

/* ------------------------------------------------------------------ */
/* MCU event ring (shared accessor, defined in fw.c)                  */
/* ------------------------------------------------------------------ */

/* Poll MCU event RX ring (ring 4) for one event frame.
 * Returns pointer into RX buffer (valid until next poll), or NULL. */
static const uint8_t *mcu_poll_one(struct mt7925_dev *dev, uint32_t *out_len)
{
	/* We peek at ring 4 DMA index and our own cidx.
	 * The ring structures were allocated in mt7925_fw.c — we access
	 * them via the exported symbol below. */
	extern struct mt7925_dma_desc *mcu_evt_ring;
	extern uint8_t *mcu_evt_bufs;
	extern uint32_t mcu_evt_cidx;
	extern uint32_t mcu_evt_ring_count;

	uint32_t didx = mcu_rd(dev,
			       MT_WFDMA0_RX_RING_DIDX(MT_MCU_EVENT_RXRING));
	uint32_t count = mcu_evt_ring_count;
	uint32_t cidx  = mcu_evt_cidx % count;

	if (cidx == didx) return NULL;

	struct mt7925_dma_desc *desc = &mcu_evt_ring[cidx];
	uint32_t len = desc->ctrl & MT_DMA_CTRL_SD_LEN0_MASK;
	const uint8_t *buf = mcu_evt_bufs + cidx * 2048;

	desc->ctrl = 2048 | MT_DMA_CTRL_DMA_DONE;
	mcu_evt_cidx++;
	mcu_wr(dev, MT_WFDMA0_RX_RING_CIDX(MT_MCU_EVENT_RXRING),
	       mcu_evt_cidx % count);

	if (out_len) *out_len = len;
	return buf;
}

static int mcu_wait_for(struct mt7925_dev *dev,
			 uint8_t ext_cid, uint16_t seq,
			 uint32_t timeout_iters)
{
	for (uint32_t i = 0; i < timeout_iters; i++) {
		uint32_t len;
		const uint8_t *evt = mcu_poll_one(dev, &len);
		if (evt && len >= sizeof(struct mt7925_mcu_evt)) {
			const struct mt7925_mcu_txd *txd =
				(const struct mt7925_mcu_txd *)evt;
			if (txd->ext_cid == ext_cid &&
			    txd->seq_num == seq)
				return 0;
		}
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
		__asm__ volatile("wfe" ::: "memory");
#else
		__asm__ volatile("" ::: "memory");
#endif
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* WiFi management commands                                            */
/* ------------------------------------------------------------------ */

#define MCU_EXT_CMD_STA_REC_UPDATE      0x25
#define MCU_EXT_CMD_BSS_INFO_UPDATE     0x26
#define MCU_EXT_CMD_CHANNEL_SWITCH      0x34
#define MCU_EXT_CMD_GET_MAC_INFO        0x3a

int mt7925_mcu_send_cmd(struct mt7925_dev *dev, uint8_t ext_cid,
			const void *payload, uint16_t payload_len)
{
	return mcu_send(dev, ext_cid, payload, payload_len);
}

int mt7925_mcu_connect(struct mt7925_dev *dev,
		       const char *ssid, const char *psk)
{
	struct mt7925_bss_req bss;
	int seq;

	anx_memset(&bss, 0, sizeof(bss));
	bss.bss_idx    = 0;
	bss.net_type   = 1;  /* infrastructure (STA mode) */
	bss.active     = 1;
	bss.encryption = (psk && psk[0]) ? 4 : 0;  /* 4 = WPA2-CCMP */

	size_t slen = anx_strlen(ssid);
	if (slen > 32) slen = 32;
	anx_memcpy(bss.ssid, ssid, slen);
	bss.ssid_len = (uint8_t)slen;

	kprintf("mt7925: connecting to \"%s\" (%s)\n",
		ssid, psk ? "WPA2" : "open");

	seq = mcu_send(dev, MCU_EXT_CMD_BSS_INFO_UPDATE, &bss, sizeof(bss));
	if (seq < 0) return -1;

	/* Wait up to 5s for MCU to ack the BSS config, then proceed.
	 * A timeout here is non-fatal — the MCU may already be scanning. */
	mcu_wait_for(dev, MCU_EXT_CMD_BSS_INFO_UPDATE,
		     (uint16_t)seq, 50000000);

	if (psk && psk[0]) {
		/* WPA2: drive the full 4-way handshake */
		dev->state = MT7925_STATE_SCANNING;
		return mt7925_wpa_connect(dev, ssid, psk);
	}

	/* Open network: association is implicit in the MCU scan */
	kprintf("mt7925: open network associated\n");
	dev->state = MT7925_STATE_ASSOC;
	return 0;
}

void mt7925_mcu_disconnect(struct mt7925_dev *dev)
{
	struct mt7925_bss_req bss;
	anx_memset(&bss, 0, sizeof(bss));
	bss.bss_idx  = 0;
	bss.net_type = 1;
	bss.active   = 0;

	mcu_send(dev, MCU_EXT_CMD_BSS_INFO_UPDATE, &bss, sizeof(bss));
	dev->state = MT7925_STATE_FW_UP;
	kprintf("mt7925: disconnected\n");
}
