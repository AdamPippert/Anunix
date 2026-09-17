/*
 * mt7925.c — MediaTek MT7925 (RZ717) Wi-Fi 7 driver: bring-up, frame
 * transport and the public interface.
 *
 * PCIe device 14C3:0717, the only network path on the Framework Laptop 16
 * (jekyll). The driver polls: interrupts stay masked and the rings are
 * drained from anx_mt7925_poll() and from every wait inside the driver.
 *
 * Bring-up follows Linux v6.19 mt76 in order: mt7925_pci_probe(),
 * mt7925_dma_init(), mt7925e_mcu_init(), __mt7925_init_hardware(),
 * mt7925_init_work(), __mt7925_start(), the regulatory update, and
 * mt7925_add_interface(). Association lives in mt7925_sta.c.
 */

#include <anx/types.h>
#include <anx/mt7925.h>
#include <anx/pci.h>
#include <anx/mmio.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/delay.h>
#include <anx/arch.h>
#include <anx/net.h>
#include "mt7925_reg.h"
#include "mt7925_drv.h"
#include "mt7925_mac.h"

struct mt7925_dev g_mt7925;
static bool g_ready;

#define MT7925_RTS_THRESHOLD	0x92b

/* ------------------------------------------------------------------ */
/* MMIO helpers                                                        */
/* ------------------------------------------------------------------ */

uint32_t anx_mt7925_bar_rd(uint32_t reg)
{
	return *(volatile uint32_t *)((uint8_t *)g_mt7925.bar0 + reg);
}

void anx_mt7925_bar_wr(uint32_t reg, uint32_t val)
{
	*(volatile uint32_t *)((uint8_t *)g_mt7925.bar0 + reg) = val;
}

static void rmw(uint32_t reg, uint32_t mask, uint32_t val)
{
	anx_mt7925_wr(reg, (anx_mt7925_rr(reg) & ~mask) | val);
}

static void set_bits(uint32_t reg, uint32_t bits)
{
	rmw(reg, 0, bits);
}

/* ------------------------------------------------------------------ */
/* MAC initialisation (mt7925_mac_init)                                 */
/* ------------------------------------------------------------------ */

#define MT_MDP_DCR0			0x820cc800u
#define MT_MDP_DCR0_DAMSDU_EN		(1U << 15)
#define MT_MDP_DCR1			0x820cc804u
#define MT_MAX_RX_LEN_MASK		0x0000fff8u	/* GENMASK(15, 3) */
#define MT_MAX_RX_LEN(v)		((uint32_t)(v) << 3)

#define MT_WTBLON_TOP			0x820d4000u
#define MT_WTBL_UPDATE			(MT_WTBLON_TOP + 0x380)
#define MT_WTBL_UPDATE_WLAN_IDX		0x00000fffu
#define MT_WTBL_UPDATE_ADM_COUNT_CLEAR	(1U << 14)
#define MT_WTBL_UPDATE_BUSY		(1U << 31)
#define MT_WTBL_ITCR			(MT_WTBLON_TOP + 0x3b0)
#define MT_WTBL_ITCR_WR			(1U << 16)
#define MT_WTBL_ITCR_EXEC		(1U << 31)
#define MT_WTBL_ITDR0			(MT_WTBLON_TOP + 0x3b8)
#define MT_WTBL_ITDR1			(MT_WTBLON_TOP + 0x3bc)
#define MT_WTBL_SPE_IDX_SEL		(1U << 6)

#define MT_WF_TMAC(b, o)		(((b) ? 0x820f4000u : 0x820e4000u) + (o))
#define MT_WF_RMAC(b, o)		(((b) ? 0x820f5000u : 0x820e5000u) + (o))
#define MT_WF_DMA(b, o)			(((b) ? 0x820f7000u : 0x820e7000u) + (o))
#define MT_WTBLOFF(b, o)		(((b) ? 0x820f9000u : 0x820e9000u) + (o))
#define MT_WF_MIB(b, o)			(((b) ? 0x820fd000u : 0x820ed000u) + (o))

#define MT792X_BASIC_RATES_TBL		11

/* mt7925_mac_wtbl_update() with MT_WTBL_UPDATE_ADM_COUNT_CLEAR */
void mt7925_mac_wtbl_clear(uint16_t idx)
{
	uint32_t i;

	rmw(MT_WTBL_UPDATE, MT_WTBL_UPDATE_WLAN_IDX,
	    idx | MT_WTBL_UPDATE_ADM_COUNT_CLEAR);
	for (i = 0; i < 5000; i++) {
		if (!(anx_mt7925_rr(MT_WTBL_UPDATE) & MT_WTBL_UPDATE_BUSY))
			return;
		anx_delay_ms(1);
	}
	kprintf("mt7925: WTBL %u update stayed busy\n", idx);
}

/* mt792x_mac_init_band() */
static void mac_init_band(uint8_t band)
{
	rmw(MT_WF_TMAC(band, 0x0f4), 0x3f, 0x3f);
	set_bits(MT_WF_TMAC(band, 0x0f4), (1U << 17) | (1U << 18));

	set_bits(MT_WF_RMAC(band, 0x3c4), 1U << 30);
	set_bits(MT_WF_RMAC(band, 0x380), 1U << 30);

	set_bits(MT_WF_MIB(band, 0x004), (1U << 8) | (1U << 9));

	rmw(MT_WF_DMA(band, 0x000), MT_MAX_RX_LEN_MASK, MT_MAX_RX_LEN(1536));
	rmw(MT_WF_DMA(band, 0x000), 1U << 23, 0);	/* RXD_G5_EN off */

	rmw(MT_WTBLOFF(band, 0x008), (3U << 30) | (3U << 24), 3U << 24);
}

/* mt7925_mac_set_fixed_rate_table() */
static void set_fixed_rate_table(uint8_t tbl_idx, uint16_t rate)
{
	anx_mt7925_wr(MT_WTBL_ITDR0, rate);
	anx_mt7925_wr(MT_WTBL_ITDR1, MT_WTBL_SPE_IDX_SEL);
	anx_mt7925_wr(MT_WTBL_ITCR, MT_WTBL_ITCR_WR | MT_WTBL_ITCR_EXEC |
				    tbl_idx);
}

static void mac_init(void)
{
	uint32_t i;

	rmw(MT_MDP_DCR1, MT_MAX_RX_LEN_MASK, MT_MAX_RX_LEN(1536));
	set_bits(MT_MDP_DCR0, MT_MDP_DCR0_DAMSDU_EN);

	for (i = 0; i < MT7925_WTBL_SIZE; i++)
		mt7925_mac_wtbl_clear((uint16_t)i);
	mac_init_band(0);
	mac_init_band(1);

	for (i = 0; i < MT7925_RATES_2G; i++)
		set_fixed_rate_table((uint8_t)(MT792X_BASIC_RATES_TBL + i),
				     mt7925_ieee_rate_table_value(i));
}

/* ------------------------------------------------------------------ */
/* TX: token-owned frame buffers                                        */
/* ------------------------------------------------------------------ */

/*
 * Each token owns one DMA-reachable buffer holding the TXWI and, after it,
 * the frame. The firmware returns tokens in TXRX_NOTIFY reports; until
 * then the buffer must stay untouched, because the device reads the frame
 * through the TXP long after the descriptor was taken.
 */
#define TX_POOL			64
#define TX_BUF_SIZE		2048
#define TX_POOL_ORDER		5		/* 32 pages = 64 * 2 KiB */
#define TX_FRAME_MAX		(TX_BUF_SIZE - MT_TXWI_SIZE)
#define PACKET_ID_NO_SKB	1

enum tx_kind { TX_MGMT, TX_EAPOL, TX_ETH };

static uint8_t *g_tx_bufs;
static bool g_tx_busy[TX_POOL];
static uint8_t g_tx_kind[TX_POOL];
static uint32_t g_tx_stamp[TX_POOL];
static uint32_t g_tx_clock;

/*
 * A lost EAPOL frame stalls the port on the AP side while this side
 * believes it is connected, so its fate is always logged.
 */
static void tx_release(uint16_t token, uint8_t stat, void *arg)
{
	struct mt7925_dev *dev = arg;

	if (token >= TX_POOL || !g_tx_busy[token])
		return;
	g_tx_busy[token] = false;
	if (stat)
		dev->tx_failed++;
	else
		dev->tx_acked++;
	if (g_tx_kind[token] == TX_EAPOL)
		kprintf("mt7925: EAPOL frame %s (status %u)\n",
			stat ? "not acknowledged" : "delivered", stat);
	else if (g_tx_kind[token] == TX_ETH && dev->trace_budget) {
		dev->trace_budget--;
		kprintf("mt7925: data frame %s (status %u)\n",
			stat ? "not acknowledged" : "delivered", stat);
	}
}

static int tx_alloc(struct mt7925_dev *dev)
{
	uint32_t i, oldest = 0, pass;

	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < TX_POOL; i++) {
			if (!g_tx_busy[i]) {
				g_tx_busy[i] = true;
				g_tx_stamp[i] = ++g_tx_clock;
				return (int)i;
			}
		}
		mt7925_service(dev);
	}

	/*
	 * No report has come back for 64 frames. Take the oldest buffer; if
	 * the device still reads it the frame it sends is garbled, which is
	 * better than wedging the interface. The counter shows it happened.
	 */
	for (i = 1; i < TX_POOL; i++)
		if (g_tx_stamp[i] < g_tx_stamp[oldest])
			oldest = i;
	dev->tx_reclaimed++;
	g_tx_stamp[oldest] = ++g_tx_clock;
	return (int)oldest;
}

static int tx_frame(struct mt7925_dev *dev, const uint8_t *frame,
		    uint32_t len, const struct mt7925_txd_params *p,
		    enum tx_kind kind)
{
	uint8_t *buf;
	uint32_t i;
	int token, ret;

	if (!g_tx_bufs || len > TX_FRAME_MAX)
		return ANX_EINVAL;

	token = tx_alloc(dev);
	g_tx_kind[token] = (uint8_t)kind;
	buf = g_tx_bufs + (uint32_t)token * TX_BUF_SIZE;
	anx_memcpy(buf + MT_TXWI_SIZE, frame, len);
	mt7925_mac_write_txwi(buf, buf + MT_TXWI_SIZE, len,
			      (uint32_t)(uintptr_t)(buf + MT_TXWI_SIZE),
			      (uint16_t)token, p);

	for (i = 0; i < 50; i++) {
		ret = mt7925_data_tx_push((uint32_t)(uintptr_t)buf,
					  MT_TXWI_SIZE);
		if (ret != ANX_EBUSY)
			break;
		mt7925_service(dev);
		anx_delay_ms(1);
	}
	if (ret) {
		g_tx_busy[token] = false;
		dev->tx_dropped++;
		return ret;
	}
	dev->tx_frames++;
	return ANX_OK;
}

/*
 * mt76_tx() sends management frames through the ALTX queue unless
 * ieee80211_is_bufferable_mmpdu() says the frame may wait for a sleeping
 * peer (disassoc, deauth, action); those take the best-effort queue.
 */
static uint8_t mgmt_queue(const uint8_t *frame)
{
	uint8_t stype = frame[0] & 0xf0;

	if (stype == 0xa0 || stype == 0xc0 || stype == 0xd0)
		return (uint8_t)(MT_LMAC_AC00 + mt7925_mac_lmac_queue(MT_AC_BE));
	return MT_LMAC_ALTX0;
}

int mt7925_tx_mgmt(struct mt7925_dev *dev, const uint8_t *frame,
		   uint32_t len, uint16_t wlan_idx)
{
	struct mt7925_txd_params p = {
		.wlan_idx = wlan_idx,
		.omac_idx = MT7925_OMAC_IDX,
		.band_idx = dev->band_idx,
		.q_idx = mgmt_queue(frame),
		.is_8023 = false,
		.mcast = (frame[4] & 0x01) != 0,
		.rate_idx = dev->rates.basic_rate_idx ?
			    dev->rates.basic_rate_idx : MT792X_BASIC_RATES_TBL,
		.pid = PACKET_ID_NO_SKB,
	};

	return tx_frame(dev, frame, len, &p, TX_MGMT);
}

/*
 * EAPOL goes out as an 802.11 data frame at the lowest basic rate, never
 * through header translation: mac80211 skips its 802.3 path for the
 * control port protocol (ieee80211_subif_start_xmit_8023()).
 */
int mt7925_tx_eapol(struct mt7925_dev *dev, const uint8_t *payload,
		    uint32_t len, bool protect)
{
	static uint8_t frame[MT7925_FRAME_MAX + 64];
	struct mt7925_txd_params p = {
		.wlan_idx = MT7925_WCID_AP,
		.omac_idx = MT7925_OMAC_IDX,
		.band_idx = dev->band_idx,
		.q_idx = (uint8_t)(MT_LMAC_AC00 + mt7925_mac_lmac_queue(MT_AC_BE)),
		.is_8023 = false,
		.protect = protect,
		.min_rate = true,
		.rate_idx = dev->rates.basic_rate_idx,
		.pid = PACKET_ID_NO_SKB,
	};
	uint32_t flen;

	flen = mt7925_ieee_build_data(frame, sizeof(frame), dev->bss.bssid,
				      dev->mac, dev->bss.bssid, 0x888e,
				      payload, len, protect);
	if (!flen)
		return ANX_EINVAL;
	return tx_frame(dev, frame, flen, &p, TX_EAPOL);
}

int mt7925_tx_eth(struct mt7925_dev *dev, const uint8_t *frame, uint32_t len)
{
	struct mt7925_txd_params p = {
		.wlan_idx = MT7925_WCID_AP,
		.omac_idx = MT7925_OMAC_IDX,
		.band_idx = dev->band_idx,
		.q_idx = (uint8_t)(MT_LMAC_AC00 + mt7925_mac_lmac_queue(MT_AC_BE)),
		.is_8023 = true,
		.protect = dev->keyed,
		.pid = PACKET_ID_NO_SKB,
	};

	if (len < 14)
		return ANX_EINVAL;
	if (dev->trace_budget)
		kprintf("mt7925: tx %u bytes type %04x to "
			"%02x:%02x:%02x:%02x:%02x:%02x\n", len,
			(frame[12] << 8) | frame[13], frame[0], frame[1],
			frame[2], frame[3], frame[4], frame[5]);
	return tx_frame(dev, frame, len, &p, TX_ETH);
}

/* ------------------------------------------------------------------ */
/* RX dispatch                                                          */
/* ------------------------------------------------------------------ */

static void queue_push(struct mt7925_dev *dev, const uint8_t *data,
		       uint32_t len, bool is_eapol)
{
	struct mt7925_frame *f;
	uint32_t next = (dev->q_head + 1) % MT7925_STA_QUEUE;

	if (len > MT7925_FRAME_MAX || next == dev->q_tail) {
		dev->rx_dropped++;
		return;
	}
	f = &dev->queue[dev->q_head];
	anx_memcpy(f->data, data, len);
	f->len = (uint16_t)len;
	f->is_eapol = is_eapol;
	dev->q_head = next;
}

bool mt7925_queue_pop(struct mt7925_dev *dev, struct mt7925_frame *out)
{
	if (dev->q_head == dev->q_tail)
		return false;
	*out = dev->queue[dev->q_tail];
	dev->q_tail = (dev->q_tail + 1) % MT7925_STA_QUEUE;
	return true;
}

static bool port_open(const struct mt7925_dev *dev)
{
	return dev->state >= MT7925_STATE_CONNECTED;
}

static void rx_ethernet(struct mt7925_dev *dev, const uint8_t *eth,
			uint32_t len)
{
	uint16_t type;

	if (len < 14)
		return;
	type = (uint16_t)((eth[12] << 8) | eth[13]);
	if (type == 0x888e) {
		queue_push(dev, eth + 14, len - 14, true);
		return;
	}
	if (!port_open(dev)) {
		dev->rx_port_closed++;
		dev->rx_dropped++;
		return;
	}
	dev->rx_data++;
	anx_eth_recv(eth, len);
}

static void rx_80211(struct mt7925_dev *dev, const struct mt7925_rx_info *ri,
		     const uint8_t *frame, uint32_t len)
{
	uint16_t fc = (uint16_t)(frame[0] | (frame[1] << 8));
	uint16_t stype = fc & IEEE80211_FCTL_STYPE;

	if ((fc & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_MGMT) {
		switch (stype) {
		case IEEE80211_STYPE_BEACON:
		case IEEE80211_STYPE_PROBE_RESP:
			if (dev->scan_active) {
				struct mt7925_bss bss;

				if (mt7925_ieee_parse_beacon(frame, len,
							     &bss) != 0)
					return;
				if (!bss.channel)
					bss.channel = ri->channel;
				bss.band = ri->band;
				bss.rssi = ri->has_rssi ? ri->rssi : 0;
				mt7925_sta_scan_result(dev, frame, len, &bss);
			}
			return;
		case IEEE80211_STYPE_AUTH:
		case IEEE80211_STYPE_ASSOC_RESP:
		case IEEE80211_STYPE_REASSOC_RESP:
		case IEEE80211_STYPE_DEAUTH:
		case IEEE80211_STYPE_DISASSOC:
			queue_push(dev, frame, len, false);
			return;
		default:
			return;
		}
	}

	if ((fc & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_DATA) {
		static uint8_t eth[MT7925_FRAME_MAX + 32];
		uint32_t elen;

		if ((fc & IEEE80211_FCTL_PROTECTED) && !ri->decrypted) {
			dev->rx_undecrypted++;
			dev->rx_dropped++;
			return;
		}
		elen = mt7925_ieee_data_to_eth(frame, len, eth, sizeof(eth));
		if (elen)
			rx_ethernet(dev, eth, elen);
	}
}

static void handle_rx(struct mt7925_dev *dev, const uint8_t *buf,
		      uint32_t len)
{
	struct mt7925_rx_info ri;

	if (mt7925_mac_parse_rx(buf, len, &ri) != 0) {
		dev->rx_dropped++;
		return;
	}

	switch (ri.pkt_type) {
	case MT_PKT_TYPE_TXRX_NOTIFY:
		mt7925_mac_parse_tx_free(buf, len, tx_release, dev);
		return;
	case MT_PKT_TYPE_RX_EVENT:
		mt7925_mcu_rx_event(dev, buf, len);
		return;
	case MT_PKT_TYPE_NORMAL:
	case MT_PKT_TYPE_NORMAL_MCU:
		break;
	default:
		return;
	}

	dev->rx_frames++;
	if (dev->trace_budget && port_open(dev) && ri.payload_len >= 14 &&
	    (ri.hdr_trans || (buf[ri.payload_off] & 0x0c) == 0x08)) {
		const uint8_t *d = buf + ri.payload_off;

		dev->trace_budget--;
		kprintf("mt7925: rx %u bytes wcid %u %s%s sec %u%s%s "
			"%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x\n", ri.payload_len, ri.wlan_idx,
			ri.hdr_trans ? "802.3" : "802.11",
			ri.unicast ? "" : " group", ri.sec_mode,
			ri.decrypted ? " dec" : "",
			ri.fcs_err || ri.icv_err ? " ERR" : "",
			d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
			d[8], d[9], d[10], d[11], d[12], d[13]);
	}
	if (ri.fcs_err || ri.icv_err) {
		dev->rx_dropped++;
		return;
	}
	if (ri.hdr_trans)
		rx_ethernet(dev, buf + ri.payload_off, ri.payload_len);
	else if (ri.payload_len >= IEEE80211_HDRLEN)
		rx_80211(dev, &ri, buf + ri.payload_off, ri.payload_len);
}

/*
 * Received data goes up the network stack, which may transmit, and a
 * transmit that finds no free token services the rings. Servicing does
 * not nest: a received buffer stays valid only until the next poll of its
 * ring, and rx_80211() converts into a static buffer.
 */
void mt7925_service(struct mt7925_dev *dev)
{
	static bool active;
	const uint8_t *buf;
	uint32_t len, n;

	if (!dev->bar0 || active)
		return;
	active = true;
	for (n = 0; n < 64 && (buf = mt7925_evt_poll(&len)) != NULL; n++)
		handle_rx(dev, buf, len);
	for (n = 0; n < 64 && (buf = mt7925_data_rx_poll(&len)) != NULL; n++)
		handle_rx(dev, buf, len);
	active = false;
}

/*
 * anx_delay_ms() assumes the fastest plausible TSC, so on jekyll's 2 GHz
 * clock a "millisecond" lasts three. Timeouts here follow the 100 Hz tick
 * when it runs, and fall back to counting 1 ms delays when it does not.
 */
void mt7925_timer_start(struct mt7925_timer *t, uint32_t ms)
{
	t->tick0 = arch_timer_ticks();
	t->ms = ms;
	t->iter = 0;
}

bool mt7925_timer_expired(struct mt7925_timer *t)
{
	uint64_t now = arch_timer_ticks();

	if (now != t->tick0) {
		if ((now - t->tick0) * 10 >= t->ms)
			return true;
	} else if (t->iter >= t->ms) {
		return true;
	}
	t->iter++;
	anx_delay_ms(1);
	return false;
}

bool mt7925_wait_flag(struct mt7925_dev *dev, const bool *flag, uint32_t ms)
{
	struct mt7925_timer t;

	mt7925_timer_start(&t, ms);
	do {
		mt7925_service(dev);
		if (*flag)
			return true;
	} while (!mt7925_timer_expired(&t));
	mt7925_service(dev);
	return *flag;
}

/* ------------------------------------------------------------------ */
/* Power ownership                                                      */
/* ------------------------------------------------------------------ */

/*
 * Power ownership handshake, two phases.
 *
 * Linux hands ownership to firmware and then takes it back
 * (mt7925_pci_probe() calls __mt792x_mcu_fw_pmctrl() and only then
 * __mt792xe_mcu_drv_pmctrl()). Doing only the second half is not enough: if
 * OWN_SYNC already reads clear, clearing it again is a no-op and the domain
 * never actually transitions, so the driver reports success while the chip
 * stays asleep and every register still reads the 0xdeadbeef power-off
 * sentinel.
 */
static int lpctl_phase(uint32_t write_bit, uint32_t want_sync, const char *what)
{
	uint32_t i, j, v = 0;

	for (i = 0; i < MT7925_DRV_OWN_RETRIES; i++) {
		anx_mt7925_wr(MT_CONN_ON_LPCTL, write_bit);

		for (j = 0; j < MT7925_DRV_OWN_POLL_MS; j++) {
			v = anx_mt7925_rr(MT_CONN_ON_LPCTL);
			if ((v & PCIE_LPCR_HOST_OWN_SYNC) == want_sync)
				return ANX_OK;
			anx_delay_ms(1);
		}
	}
	kprintf("mt7925: %s failed, LPCTL=0x%08x\n", what, v);
	return ANX_EIO;
}

static int drv_own(void)
{
	int ret;

	ret = lpctl_phase(PCIE_LPCR_HOST_SET_OWN, PCIE_LPCR_HOST_OWN_SYNC,
			  "firmware own");
	if (ret != ANX_OK)
		return ret;
	return lpctl_phase(PCIE_LPCR_HOST_CLR_OWN, 0, "driver own");
}

/* mt792x_wfsys_reset() */
static int wfsys_reset(void)
{
	uint32_t v, i;

	v = anx_mt7925_rr(MT_WFSYS_SW_RST_B);
	anx_mt7925_wr(MT_WFSYS_SW_RST_B, v & ~WFSYS_SW_RST_B);
	anx_delay_ms(50);
	v = anx_mt7925_rr(MT_WFSYS_SW_RST_B);
	anx_mt7925_wr(MT_WFSYS_SW_RST_B, v | WFSYS_SW_RST_B);

	for (i = 0; i < 500; i++) {
		v = anx_mt7925_rr(MT_WFSYS_SW_RST_B);
		if (v & WFSYS_SW_INIT_DONE)
			return ANX_OK;
		anx_delay_ms(1);
	}
	kprintf("mt7925: wfsys reset timed out (reg=0x%08x)\n", v);
	return ANX_ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                             */
/* ------------------------------------------------------------------ */

static int probe_hw(struct mt7925_dev *dev, struct anx_pci_device *pci)
{
	int was;

	/*
	 * Map the BAR as device memory: the boot identity map is write-back
	 * cached, and a cached register mapping answers reads from stale
	 * lines and holds writes in cache.
	 */
	dev->bar0 = anx_mmio_map((uint64_t)(pci->bar[0] & ~0xfu), 0x200000);
	if (!dev->bar0) {
		kprintf("mt7925: could not map BAR0\n");
		return ANX_ENOMEM;
	}

	was = anx_pci_power_on(pci);
	if (was > 0)
		kprintf("mt7925: device was in D%d, moved to D0\n", was);
	anx_pci_enable_bus_master(pci);

	if (drv_own() != ANX_OK) {
		kprintf("mt7925: chip did not wake\n");
		return ANX_EIO;
	}

	kprintf("mt7925: chip id=0x%08x rev=0x%08x\n",
		anx_mt7925_rr(MT_HW_CHIPID), anx_mt7925_rr(MT_HW_REV));

	/* mt7925_pci_probe(): sleep protection, WFSYS reset, IRQs masked */
	set_bits(MT_HW_EMI_CTL, MT_HW_EMI_CTL_SLPPROT_EN);
	if (wfsys_reset() != ANX_OK)
		return ANX_ETIMEDOUT;
	anx_mt7925_bar_wr(MT_WFDMA0_HOST_INT_ENA, 0);
	anx_mt7925_bar_wr(MT_PCIE_MAC_INT_ENABLE, 0xff);

	/* mt7925e_mcu_init(): ownership again, then disable L0s. */
	if (drv_own() != ANX_OK)
		return ANX_EIO;
	set_bits(MT_PCIE_MAC_PM, MT_PCIE_MAC_PM_L0S_DIS);
	return ANX_OK;
}

static void default_edca(struct mt7925_edca_params ac[4], bool use_11b)
{
	uint32_t i;

	/* ieee80211_set_wmm_default() for a non-QoS station */
	for (i = 0; i < 4; i++) {
		ac[i].aifs = 2;
		ac[i].cw_min = use_11b ? 31 : 15;
		ac[i].cw_max = 1023;
		ac[i].txop = 0;
	}
}

int mt7925_bring_up(struct mt7925_dev *dev)
{
	struct mt7925_edca_params ac[4];
	int ret;

	if (!g_tx_bufs) {
		uintptr_t pa = anx_page_alloc(TX_POOL_ORDER);

		if (!pa)
			return ANX_ENOMEM;
		g_tx_bufs = (uint8_t *)pa;
	}

	ret = mt7925_fw_download(dev);
	if (ret) {
		kprintf("mt7925: firmware download failed (%d)\n", ret);
		return ret;
	}

	/* mt7925_run_firmware(), after mt792x_load_firmware() */
	ret = mt7925_mcu_run_firmware(dev);
	if (ret)
		return ret;

	/* __mt7925_init_hardware() */
	ret = mt7925_mcu_set_eeprom();
	if (ret)
		kprintf("mt7925: eeprom mode failed (%d)\n", ret);
	mac_init();

	/*
	 * mt7925_init_work(). Deep sleep stays off: it relies on the
	 * interrupt-driven ownership hand-off this polled driver lacks.
	 */
	mt7925_mcu_chip_config("ThermalProtGband 0 100 90 80 30 1 1 115 105 5");
	mt7925_mcu_chip_config("ThermalProtAband 1 100 90 80 30 1 1 115 105 5");
	mt7925_mcu_chip_config("KeepFullPwr 1");

	/* __mt7925_start() */
	ret = mt7925_mcu_set_channel_domain(dev);
	if (ret)
		return ret;
	ret = mt7925_mcu_set_rts_thresh(MT7925_RTS_THRESHOLD);
	if (ret)
		return ret;

	/* mt7925_mcu_regd_update(): CLC, channel domain, power table */
	ret = mt7925_mcu_set_clc(dev, "US");
	if (ret)
		kprintf("mt7925: CLC update failed (%d)\n", ret);
	ret = mt7925_mcu_set_channel_domain(dev);
	if (ret)
		return ret;
	ret = mt7925_mcu_set_rate_txpower(dev);
	if (ret)
		return ret;

	/* mt7925_add_interface() */
	mt7925_mac_wtbl_clear(MT7925_WCID_BSS);
	ret = mt7925_mcu_add_dev(dev, true);
	if (ret) {
		kprintf("mt7925: interface add failed (%d)\n", ret);
		return ret;
	}

	/* mt7925_configure_filter() with no FIF_* flags */
	ret = mt7925_mcu_set_rxfilter(1U << 31, 0, 0);
	if (ret)
		return ret;

	/* ieee80211_do_open(): default EDCA and a long slot */
	default_edca(ac, false);
	mt7925_mcu_set_tx(ac);
	mt7925_mcu_set_timing(20);
	return ANX_OK;
}

int anx_mt7925_init(void)
{
	struct mt7925_dev *dev = &g_mt7925;
	struct anx_pci_device *pci;
	int ret;

	anx_memset(dev, 0, sizeof(*dev));
	dev->band_idx = 0xff;
	g_ready = false;

	pci = anx_pci_find_device(MT7925_VENDOR_ID, MT7925_DEVICE_ID);
	if (!pci)
		return ANX_ENODEV;
	kprintf("mt7925: found at %02x:%02x.%x BAR0=0x%x\n",
		pci->bus, pci->slot, pci->func, pci->bar[0] & ~0xf);

	ret = probe_hw(dev, pci);
	if (ret)
		return ret;

	ret = mt7925_bring_up(dev);
	if (ret) {
		kprintf("mt7925: bring-up failed (%d)\n", ret);
		return ret;
	}

	dev->state = MT7925_STATE_FW_UP;
	g_ready = true;
	kprintf("mt7925: ready\n");
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

bool anx_mt7925_ready(void)
{
	return g_ready && g_mt7925.state >= MT7925_STATE_CONNECTED;
}

int anx_mt7925_tx(const void *frame, uint16_t len)
{
	if (!anx_mt7925_ready())
		return ANX_EIO;
	return mt7925_tx_eth(&g_mt7925, frame, len);
}

void anx_mt7925_poll(void)
{
	if (!g_ready)
		return;
	mt7925_service(&g_mt7925);
	mt7925_sta_poll(&g_mt7925);
}

const uint8_t *anx_mt7925_mac(void)
{
	return g_mt7925.mac;
}

int anx_mt7925_connect(const char *ssid, const char *psk)
{
	if (!g_ready)
		return ANX_EIO;
	return mt7925_sta_connect(&g_mt7925, ssid, psk);
}

void anx_mt7925_disconnect(void)
{
	if (!g_ready)
		return;
	mt7925_sta_disconnect(&g_mt7925, 3 /* deauth leaving */);
}

int anx_mt7925_scan(void)
{
	if (!g_ready)
		return ANX_EIO;
	return mt7925_sta_scan_print(&g_mt7925);
}

anx_mt7925_state_t anx_mt7925_state(void)
{
	return g_mt7925.state;
}

void anx_mt7925_info(void)
{
	static const char * const state_names[] = {
		"down", "fw_up", "scanning", "assoc", "connected"
	};
	const struct mt7925_dev *dev = &g_mt7925;
	uint32_t s = (uint32_t)dev->state;

	kprintf("mt7925: state=%s\n", s < 5 ? state_names[s] : "?");
	if (!g_ready)
		return;
	kprintf("mt7925: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
		dev->mac[0], dev->mac[1], dev->mac[2],
		dev->mac[3], dev->mac[4], dev->mac[5]);
	if (dev->state >= MT7925_STATE_ASSOC)
		kprintf("mt7925: SSID \"%s\" BSSID %02x:%02x:%02x:%02x:%02x:%02x "
			"channel %u aid %u rssi %d\n", dev->ssid,
			dev->bss.bssid[0], dev->bss.bssid[1],
			dev->bss.bssid[2], dev->bss.bssid[3],
			dev->bss.bssid[4], dev->bss.bssid[5],
			dev->bss.channel, dev->aid, dev->bss.rssi);
	kprintf("mt7925: rx %u (data %u, dropped %u) tx %u (dropped %u, "
		"reclaimed %u)%s\n", dev->rx_frames, dev->rx_data,
		dev->rx_dropped, dev->tx_frames, dev->tx_dropped,
		dev->tx_reclaimed, dev->fw_assert ? " FIRMWARE ASSERTED" : "");
	kprintf("mt7925: tx acked %u failed %u; rx undecrypted %u, "
		"before authorization %u\n", dev->tx_acked, dev->tx_failed,
		dev->rx_undecrypted, dev->rx_port_closed);
}

/* Default (no-op) implementations of the state-change hooks. */
void mt7925_on_connect(const char *ssid)    __attribute__((weak));
void mt7925_on_connect(const char *ssid)    { (void)ssid; }

void mt7925_on_disconnect(void)             __attribute__((weak));
void mt7925_on_disconnect(void)             { }
