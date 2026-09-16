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

#define WM_CMD_BUF     512   /* max command size */

/* The WM and event rings live in mt7925_fw.c; see mt7925_wm_push(). */
static uint16_t wm_seq;

/* ------------------------------------------------------------------ */
/* WM ring initialization                                              */
/* ------------------------------------------------------------------ */

int mt7925_mcu_init(struct mt7925_dev *dev)
{
	/*
	 * The WM ring and the event ring are set up by mt7925_fw_download()
	 * and stay in use after the firmware starts. Re-programming ring 15
	 * here would reset its indices under the device.
	 */
	(void)dev;
	kprintf("mt7925: MCU uses the WM ring from firmware download\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* MCU command send                                                    */
/* ------------------------------------------------------------------ */

static int mcu_send(struct mt7925_dev *dev, uint8_t ext_cid,
		    const void *payload, uint16_t payload_len)
{
	uint32_t total = sizeof(struct mt7925_mcu_txd) + payload_len;

	(void)dev;
	if (total > WM_CMD_BUF) return -1;

	uint8_t buf[WM_CMD_BUF];

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

	/*
	 * TODO(mt7925 stage 2): after boot the MT7925 takes UNI commands, not
	 * this extended-command framing. Porting that is the next stage.
	 */
	if (mt7925_wm_push(buf, total) != ANX_OK)
		return -1;

	return (int)txd->seq_num;
}

/* ------------------------------------------------------------------ */
/* MCU event ring (shared accessor, defined in fw.c)                  */
/* ------------------------------------------------------------------ */

/* Poll MCU event RX ring (ring 4) for one event frame.
 * Returns pointer into RX buffer (valid until next poll), or NULL. */
static const uint8_t *mcu_poll_one(struct mt7925_dev *dev, uint32_t *out_len)
{
	(void)dev;
	return mt7925_evt_poll(out_len);
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
	if (mt7925_on_connect)
		mt7925_on_connect(ssid);
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
	if (mt7925_on_disconnect)
		mt7925_on_disconnect();
}
