/*
 * mt7925_mcu.c — MT7925 unified-command transport and command wrappers.
 *
 * After the WM firmware starts, every configuration step is a unified
 * command on the WM ring, answered (when asked) by an event carrying the
 * same 4-bit sequence number. Events flagged unsolicited — scan done,
 * channel grant, beacon loss, firmware log — arrive on their own and are
 * handled here too.
 *
 * Sources, Linux v6.19 drivers/net/wireless/mediatek/mt76:
 *   mt7925/mcu.c, mt7925/pci_mcu.c, mt7925/main.c, mt7925/regd.c,
 *   mt76_connac_mcu.c, mt76_mcu.c.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/delay.h>
#include "mt7925_reg.h"
#include "mt7925_drv.h"

#define MCU_TIMEOUT_MS		3000	/* mt7925_mcu_send_message() */
#define FW_LOG_LINES_MAX	400

/* CLC blobs (mt792x.h, mt7925/mt7925.h, mt76_connac_mcu.h) */
#define CLC_POWER		0
#define CLC_POWER_EXT		1
#define CLC_BE_CTRL		2
#define FW_FEATURE_NON_DL	(1U << 6)
#define FW_TYPE_CLC		2
#define FW_TRAILER_SIZE		36
#define FW_REGION_SIZE		40
#define EE_HW_TYPE		0xa71
#define ENVIRON_ANY		0
#define ENVIRON_INDOOR		1

#define FIF_BIT_SET		(1U << 0)
#define FIF_BIT_CLR		(1U << 1)
#define WF_RFCR_DROP_OTHER_BEACON (1U << 11)

extern const uint8_t mt7925_ram_fw[];
extern const uint32_t mt7925_ram_fw_size;

static const char g_alpha2[2] = { 'U', 'S' };

/* ------------------------------------------------------------------ */
/* Transport                                                            */
/* ------------------------------------------------------------------ */

static struct {
	bool      active;
	bool      done;
	bool      uni_status;
	uint8_t   seq;
	uint16_t  cid;
	int       status;
	uint8_t  *rsp;
	uint32_t  cap;
	uint32_t  len;
} g_wait;

static uint32_t g_stray_events;

bool mt7925_mcu_busy(void)
{
	return g_wait.active;
}

void mt7925_mcu_body(struct mt7925_msg *msg)
{
	uint32_t cap;
	uint8_t *buf = mt7925_cmd_buf(&cap);

	if (!buf || cap < sizeof(struct mt7925_uni_txd)) {
		mt7925_msg_init(msg, NULL, 0);
		msg->overflow = true;
		return;
	}
	mt7925_msg_init(msg, buf + sizeof(struct mt7925_uni_txd),
			cap - sizeof(struct mt7925_uni_txd));
}

/* mt7925_mcu_parse_response(): which commands answer with cid+status */
static bool has_uni_status(uint16_t cid)
{
	return cid == MT_UNI_CMD_DEV_INFO_UPDATE ||
	       cid == MT_UNI_CMD_BSS_INFO_UPDATE ||
	       cid == MT_UNI_CMD_STA_REC_UPDATE ||
	       cid == 0x05 /* SUSPEND */ || cid == 0x06 /* OFFLOAD */;
}

int mt7925_mcu_send(uint16_t cid, uint8_t option, struct mt7925_msg *msg,
		    bool wait, uint8_t *rsp, uint32_t rsp_cap,
		    uint32_t *rsp_len)
{
	struct mt7925_uni_txd *txd;
	uint8_t *buf = mt7925_cmd_buf(NULL);
	uint8_t seq;
	int ret;

	if (!buf || msg->overflow) {
		kprintf("mt7925: cmd 0x%02x body does not fit\n", cid);
		return ANX_ENOMEM;
	}
	if (g_wait.active) {
		kprintf("mt7925: cmd 0x%02x while 0x%02x pending\n",
			cid, g_wait.cid);
		return ANX_EBUSY;
	}

	seq = mt7925_mcu_next_seq();
	txd = (struct mt7925_uni_txd *)buf;
	mt7925_uni_fill_txd(txd, cid, option, seq, msg->len);

	if (wait) {
		g_wait.active = true;
		g_wait.done = false;
		g_wait.seq = seq;
		g_wait.cid = cid;
		g_wait.uni_status = has_uni_status(cid);
		g_wait.status = 0;
		g_wait.rsp = rsp;
		g_wait.cap = rsp_cap;
		g_wait.len = 0;
	}

	ret = mt7925_wm_send(sizeof(*txd) + msg->len);
	if (ret) {
		g_wait.active = false;
		kprintf("mt7925: cmd 0x%02x seq %u not taken (%d)\n",
			cid, seq, ret);
		return ret;
	}
	if (!wait)
		return ANX_OK;

	mt7925_wait_flag(&g_mt7925, &g_wait.done, MCU_TIMEOUT_MS);
	g_wait.active = false;
	if (!g_wait.done) {
		kprintf("mt7925: cmd 0x%02x seq %u: no response\n", cid, seq);
		return ANX_ETIMEDOUT;
	}
	if (rsp_len)
		*rsp_len = g_wait.len;
	if (g_wait.status)
		kprintf("mt7925: cmd 0x%02x status %d\n", cid, g_wait.status);
	return g_wait.status;
}

static int send_set(uint16_t cid, struct mt7925_msg *msg)
{
	return mt7925_mcu_send(cid, MT_UNI_OPT_SET_ACK, msg, true,
			       NULL, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Events                                                               */
/* ------------------------------------------------------------------ */

static void print_log_text(const uint8_t *s, uint32_t len)
{
	char line[160];
	uint32_t i, n = 0;

	for (i = 0; i < len && n < sizeof(line) - 1; i++) {
		char c = (char)s[i];

		if (c == '\0')
			break;
		if (c == '\r' || c == '\n')
			continue;
		line[n++] = (c >= 0x20 && c < 0x7f) ? c : '.';
	}
	line[n] = '\0';
	if (n)
		kprintf("mt7925 fw: %s\n", line);
}

/* mt7925_mcu_uni_debug_msg_event() */
static void event_fw_log(struct mt7925_dev *dev, const uint8_t *b,
			 uint32_t len)
{
	uint8_t id, type;

	if (len < 16)
		return;
	if (++dev->fw_log_lines > FW_LOG_LINES_MAX) {
		if (dev->fw_log_lines == FW_LOG_LINES_MAX + 1)
			kprintf("mt7925: further firmware log suppressed\n");
		return;
	}

	id = b[8];
	type = b[9] & 0x7;
	if (id == 0x28) {
		print_log_text(b + 8, len - 8);
	} else if (id == 0xa8 && type == 2) {
		uint32_t tlen = b[10];

		if (16 + tlen > len)
			tlen = len - 16;
		print_log_text(b + 16, tlen);
	} else if (id == 0xa8 && type == 0) {
		uint32_t i, n = (len - 16) / 4;

		kprintf("mt7925 fw: idx 0x%x:",
			(uint32_t)(b[12] | (b[13] << 8) | (b[14] << 16) |
				   ((uint32_t)b[15] << 24)));
		for (i = 0; i < n && i < 8; i++) {
			const uint8_t *v = b + 16 + 4 * i;

			kprintf(" 0x%x", (uint32_t)(v[0] | (v[1] << 8) |
						   (v[2] << 16) |
						   ((uint32_t)v[3] << 24)));
		}
		kprintf("\n");
	}
}

static void event_scan_done(struct mt7925_dev *dev, const uint8_t *b,
			    uint32_t len)
{
	const struct mt7925_tlv *t;
	uint32_t pos = 0;

	while ((t = mt7925_tlv_next(b, len, &pos)) != NULL) {
		if (t->tag == MT_UNI_EVENT_SCAN_DONE_BASIC) {
			dev->scan_done = true;
		} else if (t->tag == 2 && t->len >= 8) {
			const uint8_t *d = (const uint8_t *)(t + 1);

			kprintf("mt7925: scan saw country %c%c\n",
				d[1] ? d[1] : '?', d[2] ? d[2] : '?');
		}
	}
}

/* mt7925_mcu_roc_handle_grant() */
static void event_roc(struct mt7925_dev *dev, const uint8_t *b, uint32_t len)
{
	const struct mt7925_tlv *t;
	uint32_t pos = 0;

	while ((t = mt7925_tlv_next(b, len, &pos)) != NULL) {
		const struct mt7925_roc_grant *g;

		if (t->tag != MT_UNI_EVENT_ROC_GRANT ||
		    t->len < sizeof(*g))
			continue;
		g = (const struct mt7925_roc_grant *)t;
		if (g->reqtype == MT_ROC_REQ_JOIN &&
		    g->bss_idx == MT7925_BSS_IDX)
			dev->band_idx = g->dbdcband;
		dev->roc_granted = true;
		kprintf("mt7925: channel %u granted (token %u, status %u, "
			"band %u, %u ms)\n", g->primarychannel, g->tokenid,
			g->status, g->dbdcband, g->max_interval);
	}
}

static void event_beacon_loss(struct mt7925_dev *dev, const uint8_t *b,
			      uint32_t len)
{
	/* struct mt7925_uni_beacon_loss_event: bss_idx, pad, tlv, reason */
	if (len < 9 || b[0] != MT7925_BSS_IDX)
		return;
	dev->beacon_lost = true;
	kprintf("mt7925: beacon loss (reason %u)\n", b[8]);
}

static void event_unsolicited(struct mt7925_dev *dev, uint8_t eid,
			      const uint8_t *body, uint32_t len)
{
	/* Most unified events carry a 4-byte header before their TLVs. */
	const uint8_t *tlvs = body + 4;
	uint32_t tlen = len > 4 ? len - 4 : 0;

	switch (eid) {
	case MT_UNI_EVENT_FW_LOG_2_HOST:
		event_fw_log(dev, tlvs, tlen);
		break;
	case MT_UNI_EVENT_SCAN_DONE:
		event_scan_done(dev, tlvs, tlen);
		break;
	case MT_UNI_EVENT_ROC:
		event_roc(dev, tlvs, tlen);
		break;
	case MT_UNI_EVENT_BSS_BEACON_LOSS:
		event_beacon_loss(dev, body, len);
		break;
	case MT_UNI_EVENT_COREDUMP:
		if (!dev->fw_assert) {
			dev->fw_assert = true;
			kprintf("mt7925: FIRMWARE ASSERT\n");
			print_log_text(body, len);
		}
		break;
	default:
		break;
	}
}

/* mt7925_mcu_rx_event() and mt76_mcu_get_response() */
void mt7925_mcu_rx_event(struct mt7925_dev *dev, const uint8_t *buf,
			 uint32_t len)
{
	const struct mt7925_mcu_rxd *rxd = (const struct mt7925_mcu_rxd *)buf;
	const uint8_t *body = buf + sizeof(*rxd);
	uint32_t blen;

	if (len < sizeof(*rxd))
		return;
	blen = len - sizeof(*rxd);

	if (rxd->option & MT_UNI_EVENT_OPT_UNSOLICITED) {
		event_unsolicited(dev, rxd->eid, body, blen);
		return;
	}

	if (!g_wait.active || g_wait.done || rxd->seq != g_wait.seq) {
		g_stray_events++;
		return;
	}

	if (g_wait.uni_status) {
		const struct mt7925_uni_event *ev;

		if (blen < sizeof(*ev))
			return;
		ev = (const struct mt7925_uni_event *)body;
		if (ev->cid != (uint8_t)g_wait.cid)
			return;
		g_wait.status = (int)ev->status;
	}

	if (g_wait.rsp) {
		uint32_t n = blen < g_wait.cap ? blen : g_wait.cap;

		anx_memcpy(g_wait.rsp, body, n);
		g_wait.len = n;
	}
	g_wait.done = true;
}

/* ------------------------------------------------------------------ */
/* Firmware start-up (mt7925_run_firmware)                              */
/* ------------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* mt7925_mcu_parse_phy_cap() */
static void parse_phy_cap(struct mt7925_dev *dev, const uint8_t *d)
{
	dev->nss = d[4];
	dev->has_2g = (d[10] & (1U << 0)) != 0;
	dev->has_5g = (d[10] & (1U << 1)) != 0;
}

/* mt7925_mcu_get_nic_capability() */
static int get_nic_capability(struct mt7925_dev *dev)
{
	static uint8_t rsp[1024];
	const struct mt7925_nic_cap_hdr *hdr;
	struct mt7925_msg m;
	uint32_t len, pos, i;
	int ret;

	/*
	 * Linux sends this as MCU_UNI_CMD(CHIP_CONFIG), not as a query, so the
	 * descriptor goes out as a set with the ack bit stripped -- and the
	 * firmware answers it anyway.
	 */
	mt7925_mcu_body(&m);
	mt7925_build_nic_cap(&m);
	ret = mt7925_mcu_send(MT_UNI_CMD_CHIP_CONFIG, MT_UNI_OPT_SET_ACK,
			      &m, true, rsp, sizeof(rsp), &len);
	if (ret)
		return ret;
	if (len < sizeof(*hdr))
		return ANX_EINVAL;

	hdr = (const struct mt7925_nic_cap_hdr *)rsp;
	pos = sizeof(*hdr);
	for (i = 0; i < hdr->n_element; i++) {
		const struct mt7925_tlv *t = mt7925_tlv_next(rsp, len, &pos);
		const uint8_t *d;

		if (!t)
			break;
		d = (const uint8_t *)(t + 1);
		switch (t->tag) {
		case MT_NIC_CAP_MAC_ADDR:
			if (t->len >= 4 + 6)
				anx_memcpy(dev->mac, d, 6);
			break;
		case MT_NIC_CAP_PHY:
			if (t->len >= 4 + 13)
				parse_phy_cap(dev, d);
			break;
		case MT_NIC_CAP_6G:
			if (t->len >= 4 + 1)
				dev->has_6g = d[0] != 0;
			break;
		case MT_NIC_CAP_CHIP_CAP:
			if (t->len >= 4 + 8)
				dev->chip_cap = rd32(d) |
						((uint64_t)rd32(d + 4) << 32);
			break;
		default:
			break;
		}
	}

	kprintf("mt7925: MAC %02x:%02x:%02x:%02x:%02x:%02x, %u streams, "
		"bands %s%s%s, chip cap 0x%08x%08x\n",
		dev->mac[0], dev->mac[1], dev->mac[2],
		dev->mac[3], dev->mac[4], dev->mac[5], dev->nss,
		dev->has_2g ? "2G " : "", dev->has_5g ? "5G " : "",
		dev->has_6g ? "6G" : "",
		(uint32_t)(dev->chip_cap >> 32), (uint32_t)dev->chip_cap);
	return ANX_OK;
}

/* mt7925_mcu_read_eeprom() */
static int read_eeprom(uint32_t offset, uint8_t *val)
{
	uint8_t rsp[128];
	struct mt7925_msg m;
	uint32_t len;
	int ret;

	mt7925_mcu_body(&m);
	mt7925_build_efuse_read(&m, offset);
	ret = mt7925_mcu_send(MT_UNI_CMD_EFUSE_CTRL, MT_UNI_OPT_QUERY_ACK,
			      &m, true, rsp, sizeof(rsp), &len);
	if (ret)
		return ret;
	if (len < MT_EFUSE_READ_EVT_DATA_OFFSET + MT_EEPROM_BLOCK_SIZE)
		return ANX_EINVAL;
	*val = rsp[MT_EFUSE_READ_EVT_DATA_OFFSET +
		   offset % MT_EEPROM_BLOCK_SIZE];
	return ANX_OK;
}

/* mt7925_load_clc(): find the CLC region in the embedded WM image. */
static int load_clc(struct mt7925_dev *dev)
{
	const uint8_t *fw = mt7925_ram_fw;
	uint32_t size = mt7925_ram_fw_size;
	const uint8_t *trailer, *base = NULL;
	uint32_t n_region, i, offset = 0, len = 0;
	uint8_t encap = 0;
	int ret;

	ret = read_eeprom(EE_HW_TYPE, &encap);
	if (ret)
		return ret;
	dev->hw_encap = encap & 0x3;

	trailer = fw + size - FW_TRAILER_SIZE;
	n_region = trailer[2];
	for (i = 0; i < n_region; i++) {
		const uint8_t *reg = trailer - (n_region - i) * FW_REGION_SIZE;

		len = rd32(reg + 20);
		if (offset + len > size)
			return ANX_EINVAL;
		if ((reg[24] & FW_FEATURE_NON_DL) && reg[25] == FW_TYPE_CLC) {
			base = fw + offset;
			break;
		}
		offset += len;
	}
	if (!base) {
		kprintf("mt7925: no CLC in the WM image\n");
		return ANX_OK;
	}

	for (offset = 0; offset + sizeof(struct mt7925_clc_hdr) <= len;) {
		const struct mt7925_clc_hdr *clc =
			(const struct mt7925_clc_hdr *)(base + offset);

		if (clc->len == 0 || clc->idx > CLC_BE_CTRL)
			break;
		if (clc->idx != CLC_BE_CTRL && !dev->clc[clc->idx] &&
		    (clc->t0_type & 0x3) == dev->hw_encap)
			dev->clc[clc->idx] = (const uint8_t *)clc;
		offset += clc->len;
	}

	kprintf("mt7925: CLC power %s, power-ext %s (hw type %u)\n",
		dev->clc[CLC_POWER] ? "found" : "missing",
		dev->clc[CLC_POWER_EXT] ? "found" : "missing", dev->hw_encap);
	return ANX_OK;
}

/* mt7925_mcu_fw_log_2_host() */
static int fw_log_2_host(uint8_t ctrl)
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_fw_log(&m, ctrl);
	return mt7925_mcu_send(MT_UNI_CMD_WSYS_CONFIG, MT_UNI_OPT_SET_ACK,
			       &m, true, NULL, 0, NULL);
}

int mt7925_mcu_run_firmware(struct mt7925_dev *dev)
{
	int ret;

	ret = get_nic_capability(dev);
	if (ret) {
		kprintf("mt7925: NIC capability query failed (%d)\n", ret);
		return ret;
	}
	ret = load_clc(dev);
	if (ret)
		kprintf("mt7925: CLC load failed (%d)\n", ret);
	return fw_log_2_host(1);
}

/* ------------------------------------------------------------------ */
/* Device configuration                                                 */
/* ------------------------------------------------------------------ */

int mt7925_mcu_set_eeprom(void)
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_eeprom_mode(&m);
	return mt7925_mcu_send(MT_UNI_CMD_EFUSE_CTRL, MT_UNI_OPT_SET_ACK,
			       &m, true, NULL, 0, NULL);
}

/* Chip-config commands are sent without waiting (mt7925_mcu_chip_config). */
int mt7925_mcu_chip_config(const char *cmd)
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_chip_config(&m, cmd);
	return mt7925_mcu_send(MT_UNI_CMD_CHIP_CONFIG, MT_UNI_OPT_SET_ACK,
			       &m, false, NULL, 0, NULL);
}

/*
 * The United States channel set, as cfg80211 would hand it over for
 * regulatory domain US: DFS channels are passive (NO_IR | RADAR).
 */
static const struct mt7925_chan_entry g_channels[] = {
	{ MT_BAND_2G, 1, 0 }, { MT_BAND_2G, 2, 0 }, { MT_BAND_2G, 3, 0 },
	{ MT_BAND_2G, 4, 0 }, { MT_BAND_2G, 5, 0 }, { MT_BAND_2G, 6, 0 },
	{ MT_BAND_2G, 7, 0 }, { MT_BAND_2G, 8, 0 }, { MT_BAND_2G, 9, 0 },
	{ MT_BAND_2G, 10, 0 }, { MT_BAND_2G, 11, 0 },
	{ MT_BAND_5G, 36, 0 }, { MT_BAND_5G, 40, 0 },
	{ MT_BAND_5G, 44, 0 }, { MT_BAND_5G, 48, 0 },
#define DFS (MT_CHAN_FLAG_NO_IR | MT_CHAN_FLAG_RADAR)
	{ MT_BAND_5G, 52, DFS }, { MT_BAND_5G, 56, DFS },
	{ MT_BAND_5G, 60, DFS }, { MT_BAND_5G, 64, DFS },
	{ MT_BAND_5G, 100, DFS }, { MT_BAND_5G, 104, DFS },
	{ MT_BAND_5G, 108, DFS }, { MT_BAND_5G, 112, DFS },
	{ MT_BAND_5G, 116, DFS }, { MT_BAND_5G, 120, DFS },
	{ MT_BAND_5G, 124, DFS }, { MT_BAND_5G, 128, DFS },
	{ MT_BAND_5G, 132, DFS }, { MT_BAND_5G, 136, DFS },
	{ MT_BAND_5G, 140, DFS }, { MT_BAND_5G, 144, DFS },
#undef DFS
	{ MT_BAND_5G, 149, 0 }, { MT_BAND_5G, 153, 0 },
	{ MT_BAND_5G, 157, 0 }, { MT_BAND_5G, 161, 0 },
	{ MT_BAND_5G, 165, 0 },
};

#define N_CHANNELS (sizeof(g_channels) / sizeof(g_channels[0]))

int mt7925_mcu_set_channel_domain(struct mt7925_dev *dev)
{
	struct mt7925_msg m;

	(void)dev;
	mt7925_mcu_body(&m);
	mt7925_build_domain(&m, g_alpha2, g_channels, N_CHANNELS);
	return send_set(MT_UNI_CMD_SET_DOMAIN_INFO, &m);
}

int mt7925_mcu_set_rts_thresh(uint32_t val)
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_rts(&m, 0, val);
	return send_set(MT_UNI_CMD_BAND_CONFIG, &m);
}

/*
 * 2 * max_reg_power for regulatory domain US, as mt76_connac_get_ch_power()
 * would compute it; channels the band table does not list keep 127.
 */
static int8_t channel_power(uint8_t band, uint8_t ch)
{
	uint32_t i;

	for (i = 0; i < N_CHANNELS; i++) {
		if (g_channels[i].band != band || g_channels[i].channel != ch)
			continue;
		if (band == MT_BAND_2G)
			return 60;
		if (ch <= 48)
			return 46;
		if (ch <= 144)
			return 48;
		return 60;
	}
	return 127;
}

/* mt7925_mcu_rate_txpower_band() */
static int rate_txpower_band(uint8_t band)
{
	static const uint8_t list_2g[] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14
	};
	static const uint8_t list_5g[] = {
		 36,  38,  40,  42,  44,  46,  48,  50,  52,  54,  56,  58,
		 60,  62,  64, 100, 102, 104, 106, 108, 110, 112, 114, 116,
		118, 120, 122, 124, 126, 128, 132, 134, 136, 138, 140, 142,
		144, 149, 151, 153, 155, 157, 159, 161, 165, 167
	};
	const uint8_t *list = band == MT_BAND_2G ? list_2g : list_5g;
	uint32_t n = band == MT_BAND_2G ? sizeof(list_2g) : sizeof(list_5g);
	uint32_t batches = (n + 2) / 3, i, idx = 0;
	int ret;

	for (i = 0; i < batches; i++) {
		uint32_t num = (i == batches - 1) ? n % 3 : 3, j;
		int8_t power[3];
		struct mt7925_msg m;

		if (num == 0)
			num = 3;
		for (j = 0; j < num; j++)
			power[j] = channel_power(band, list[idx + j]);

		mt7925_mcu_body(&m);
		mt7925_build_power_limit(&m, g_alpha2, band, list + idx, power,
					 (uint8_t)num, idx + num == n);
		ret = send_set(MT_UNI_CMD_SET_POWER_LIMIT, &m);
		if (ret)
			return ret;
		idx += num;
	}
	return ANX_OK;
}

int mt7925_mcu_set_rate_txpower(struct mt7925_dev *dev)
{
	int ret;

	if (dev->has_2g) {
		ret = rate_txpower_band(MT_BAND_2G);
		if (ret)
			return ret;
	}
	if (dev->has_5g) {
		ret = rate_txpower_band(MT_BAND_5G);
		if (ret)
			return ret;
	}
	return ANX_OK;
}

/* __mt7925_mcu_set_clc(): returns 1 when no rule matched alpha2. */
static int set_clc_one(const uint8_t *blob, uint8_t idx,
		       const uint8_t alpha2[2], uint8_t env)
{
	const struct mt7925_clc_hdr *clc = (const struct mt7925_clc_hdr *)blob;
	const uint8_t *data = blob + sizeof(*clc);
	const uint8_t *pos, *last;
	uint32_t matched = 0;
	int ret;

	if (clc->len < sizeof(*clc) + 8)
		return ANX_EINVAL;
	pos = data + sizeof(struct mt7925_clc_segment) * clc->t0_nr_seg;
	last = data + rd32(data + 4);
	if (last > blob + clc->len)
		return ANX_EINVAL;

	for (; pos + sizeof(struct mt7925_clc_rule) <= last;
	     pos += sizeof(struct mt7925_clc_rule)) {
		const struct mt7925_clc_rule *rule =
			(const struct mt7925_clc_rule *)pos;
		const struct mt7925_clc_segment *seg;
		struct mt7925_msg m;

		if (rule->alpha2[0] != alpha2[0] || rule->alpha2[1] != alpha2[1])
			continue;
		if (rule->seg_idx == 0 || rule->seg_idx > clc->t0_nr_seg)
			continue;
		seg = (const struct mt7925_clc_segment *)
		      (data + (rule->seg_idx - 1) *
		       sizeof(struct mt7925_clc_segment));
		if (seg->offset + seg->len > clc->len - sizeof(*clc))
			return ANX_EINVAL;

		mt7925_mcu_body(&m);
		mt7925_build_clc(&m, clc->ver, idx, env, rule->alpha2,
				 rule->type, data + seg->offset, seg->len);
		ret = send_set(MT_UNI_CMD_SET_POWER_LIMIT, &m);
		if (ret)
			return ret;
		matched++;
	}
	return matched ? ANX_OK : 1;
}

/* mt7925_mcu_set_clc() */
int mt7925_mcu_set_clc(struct mt7925_dev *dev, const char alpha2[2])
{
	static const uint8_t world[2] = { '0', '0' };
	uint8_t cc[2] = { (uint8_t)alpha2[0], (uint8_t)alpha2[1] };
	uint8_t i;
	int ret;

	for (i = CLC_POWER; i <= CLC_POWER_EXT; i++) {
		if (!dev->clc[i])
			continue;
		ret = set_clc_one(dev->clc[i], i, cc, ENVIRON_ANY);
		if (ret == 1) {
			kprintf("mt7925: CLC %u has no %c%c rule, using 00\n",
				i, alpha2[0], alpha2[1]);
			ret = set_clc_one(dev->clc[i], i, world, ENVIRON_INDOOR);
		}
		if (ret < 0 || ret == 1)
			return ret < 0 ? ret : ANX_ENOENT;
	}
	return ANX_OK;
}

int mt7925_mcu_set_rxfilter(uint32_t fif, uint8_t bit_op, uint32_t bit_map)
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_rxfilter(&m, 0, fif, bit_op, bit_map);
	return send_set(MT_UNI_CMD_BAND_CONFIG, &m);
}

/* ------------------------------------------------------------------ */
/* Interface, BSS and station records                                   */
/* ------------------------------------------------------------------ */

/* mt76_connac_mcu_uni_add_dev() */
int mt7925_mcu_add_dev(struct mt7925_dev *dev, bool enable)
{
	struct mt7925_bss_params p = {
		.bss_idx = MT7925_BSS_IDX,
		.omac_idx = MT7925_OMAC_IDX,
		.band_idx = dev->band_idx,
		.bmc_wlan_idx = MT7925_WCID_BSS,
		.sta_wlan_idx = MT7925_WCID_BSS,
		.enable = enable,
	};
	struct mt7925_msg m;
	int ret;

	mt7925_mcu_body(&m);
	mt7925_build_dev_info(&m, MT7925_OMAC_IDX, dev->band_idx, 0,
			      dev->mac, enable);
	ret = send_set(MT_UNI_CMD_DEV_INFO_UPDATE, &m);
	if (ret)
		return ret;

	mt7925_mcu_body(&m);
	mt7925_build_bss_add(&m, &p);
	return send_set(MT_UNI_CMD_BSS_INFO_UPDATE, &m);
}

int mt7925_mcu_add_bss_info(const struct mt7925_bss_params *p)
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_bss_info(&m, p);
	return send_set(MT_UNI_CMD_BSS_INFO_UPDATE, &m);
}

int mt7925_mcu_sta_update(const struct mt7925_sta_params *p)
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_sta_rec(&m, p);
	return send_set(MT_UNI_CMD_STA_REC_UPDATE, &m);
}

int mt7925_mcu_sta_hdr_trans(const struct mt7925_sta_params *p)
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_sta_hdr_trans(&m, p);
	return send_set(MT_UNI_CMD_STA_REC_UPDATE, &m);
}

int mt7925_mcu_add_key(const struct mt7925_key_params *p)
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_sta_key(&m, p);
	return send_set(MT_UNI_CMD_STA_REC_UPDATE, &m);
}

/* mt7925_set_roc(): acquire the channel and wait up to 4 s for the grant. */
int mt7925_mcu_set_roc(struct mt7925_dev *dev, uint8_t channel,
		       uint8_t band, uint32_t duration_ms)
{
	struct mt7925_msg m;
	int ret;

	dev->roc_granted = false;
	dev->roc_token++;
	mt7925_mcu_body(&m);
	mt7925_build_roc(&m, MT7925_BSS_IDX, dev->roc_token, channel, band,
			 duration_ms);
	ret = send_set(MT_UNI_CMD_ROC, &m);
	if (ret)
		return ret;
	if (!mt7925_wait_flag(dev, &dev->roc_granted, 4000)) {
		kprintf("mt7925: no grant for channel %u\n", channel);
		mt7925_mcu_abort_roc(dev);
		return ANX_ETIMEDOUT;
	}
	dev->roc_channel = channel;
	return ANX_OK;
}

int mt7925_mcu_abort_roc(struct mt7925_dev *dev)
{
	struct mt7925_msg m;

	dev->roc_granted = false;
	mt7925_mcu_body(&m);
	mt7925_build_roc_abort(&m, MT7925_BSS_IDX, dev->roc_token);
	return send_set(MT_UNI_CMD_ROC, &m);
}

int mt7925_mcu_hw_scan(struct mt7925_dev *dev, const uint8_t *ssid,
		       uint8_t ssid_len)
{
	uint8_t bands[N_CHANNELS], nums[N_CHANNELS];
	uint8_t ies_2g[32], ies_5g[32];
	struct mt7925_scan_params p;
	struct mt7925_msg m;
	uint32_t i, n = 0;

	for (i = 0; i < N_CHANNELS; i++) {
		if ((g_channels[i].band == MT_BAND_2G && !dev->has_2g) ||
		    (g_channels[i].band == MT_BAND_5G && !dev->has_5g))
			continue;
		bands[n] = g_channels[i].band;
		nums[n] = g_channels[i].channel;
		n++;
	}

	dev->scan_seq = (uint8_t)((dev->scan_seq + 1) & 0x7f);
	anx_memset(&p, 0, sizeof(p));
	p.seq_num = (uint8_t)(dev->scan_seq | (dev->band_idx << 7));
	p.bss_idx = MT7925_BSS_IDX;
	p.ssid = ssid;
	p.ssid_len = ssid_len;
	p.n_channels = (uint8_t)n;
	p.chan_band = bands;
	p.chan_num = nums;
	p.ies_2g = ies_2g;
	p.ies_2g_len = (uint16_t)mt7925_ieee_build_preq_ies(MT_BAND_2G, ies_2g,
							     sizeof(ies_2g));
	p.ies_5g = ies_5g;
	p.ies_5g_len = (uint16_t)mt7925_ieee_build_preq_ies(MT_BAND_5G, ies_5g,
							     sizeof(ies_5g));

	mt7925_mcu_body(&m);
	mt7925_build_scan(&m, &p);
	return send_set(MT_UNI_CMD_SCAN_REQ, &m);
}

int mt7925_mcu_set_tx(const struct mt7925_edca_params ac[4])
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_edca(&m, MT7925_BSS_IDX, ac);
	return send_set(MT_UNI_CMD_EDCA_UPDATE, &m);
}

int mt7925_mcu_set_timing(uint16_t slot_time)
{
	struct mt7925_msg m;

	mt7925_mcu_body(&m);
	mt7925_build_bss_timing(&m, MT7925_BSS_IDX, slot_time);
	return send_set(MT_UNI_CMD_BSS_INFO_UPDATE, &m);
}

/* mt7925_mcu_set_beacon_filter() */
int mt7925_mcu_set_beacon_filter(struct mt7925_dev *dev, bool enable)
{
	struct mt7925_msg m;
	int ret;

	mt7925_mcu_body(&m);
	if (enable)
		mt7925_build_bss_bcnft(&m, MT7925_BSS_IDX,
				       dev->bss.beacon_int,
				       dev->bss.dtim_period);
	else
		mt7925_build_bss_pm_disable(&m, MT7925_BSS_IDX);
	ret = send_set(MT_UNI_CMD_BSS_INFO_UPDATE, &m);
	if (ret)
		return ret;

	return mt7925_mcu_set_rxfilter(0, enable ? FIF_BIT_SET : FIF_BIT_CLR,
				       WF_RFCR_DROP_OTHER_BEACON);
}
