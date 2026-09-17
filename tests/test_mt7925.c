/*
 * test_mt7925.c — MT7925 station path encodings.
 *
 * The driver cannot run on the host, but everything it puts on the wire
 * can be checked here: command layouts against the Linux v6.19 mt76
 * structures they were ported from, the per-frame descriptors, the 802.11
 * frames, and the WPA2 handshake against vectors produced independently by
 * tools/gen_wpa_vectors.py.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/crypto.h>
#include "../kernel/drivers/net/wifi/mt7925_uni.h"
#include "../kernel/drivers/net/wifi/mt7925_mac.h"
#include "../kernel/drivers/net/wifi/mt7925_ieee.h"
#include "../kernel/drivers/net/wifi/mt7925_eapol.h"
#include "harness/mt76_linux_layout.h"
#include "harness/mt7925_wpa_vectors.h"

#define CHECK(cond, msg)						\
	do {								\
		if (!(cond)) {						\
			kprintf("  FAIL: %s (line %d)\n", (msg), __LINE__); \
			return -1;					\
		}							\
	} while (0)

#define OFF(s, f)	__builtin_offsetof(struct s, f)

/* Same size, and each named field at the same offset. */
#define SAME_SIZE(ours, theirs)						\
	CHECK(sizeof(struct ours) == sizeof(struct theirs), #ours " size")
#define SAME(ours, theirs, f)						\
	CHECK(OFF(ours, f) == OFF(theirs, f), #ours "." #f)
#define SAME2(ours, fo, theirs, ft)					\
	CHECK(OFF(ours, fo) == OFF(theirs, ft), #ours "." #fo)

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool all_equal(const uint8_t *p, uint32_t n, uint8_t v)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (p[i] != v)
			return false;
	return true;
}

/*
 * Walk a message body's TLVs from offset start. Fills tags[] and lens[]
 * and returns the count, or -1 if a TLV overruns the body.
 */
static int walk(const uint8_t *body, uint32_t len, uint32_t start,
		uint16_t *tags, uint16_t *lens, int max)
{
	uint32_t pos = start;
	int n = 0;

	while (pos < len && n < max) {
		uint16_t t = le16(body + pos), l = le16(body + pos + 2);

		if (l < 4 || pos + l > len)
			return -1;
		tags[n] = t;
		lens[n] = l;
		n++;
		pos += l;
	}
	return pos == len ? n : -1;
}

/* ------------------------------------------------------------------ */
/* Layouts                                                              */
/* ------------------------------------------------------------------ */

static int test_layouts(void)
{
	SAME_SIZE(mt7925_uni_txd, lnx_mt76_connac2_mcu_uni_txd);
	SAME(mt7925_uni_txd, lnx_mt76_connac2_mcu_uni_txd, len);
	SAME(mt7925_uni_txd, lnx_mt76_connac2_mcu_uni_txd, cid);
	SAME(mt7925_uni_txd, lnx_mt76_connac2_mcu_uni_txd, pkt_type);
	SAME(mt7925_uni_txd, lnx_mt76_connac2_mcu_uni_txd, seq);
	SAME(mt7925_uni_txd, lnx_mt76_connac2_mcu_uni_txd, s2d_index);
	SAME(mt7925_uni_txd, lnx_mt76_connac2_mcu_uni_txd, option);

	SAME_SIZE(mt7925_mcu_rxd, lnx_mt7925_mcu_rxd);
	SAME(mt7925_mcu_rxd, lnx_mt7925_mcu_rxd, eid);
	SAME(mt7925_mcu_rxd, lnx_mt7925_mcu_rxd, seq);
	SAME(mt7925_mcu_rxd, lnx_mt7925_mcu_rxd, option);
	SAME(mt7925_mcu_rxd, lnx_mt7925_mcu_rxd, s2d_index);
	CHECK(sizeof(struct mt7925_mcu_rxd) == OFF(lnx_mt7925_mcu_rxd, tlv),
	      "rxd payload offset");

	SAME_SIZE(mt7925_uni_event, lnx_mt7925_mcu_uni_event);
	SAME(mt7925_uni_event, lnx_mt7925_mcu_uni_event, status);

	SAME_SIZE(mt7925_sta_req_hdr, lnx_sta_req_hdr);
	SAME(mt7925_sta_req_hdr, lnx_sta_req_hdr, tlv_num);
	SAME(mt7925_sta_req_hdr, lnx_sta_req_hdr, muar_idx);
	SAME(mt7925_sta_req_hdr, lnx_sta_req_hdr, wlan_idx_hi);

	SAME_SIZE(mt7925_sta_rec_basic, lnx_sta_rec_basic);
	SAME(mt7925_sta_rec_basic, lnx_sta_rec_basic, conn_type);
	SAME(mt7925_sta_rec_basic, lnx_sta_rec_basic, aid);
	SAME(mt7925_sta_rec_basic, lnx_sta_rec_basic, peer_addr);
	SAME(mt7925_sta_rec_basic, lnx_sta_rec_basic, extra_info);

	SAME_SIZE(mt7925_sta_rec_phy, lnx_sta_rec_phy);
	SAME(mt7925_sta_rec_phy, lnx_sta_rec_phy, phy_type);
	SAME(mt7925_sta_rec_phy, lnx_sta_rec_phy, max_ampdu_len);

	SAME_SIZE(mt7925_sta_rec_ra, lnx_sta_rec_ra_info);
	SAME(mt7925_sta_rec_ra, lnx_sta_rec_ra_info, legacy);

	SAME_SIZE(mt7925_sta_rec_state, lnx_sta_rec_state_v2);
	SAME(mt7925_sta_rec_state, lnx_sta_rec_state_v2, state);
	SAME(mt7925_sta_rec_state, lnx_sta_rec_state_v2, flags);
	SAME(mt7925_sta_rec_state, lnx_sta_rec_state_v2, vht_opmode);

	SAME_SIZE(mt7925_sta_rec_mld, lnx_sta_rec_mld);
	SAME(mt7925_sta_rec_mld, lnx_sta_rec_mld, primary_id);
	SAME(mt7925_sta_rec_mld, lnx_sta_rec_mld, wlan_id);
	SAME(mt7925_sta_rec_mld, lnx_sta_rec_mld, link_num);
	SAME(mt7925_sta_rec_mld, lnx_sta_rec_mld, link);

	SAME_SIZE(mt7925_sta_rec_hdr_trans, lnx_sta_rec_hdr_trans);
	SAME(mt7925_sta_rec_hdr_trans, lnx_sta_rec_hdr_trans, to_ds);
	SAME(mt7925_sta_rec_hdr_trans, lnx_sta_rec_hdr_trans, dis_rx_hdr_tran);

	SAME_SIZE(mt7925_sta_rec_remove, lnx_sta_rec_remove);

	SAME_SIZE(mt7925_sta_rec_key, lnx_sta_rec_sec_uni);
	SAME(mt7925_sta_rec_key, lnx_sta_rec_sec_uni, peer_addr);
	SAME(mt7925_sta_rec_key, lnx_sta_rec_sec_uni, bss_idx);
	SAME(mt7925_sta_rec_key, lnx_sta_rec_sec_uni, cipher_id);
	SAME(mt7925_sta_rec_key, lnx_sta_rec_sec_uni, key_len);
	SAME(mt7925_sta_rec_key, lnx_sta_rec_sec_uni, wlan_idx);
	SAME(mt7925_sta_rec_key, lnx_sta_rec_sec_uni, mgmt_prot);
	SAME(mt7925_sta_rec_key, lnx_sta_rec_sec_uni, key);
	SAME(mt7925_sta_rec_key, lnx_sta_rec_sec_uni, key_rsc);

	SAME_SIZE(mt7925_bss_req_hdr, lnx_bss_req_hdr);

	SAME_SIZE(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv);
	SAME(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv, band_idx);
	SAME(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv, conn_type);
	SAME(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv, conn_state);
	SAME(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv, bssid);
	SAME(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv, bmc_tx_wlan_idx);
	SAME(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv, bcn_interval);
	SAME(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv, phymode);
	SAME(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv, sta_idx);
	SAME(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv, nonht_basic_phy);
	SAME(mt7925_bss_basic, lnx_mt76_connac_bss_basic_tlv, link_idx);

	SAME_SIZE(mt7925_bss_sec, lnx_bss_sec_tlv);
	SAME(mt7925_bss_sec, lnx_bss_sec_tlv, cipher);

	SAME_SIZE(mt7925_bss_rate, lnx_bss_rate_tlv);
	SAME(mt7925_bss_rate, lnx_bss_rate_tlv, basic_rate);
	SAME(mt7925_bss_rate, lnx_bss_rate_tlv, short_preamble);
	SAME(mt7925_bss_rate, lnx_bss_rate_tlv, mc_fixed_rate);

	SAME_SIZE(mt7925_bss_qos, lnx_mt76_connac_bss_qos_tlv);

	SAME_SIZE(mt7925_bss_mld, lnx_bss_mld_tlv);
	SAME(mt7925_bss_mld, lnx_bss_mld_tlv, mac_addr);
	SAME(mt7925_bss_mld, lnx_bss_mld_tlv, remap_idx);
	SAME(mt7925_bss_mld, lnx_bss_mld_tlv, hybrid_mode);

	SAME_SIZE(mt7925_bss_ifs, lnx_bss_ifs_time_tlv);
	SAME(mt7925_bss_ifs, lnx_bss_ifs_time_tlv, slot_time);
	SAME(mt7925_bss_ifs, lnx_bss_ifs_time_tlv, eifs_cck_time);

	SAME_SIZE(mt7925_bss_rlm, lnx_bss_rlm_tlv);
	SAME(mt7925_bss_rlm, lnx_bss_rlm_tlv, bw);
	SAME(mt7925_bss_rlm, lnx_bss_rlm_tlv, sco);
	SAME(mt7925_bss_rlm, lnx_bss_rlm_tlv, band);

	SAME_SIZE(mt7925_bss_mbssid, lnx_bss_info_uni_mbssid);

	CHECK(sizeof(struct mt7925_bss_req_hdr) +
	      sizeof(struct mt7925_bss_bcnft) == sizeof(struct lnx_bcnft_req),
	      "bcnft request size");
	CHECK(OFF(mt7925_bss_bcnft, dtim_period) + 4 ==
	      OFF(lnx_bcnft_req, bcnft) + 6, "bcnft dtim offset");

	SAME_SIZE(mt7925_dev_info_req, lnx_dev_req);
	SAME2(mt7925_dev_info_req, tag, lnx_dev_req, tlv.tag);
	SAME2(mt7925_dev_info_req, active, lnx_dev_req, tlv.active);
	SAME2(mt7925_dev_info_req, omac_addr, lnx_dev_req, tlv.omac_addr);

	SAME_SIZE(mt7925_roc_acquire, lnx_roc_acquire_tlv);
	SAME(mt7925_roc_acquire, lnx_roc_acquire_tlv, reqtype);
	SAME(mt7925_roc_acquire, lnx_roc_acquire_tlv, maxinterval);
	SAME(mt7925_roc_acquire, lnx_roc_acquire_tlv, dbdcband);

	SAME_SIZE(mt7925_roc_abort, lnx_roc_abort_tlv);
	SAME(mt7925_roc_abort, lnx_roc_abort_tlv, dbdcband);

	SAME_SIZE(mt7925_roc_grant, lnx_mt7925_roc_grant_tlv);
	SAME(mt7925_roc_grant, lnx_mt7925_roc_grant_tlv, primarychannel);
	SAME(mt7925_roc_grant, lnx_mt7925_roc_grant_tlv, reqtype);
	SAME(mt7925_roc_grant, lnx_mt7925_roc_grant_tlv, dbdcband);
	SAME(mt7925_roc_grant, lnx_mt7925_roc_grant_tlv, max_interval);

	SAME_SIZE(mt7925_scan_hdr, lnx_scan_hdr_tlv);
	SAME_SIZE(mt7925_scan_req, lnx_scan_req_tlv);
	SAME(mt7925_scan_req, lnx_scan_req_tlv, scan_func);
	SAME(mt7925_scan_req, lnx_scan_req_tlv, func_mask_ext);
	SAME_SIZE(mt7925_scan_ssid_entry, lnx_mt76_connac_mcu_scan_ssid);
	SAME_SIZE(mt7925_scan_ssid, lnx_scan_ssid_tlv);
	SAME(mt7925_scan_ssid, lnx_scan_ssid_tlv, ssids);
	SAME_SIZE(mt7925_scan_bssid, lnx_scan_bssid_tlv);
	SAME(mt7925_scan_bssid, lnx_scan_bssid_tlv, match_short_ssid_ind);
	SAME_SIZE(mt7925_scan_chan, lnx_scan_chan_info_tlv);
	SAME(mt7925_scan_chan, lnx_scan_chan_info_tlv, channels);
	SAME_SIZE(mt7925_scan_ie, lnx_scan_ie_tlv);
	SAME(mt7925_scan_ie, lnx_scan_ie_tlv, band);
	SAME_SIZE(mt7925_scan_misc, lnx_scan_misc_tlv);

	SAME_SIZE(mt7925_edca, lnx_edca);
	SAME2(mt7925_edca, rsv, lnx_edca, __rsv);
	SAME(mt7925_edca, lnx_edca, txop);
	SAME(mt7925_edca, lnx_edca, aifs);

	SAME_SIZE(mt7925_chip_config_req, lnx_chip_config_req);
	SAME2(mt7925_chip_config_req, data_size, lnx_chip_config_req,
	      config.data_size);
	SAME2(mt7925_chip_config_req, data, lnx_chip_config_req, config.data);

	SAME_SIZE(mt7925_rts_req, lnx_rts_req);
	SAME(mt7925_rts_req, lnx_rts_req, pkt_thresh);
	SAME_SIZE(mt7925_rxfilter_req, lnx_rxfilter_req);
	SAME(mt7925_rxfilter_req, lnx_rxfilter_req, bit_op);
	SAME_SIZE(mt7925_fw_log_req, lnx_fw_log_req);
	SAME_SIZE(mt7925_eeprom_mode_req, lnx_eeprom_req);
	SAME(mt7925_eeprom_mode_req, lnx_eeprom_req, format);
	SAME_SIZE(mt7925_efuse_read_req, lnx_efuse_read_req);
	SAME(mt7925_efuse_read_req, lnx_efuse_read_req, addr);
	CHECK(MT_EFUSE_READ_EVT_DATA_OFFSET == OFF(lnx_efuse_read_evt, data),
	      "efuse event data offset");
	SAME_SIZE(mt7925_nic_cap_req, lnx_nic_cap_req);
	SAME_SIZE(mt7925_nic_cap_hdr, lnx_mt76_connac_cap_hdr);

	SAME_SIZE(mt7925_domain_req, lnx_domain_req);
	SAME2(mt7925_domain_req, bw_5g, lnx_domain_req, hdr.bw_5g);
	SAME2(mt7925_domain_req, tag, lnx_domain_req, n_ch.tag);
	SAME2(mt7925_domain_req, n_6ch, lnx_domain_req, n_ch.n_6ch);
	SAME_SIZE(mt7925_domain_chan, lnx_mt76_connac_mcu_chan);
	SAME(mt7925_domain_chan, lnx_mt76_connac_mcu_chan, flags);

	SAME_SIZE(mt7925_power_limit_req, lnx_mt7925_tx_power_limit_tlv);
	SAME(mt7925_power_limit_req, lnx_mt7925_tx_power_limit_tlv, n_chan);
	SAME(mt7925_power_limit_req, lnx_mt7925_tx_power_limit_tlv, last_msg);
	SAME(mt7925_power_limit_req, lnx_mt7925_tx_power_limit_tlv, alpha2);
	SAME_SIZE(mt7925_sku, lnx_mt7925_sku_tlv);

	SAME_SIZE(mt7925_clc_req, lnx_clc_req);
	SAME(mt7925_clc_req, lnx_clc_req, size);
	SAME(mt7925_clc_req, lnx_clc_req, env);
	SAME(mt7925_clc_req, lnx_clc_req, type);
	SAME_SIZE(mt7925_clc_hdr, lnx_mt7925_clc);
	SAME2(mt7925_clc_hdr, t0_type, lnx_mt7925_clc, t0.type);
	SAME2(mt7925_clc_hdr, t0_nr_seg, lnx_mt7925_clc, t0.nr_seg);
	SAME_SIZE(mt7925_clc_rule, lnx_mt7925_clc_rule);
	SAME(mt7925_clc_rule, lnx_mt7925_clc_rule, seg_idx);
	SAME_SIZE(mt7925_clc_segment, lnx_mt7925_clc_segment);
	SAME(mt7925_clc_segment, lnx_mt7925_clc_segment, offset);
	SAME(mt7925_clc_segment, lnx_mt7925_clc_segment, len);

	CHECK(MT_TXP_SIZE == sizeof(struct lnx_mt76_connac_hw_txp),
	      "TXP size");
	CHECK(OFF(lnx_mt76_connac_hw_txp, ptr) == 8, "TXP ptr offset");
	CHECK(OFF(lnx_mt76_connac_txp_ptr, len0) == 4, "TXP len0 offset");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Command builders                                                     */
/* ------------------------------------------------------------------ */

static uint8_t g_buf[16384];

static const uint8_t own_mac[6] = { 0x46, 0x3b, 0x72, 0x47, 0xb2, 0x5c };
static const uint8_t ap_mac[6]  = { 0x02, 0x00, 0x00, 0xaa, 0x00, 0x01 };

static void bss_template(struct mt7925_bss_params *p, bool enable)
{
	anx_memset(p, 0, sizeof(*p));
	p->bss_idx = 0;
	p->band_idx = 0xff;
	anx_memcpy(p->own_addr, own_mac, 6);
	anx_memcpy(p->bssid, ap_mac, 6);
	p->bcn_interval = 100;
	p->dtim_period = 3;
	p->band = MT_BAND_2G;
	p->channel = 6;
	p->center_chan = 6;
	p->nss = 2;
	p->phymode = MT_PHY_MODE_B | MT_PHY_MODE_G;
	p->bmc_wlan_idx = 19;
	p->sta_wlan_idx = 1;
	p->cipher = MT_CIPHER_AES_CCMP;
	p->rate_idx = 11;
	p->slot_time = 20;
	p->enable = enable;
}

static int test_uni_txd(void)
{
	struct mt7925_uni_txd txd;

	mt7925_uni_fill_txd(&txd, MT_UNI_CMD_BSS_INFO_UPDATE,
			    MT_UNI_OPT_SET_ACK, 5, 12);
	CHECK(txd.txd[0] == (60U | (2U << 23) | (0x20U << 25)), "txd0");
	CHECK(txd.txd[1] == (1U << 14), "txd1 HDR_FORMAT_CMD at bit 14");
	CHECK(txd.len == 60 - 32, "len excludes hardware descriptor");
	CHECK(txd.cid == MT_UNI_CMD_BSS_INFO_UPDATE, "cid");
	CHECK(txd.pkt_type == 0xa0 && txd.seq == 5, "pkt type and seq");
	CHECK(txd.option == 0x07 && txd.s2d_index == 0, "option");

	mt7925_uni_fill_txd(&txd, MT_UNI_CMD_CHIP_CONFIG,
			    MT_UNI_OPT_SET_ACK, 1, 0);
	CHECK(txd.option == 0x06, "chip config is sent without ack");

	mt7925_uni_fill_txd(&txd, MT_UNI_CMD_EFUSE_CTRL,
			    MT_UNI_OPT_QUERY_ACK, 1, 0);
	CHECK(txd.option == 0x03, "query option");
	return 0;
}

static int test_uni_bss(void)
{
	static const uint16_t tags_off[] = { 0, 16, 11, 15, 26, 23 };
	static const uint16_t lens_off[] = { 32, 8, 16, 8, 20, 20 };
	struct mt7925_bss_params p;
	struct mt7925_msg m;
	uint16_t tags[16], lens[16];
	const uint8_t *b;
	int n, i;

	bss_template(&p, false);
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_bss_info(&m, &p);
	CHECK(!m.overflow, "no overflow");
	n = walk(g_buf, m.len, 4, tags, lens, 16);
	CHECK(n == 6, "disabled BSS carries six TLVs");
	for (i = 0; i < 6; i++) {
		CHECK(tags[i] == tags_off[i], "TLV order");
		CHECK(lens[i] == lens_off[i], "TLV length");
	}
	CHECK(g_buf[0] == 0 && le16(g_buf + 2) == 6, "header count");

	b = g_buf + 4;				/* basic */
	CHECK(b[4] == 1, "basic.active");
	CHECK(b[7] == 0xff, "basic.band_idx");
	CHECK(le32(b + 8) == 0x10001, "basic.conn_type INFRA_STA");
	CHECK(b[12] == 1, "basic.conn_state = !enable");
	CHECK(anx_memcmp(b + 14, ap_mac, 6) == 0, "basic.bssid");
	CHECK(le16(b + 20) == 19 && le16(b + 22) == 100, "bmc idx, beacon");
	CHECK(b[24] == 3 && b[25] == 0x06, "dtim, phymode");
	CHECK(le16(b + 26) == 1 && le16(b + 28) == 1, "sta_idx, ERP index");

	b = g_buf + 4 + 32;			/* sec */
	CHECK(b[4] == 7 && b[5] == 6 && b[6] == 4, "sec: WPA2-PSK CCMP");

	b += 8;					/* rate */
	CHECK(le16(b + 6) == 0x000f && b[12] == 1, "2 GHz basic rates");
	CHECK(b[13] == 11 && b[14] == 11, "bc/mc fixed rate");

	b += 16 + 8;				/* mld */
	CHECK(b[4] == 0xff && b[5] == 32, "mld group/own id");
	CHECK(anx_memcmp(b + 6, own_mac, 6) == 0, "mld own address");
	CHECK(b[12] == 0xff && b[13] == 0xff, "mld remap and link id");

	b += 20;				/* ifs */
	CHECK(b[4] == 1 && le16(b + 8) == 20, "ifs slot");

	p.enable = true;
	p.band = MT_BAND_5G;
	p.channel = p.center_chan = 36;
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_bss_info(&m, &p);
	n = walk(g_buf, m.len, 4, tags, lens, 16);
	CHECK(n == 8 && le16(g_buf + 2) == 8, "enabled BSS adds two TLVs");
	CHECK(tags[6] == 2 && lens[6] == 16, "RLM");
	CHECK(tags[7] == 6 && lens[7] == 8, "MBSSID");
	CHECK(g_buf[4 + 12] == 0, "basic.conn_state when enabled");
	CHECK(le16(g_buf + 4 + 28) == 3, "5 GHz OFDM index");
	b = g_buf + 4 + 32 + 8;
	CHECK(le16(b + 6) == 0x0540 && b[12] == 0, "5 GHz basic rates");
	b = g_buf + m.len - 8 - 16;		/* rlm */
	CHECK(b[4] == 36 && b[5] == 36 && b[7] == 0, "rlm channel, 20 MHz");
	CHECK(b[8] == 2 && b[9] == 2 && b[10] == 0 && b[11] == 0,
	      "rlm streams, ht_op_info, sco");
	CHECK(b[12] == MT_BAND_5G, "rlm band");

	/* uni_add_dev: flat basic, no TLV count */
	p.enable = true;
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_bss_add(&m, &p);
	CHECK(m.len == 36 && le16(g_buf + 2) == 0, "bss add is flat");
	CHECK(le16(g_buf + 4) == 0 && le16(g_buf + 6) == 32, "bss add tlv");
	CHECK(g_buf[8] == 1 && g_buf[16] == 1, "active, conn_state 1");

	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_dev_info(&m, 0, 0xff, 0, own_mac, true);
	CHECK(m.len == 16, "dev info size");
	CHECK(g_buf[1] == 0xff && le16(g_buf + 6) == 12, "dev info band, len");
	CHECK(g_buf[8] == 1 && anx_memcmp(g_buf + 10, own_mac, 6) == 0,
	      "dev info active, addr");

	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_bss_bcnft(&m, 0, 100, 3);
	CHECK(m.len == 16 && le16(g_buf + 4) == 22 && le16(g_buf + 6) == 12,
	      "bcnft tlv");
	CHECK(le16(g_buf + 8) == 100 && g_buf[10] == 3, "bcnft values");

	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_bss_pm_disable(&m, 0);
	CHECK(m.len == 8 && le16(g_buf + 4) == 27 && le16(g_buf + 6) == 4,
	      "pm disable");
	return 0;
}

static void sta_template(struct mt7925_sta_params *p, uint8_t state,
			 bool newly)
{
	anx_memset(p, 0, sizeof(*p));
	p->wlan_idx = 1;
	p->enable = true;
	p->newly = newly;
	p->has_peer = true;
	p->state = state;
	anx_memcpy(p->peer_addr, ap_mac, 6);
	p->aid = 5;
	p->phy_type = MT_PHY_TYPE_BIT_HR_DSSS | MT_PHY_TYPE_BIT_ERP;
	p->basic_rates = 0x000f;
	p->ra_legacy = 0x3fcf;
	p->to_ds = true;
	p->dis_rx_hdr_tran = true;
}

static int test_uni_sta(void)
{
	struct mt7925_sta_params p;
	struct mt7925_key_params k;
	struct mt7925_msg m;
	uint16_t tags[16], lens[16];
	uint8_t key[32];
	const uint8_t *b;
	int n, i;

	sta_template(&p, MT_STA_INFO_STATE_NONE, true);
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_sta_rec(&m, &p);
	n = walk(g_buf, m.len, 8, tags, lens, 16);
	CHECK(n == 5 && le16(g_buf + 2) == 5, "NONE record: five TLVs");
	CHECK(tags[0] == 0x00 && lens[0] == 20, "basic");
	CHECK(tags[1] == 0x15 && lens[1] == 12, "phy");
	CHECK(tags[2] == 0x01 && lens[2] == 16, "ra");
	CHECK(tags[3] == 0x07 && lens[3] == 16, "state");
	CHECK(tags[4] == 0x2b && lens[4] == 8, "hdr_trans");
	CHECK(g_buf[0] == 0 && g_buf[1] == 1 && g_buf[4] == 1 &&
	      g_buf[5] == 0 && g_buf[6] == 0, "header indices");

	b = g_buf + 8;
	CHECK(le32(b + 4) == 0x10002, "conn_type INFRA_AP");
	CHECK(b[8] == 2 && le16(b + 10) == 5, "PORT_SECURE, aid");
	CHECK(anx_memcmp(b + 12, ap_mac, 6) == 0, "peer");
	CHECK(le16(b + 18) == 3, "extra_info VER|NEW");
	b += 20;
	CHECK(le16(b + 4) == 0x000f && b[6] == 0x03, "phy basic, type");
	b += 12;
	CHECK(le16(b + 4) == 0x3fcf, "ra legacy");
	b += 16;
	CHECK(b[4] == 0, "state NONE");
	b += 16;
	CHECK(b[4] == 0 && b[5] == 1 && b[6] == 1, "from_ds, to_ds, dis");

	sta_template(&p, MT_STA_INFO_STATE_ASSOC, false);
	p.dis_rx_hdr_tran = false;
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_sta_rec(&m, &p);
	n = walk(g_buf, m.len, 8, tags, lens, 16);
	CHECK(n == 6, "ASSOC record adds MLD");
	CHECK(tags[4] == 0x20 && lens[4] == 28, "mld");
	CHECK(le16(g_buf + 8 + 18) == 1, "extra_info VER only");
	CHECK(g_buf[8 + 20 + 12 + 16 + 4] == 2, "state ASSOC");
	b = g_buf + 8 + 20 + 12 + 16 + 16;
	CHECK(anx_memcmp(b + 4, ap_mac, 6) == 0 && le16(b + 10) == 1 &&
	      le16(b + 14) == 1 && b[16] == 0, "mld ids, no links");
	CHECK(g_buf[m.len - 2] == 0, "hdr trans enabled");

	/* Interface entry: no peer, hdr_trans only */
	anx_memset(&p, 0, sizeof(p));
	p.wlan_idx = 19;
	p.muar_idx = 0x0e;
	p.enable = true;
	p.state = MT_STA_INFO_STATE_ASSOC;
	p.to_ds = true;
	p.dis_rx_hdr_tran = true;
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_sta_rec(&m, &p);
	CHECK(m.len == 16 && le16(g_buf + 2) == 1, "own entry: one TLV");
	CHECK(g_buf[1] == 19 && g_buf[5] == 0x0e, "own entry idx, muar");

	/* Removal */
	sta_template(&p, MT_STA_INFO_STATE_NONE, false);
	p.enable = false;
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_sta_rec(&m, &p);
	n = walk(g_buf, m.len, 8, tags, lens, 16);
	CHECK(n == 2 && tags[0] == 0x25 && lens[0] == 8 &&
	      tags[1] == 0x23 && lens[1] == 4, "remove + MLD_OFF");

	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_sta_hdr_trans(&m, &p);
	CHECK(m.len == 16 && le16(g_buf + 8) == 0x2b, "hdr trans update");

	/* Keys */
	for (i = 0; i < 32; i++)
		key[i] = (uint8_t)i;
	anx_memset(&k, 0, sizeof(k));
	k.wlan_idx = 1;
	k.add = true;
	k.pairwise = true;
	anx_memcpy(k.peer_addr, ap_mac, 6);
	k.cipher = MT_CIPHER_AES_CCMP;
	k.key_len = 16;
	k.key = key;
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_sta_key(&m, &k);
	CHECK(m.len == 8 + 68 && le16(g_buf + 2) == 1, "key message");
	b = g_buf + 8;
	CHECK(le16(b) == 0x27 && le16(b + 2) == 68, "KEY_V3 tlv");
	CHECK(b[4] == 1 && b[5] == 1 && b[6] == 1 && b[7] == 0,
	      "add, tx_key, key_type, is_authenticator");
	CHECK(anx_memcmp(b + 8, ap_mac, 6) == 0, "key peer");
	CHECK(b[15] == 4 && b[16] == 0 && b[17] == 16, "cipher, id, len");
	CHECK(b[18] == 1 && b[19] == 1, "wlan idx, mgmt_prot");
	CHECK(anx_memcmp(b + 20, key, 16) == 0 && all_equal(b + 36, 32, 0),
	      "key bytes");

	k.wlan_idx = 19;
	k.muar_idx = 0x0e;
	k.pairwise = false;
	k.key_id = 1;
	k.cipher = MT_CIPHER_TKIP;
	k.key_len = 32;
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_sta_key(&m, &k);
	b = g_buf + 8;
	CHECK(b[5] == 0 && b[6] == 0 && b[16] == 1 && b[18] == 19,
	      "group key fields");
	CHECK(b[20 + 16] == 24 && b[20 + 24] == 16, "TKIP MIC keys swapped");
	return 0;
}

static int test_uni_misc(void)
{
	static const uint8_t bands[3] = { MT_BAND_2G, MT_BAND_5G, MT_BAND_5G };
	static const uint8_t chans[3] = { 1, 36, 149 };
	static const uint8_t ies2[4] = { 1, 2, 0x82, 0x84 };
	static const uint8_t ies5[3] = { 1, 1, 0x8c };
	static const uint8_t ssid[4] = { 'T', 'e', 's', 't' };
	static const struct mt7925_chan_entry dom[4] = {
		{ MT_BAND_5G, 36, 0 }, { MT_BAND_2G, 1, 0 },
		{ MT_BAND_5G, 52, MT_CHAN_FLAG_NO_IR | MT_CHAN_FLAG_RADAR },
		{ MT_BAND_2G, 6, 0 },
	};
	struct mt7925_edca_params ac[4];
	struct mt7925_scan_params sp;
	struct mt7925_msg m;
	uint16_t tags[16], lens[16];
	const uint8_t *b;
	uint8_t seg[5] = { 9, 8, 7, 6, 5 };
	int8_t sku[MT_SKU_POWER_LIMIT];
	int8_t pw[2] = { 60, 127 };
	uint8_t pch[2] = { 1, 14 };
	int n, i;

	/* ROC */
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_roc(&m, 0, 7, 44, MT_BAND_5G, 1000);
	CHECK(m.len == 28, "roc size");
	b = g_buf + 4;
	CHECK(le16(b) == 0 && le16(b + 2) == 24, "roc tlv");
	CHECK(b[5] == 7 && b[6] == 44 && b[7] == 0 && b[8] == 2, "roc fields");
	CHECK(b[10] == 44 && b[13] == 44 && b[15] == 0, "centers, JOIN");
	CHECK(le32(b + 16) == 1000 && b[20] == 0xff, "interval, dbdcband");

	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_roc_abort(&m, 0, 7);
	CHECK(m.len == 16 && le16(g_buf + 4) == 1 && le16(g_buf + 6) == 12,
	      "roc abort");
	CHECK(g_buf[9] == 7 && g_buf[10] == 0xff, "roc abort token, band");

	/* Scan */
	anx_memset(&sp, 0, sizeof(sp));
	sp.seq_num = 0x81;
	sp.ssid = ssid;
	sp.ssid_len = 4;
	sp.n_channels = 3;
	sp.chan_band = bands;
	sp.chan_num = chans;
	sp.ies_2g = ies2;
	sp.ies_2g_len = sizeof(ies2);
	sp.ies_5g = ies5;
	sp.ies_5g_len = sizeof(ies5);
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_scan(&m, &sp);
	n = walk(g_buf, m.len, 4, tags, lens, 16);
	CHECK(n == 7 && le16(g_buf + 2) == 7, "scan TLV count");
	CHECK(g_buf[0] == 0x81, "scan seq");
	CHECK(tags[0] == 1 && lens[0] == 20, "scan req");
	CHECK(tags[1] == 10 && lens[1] == 368, "scan ssid");
	CHECK(tags[2] == 11 && lens[2] == 16, "scan bssid");
	CHECK(tags[3] == 12 && lens[3] == 136, "scan channels");
	CHECK(tags[4] == 14 && lens[4] == 12, "scan misc");
	CHECK(tags[5] == 13 && lens[5] == 12, "scan ie 2g");
	CHECK(tags[6] == 13 && lens[6] == 11, "scan ie 5g");
	b = g_buf + 4;
	CHECK(b[4] == 1 && b[5] == 2 && b[6] == 0x20, "active split scan");
	b += 20;
	CHECK(b[4] == 4 && b[5] == 1 && le32(b + 8) == 4 &&
	      anx_memcmp(b + 12, ssid, 4) == 0, "ssid entry");
	b += 368;
	CHECK(all_equal(b + 4, 6, 0xff), "wildcard bssid");
	b += 16;
	CHECK(b[4] == 4 && b[5] == 3 && b[8] == 1 && b[9] == 1 &&
	      b[10] == 2 && b[11] == 36 && b[13] == 149, "channel list");
	b += 136 + 12;
	CHECK(le16(b + 4) == 4 && b[6] == 1 &&
	      anx_memcmp(b + 8, ies2, 4) == 0, "2g ies");

	/* EDCA */
	for (i = 0; i < 4; i++) {
		ac[i].aifs = 2;
		ac[i].cw_min = 15;
		ac[i].cw_max = 1023;
		ac[i].txop = (uint16_t)(i * 10);
	}
	ac[1].cw_min = 0;
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_edca(&m, 0, ac);
	n = walk(g_buf, m.len, 4, tags, lens, 16);
	CHECK(n == 4 && le16(g_buf + 2) == 4, "edca count");
	b = g_buf + 4;
	CHECK(lens[0] == 12 && b[4] == 0 && b[5] == 0x0f, "edca queue, set");
	CHECK(b[6] == 4 && b[7] == 10 && b[10] == 2, "fls(15), fls(1023)");
	CHECK(b[12 + 6] == 5, "cw_min 0 means 5");
	CHECK(le16(b + 36 + 8) == 30 && b[36 + 4] == 3, "txop, queue 3");

	/* Chip config */
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_chip_config(&m, "KeepFullPwr 1");
	CHECK(m.len == 336 && le16(g_buf + 4) == 2 && le16(g_buf + 6) == 332,
	      "chip config header");
	CHECK(le16(g_buf + 12) == 14 && anx_strcmp((char *)g_buf + 16,
						   "KeepFullPwr 1") == 0,
	      "chip config data");

	/* Small requests */
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_nic_cap(&m);
	CHECK(m.len == 8 && le16(g_buf + 4) == 3 && le16(g_buf + 6) == 4,
	      "nic cap");
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_fw_log(&m, 1);
	CHECK(m.len == 12 && le16(g_buf + 6) == 8 && g_buf[8] == 1, "fw log");
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_eeprom_mode(&m);
	CHECK(m.len == 12 && le16(g_buf + 4) == 2 && g_buf[8] == 0 &&
	      g_buf[9] == 1, "eeprom mode");
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_efuse_read(&m, 0xa71);
	CHECK(m.len == 32 && le16(g_buf + 4) == 1 && le16(g_buf + 6) == 28 &&
	      le32(g_buf + 8) == 0xa70, "efuse read");
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_rts(&m, 0, 0x92b);
	CHECK(m.len == 16 && le16(g_buf + 4) == 8 && le16(g_buf + 6) == 12 &&
	      le32(g_buf + 8) == 0x92b && le32(g_buf + 12) == 2, "rts");
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_rxfilter(&m, 0, 0, 1, 1U << 11);
	CHECK(m.len == 72 && le16(g_buf + 4) == 0x0c &&
	      le16(g_buf + 6) == 68, "rxfilter header");
	CHECK(g_buf[8] == 1 && le32(g_buf + 12) == 0 &&
	      le32(g_buf + 16) == (1U << 11) && g_buf[20] == 1,
	      "rxfilter bit update");
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_rxfilter(&m, 0, 1U << 31, 0, 0);
	CHECK(g_buf[8] == 0 && le32(g_buf + 12) == (1U << 31),
	      "rxfilter fif mode");

	/* Channel domain: grouped by band, in input order within a band */
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_domain(&m, "US", dom, 4);
	CHECK(m.len == 16 + 4 * 8, "domain size");
	CHECK(g_buf[0] == 'U' && g_buf[1] == 'S' && g_buf[2] == 0,
	      "domain alpha2");
	CHECK(g_buf[4] == 0 && g_buf[5] == 3 && g_buf[6] == 3, "domain bw");
	CHECK(le16(g_buf + 8) == 2 && le16(g_buf + 10) == 8 + 32,
	      "domain tlv");
	CHECK(g_buf[12] == 2 && g_buf[13] == 2 && g_buf[14] == 0,
	      "domain counts");
	CHECK(le16(g_buf + 16) == 1 && le16(g_buf + 24) == 6 &&
	      le16(g_buf + 32) == 36 && le16(g_buf + 40) == 52,
	      "domain order");
	CHECK(le32(g_buf + 44) == 0x0a, "domain DFS flags");

	/* SKU layout (mt7925_mcu_build_sku) */
	mt7925_fill_sku(sku, MT_BAND_2G, 60);
	CHECK(sku[0] == 60 && sku[3] == 60 && sku[4] == 60 && sku[11] == 60,
	      "sku cck, ofdm");
	CHECK(sku[12] == 127 && sku[43] == 127, "sku ofdm gap");
	CHECK(sku[44] == 60 && sku[60] == 60, "sku ht");
	CHECK(sku[61] == 60 && sku[70] == 60 && sku[71] == 127 &&
	      sku[72] == 127 && sku[73] == 60, "sku vht rows");
	CHECK(sku[108] == 127 && sku[109] == 60 && sku[192] == 60 &&
	      sku[193] == 60 && sku[448] == 60, "sku he, eht");
	mt7925_fill_sku(sku, MT_BAND_5G, 46);
	CHECK(sku[0] == 127 && sku[3] == 127 && sku[4] == 46, "5 GHz no cck");

	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_power_limit(&m, "US", MT_BAND_2G, pch, pw, 2, true);
	CHECK(m.len == 52 + 2 * 450, "power limit size");
	CHECK(le16(g_buf + 4) == 1 && le16(g_buf + 6) == 52, "power tlv");
	CHECK(g_buf[12] == 2 && g_buf[13] == 1 && g_buf[14] == 1,
	      "n_chan, band, last");
	CHECK(g_buf[16] == 'U' && g_buf[17] == 'S', "power alpha2");
	CHECK(g_buf[52] == 1 && (int8_t)g_buf[53] == 60 &&
	      g_buf[502] == 14 && (int8_t)g_buf[503] == 127,
	      "per-channel power");

	/* CLC */
	mt7925_msg_init(&m, g_buf, sizeof(g_buf));
	mt7925_build_clc(&m, 2, 1, 0, (const uint8_t *)"US",
			 (const uint8_t *)"AB", seg, sizeof(seg));
	CHECK(m.len == 84 + 5, "clc size");
	CHECK(le16(g_buf + 4) == 3 && le16(g_buf + 6) == 80, "clc tlv");
	CHECK(g_buf[8] == 2 && le16(g_buf + 10) == 5 && g_buf[12] == 1 &&
	      g_buf[13] == 0, "clc ver, size, idx, env");
	CHECK(g_buf[16] == 'U' && g_buf[18] == 'A' &&
	      anx_memcmp(g_buf + 84, seg, 5) == 0, "clc rule and segment");

	/* Overflow is sticky */
	mt7925_msg_init(&m, g_buf, 10);
	mt7925_build_chip_config(&m, "x");
	CHECK(m.overflow, "overflow detected");

	/* TLV walker */
	{
		uint8_t body[12] = { 1, 0, 4, 0, 2, 0, 8, 0, 0, 0, 0, 0 };
		uint32_t pos = 0;
		const struct mt7925_tlv *t;

		t = mt7925_tlv_next(body, 12, &pos);
		CHECK(t && t->tag == 1 && pos == 4, "tlv 1");
		t = mt7925_tlv_next(body, 12, &pos);
		CHECK(t && t->tag == 2 && pos == 12, "tlv 2");
		CHECK(!mt7925_tlv_next(body, 12, &pos), "tlv end");
		body[2] = 0;
		pos = 0;
		CHECK(!mt7925_tlv_next(body, 12, &pos), "zero length stops");
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Per-frame descriptors                                                */
/* ------------------------------------------------------------------ */

static void release_cb(uint16_t token, uint8_t stat, void *arg)
{
	uint32_t *acc = arg;

	acc[0]++;
	acc[1] += token;
	acc[2] += stat;
}

static int test_mac(void)
{
	uint8_t txwi[MT_TXWI_SIZE], frame[64], rx[256];
	struct mt7925_txd_params p;
	struct mt7925_rx_info ri;
	uint32_t acc[3] = { 0, 0, 0 }, w;
	int ret;

	/* 802.3 data */
	anx_memset(frame, 0, sizeof(frame));
	frame[12] = 0x08;
	anx_memset(&p, 0, sizeof(p));
	p.wlan_idx = 1;
	p.band_idx = 0xff;
	p.q_idx = 1;
	p.is_8023 = true;
	p.protect = true;
	p.pid = 1;
	mt7925_mac_write_txwi(txwi, frame, 60, 0x12345678, 7, &p);
	CHECK(le32(txwi) == (92U | (1U << 25)), "txd0 bytes and queue");
	CHECK(le32(txwi + 4) == (1U | (3U << 12) | (1U << 20)),
	      "txd1: wlan, TGID from band 0xff, Ethernet II");
	CHECK(le32(txwi + 8) == 0x20, "txd2 data type");
	CHECK(le32(txwi + 12) == ((15U << 11) | 2U), "txd3 protect");
	CHECK(le32(txwi + 20) == 1, "txd5 pid");
	CHECK(le32(txwi + 24) == 0x1c, "txd6 DAS, one MSDU, DIS_MAT");
	CHECK(le16(txwi + 32) == (7 | 0x8000) && le16(txwi + 34) == 0,
	      "txp msdu id");
	CHECK(le32(txwi + 40) == 0x12345678 && le16(txwi + 44) == (60 | 0x8000),
	      "txp buffer");
	CHECK(all_equal(txwi + 46, 18, 0), "txp rest zero");

	/* 802.11 authentication frame */
	anx_memset(frame, 0, sizeof(frame));
	frame[0] = 0xb0;
	anx_memset(&p, 0, sizeof(p));
	p.wlan_idx = 1;
	p.q_idx = MT_LMAC_ALTX0;
	p.rate_idx = 15;
	p.pid = 1;
	mt7925_mac_write_txwi(txwi, frame, 30, 0, 0, &p);
	w = le32(txwi + 4);
	CHECK(w == (1U | (2U << 14) | (12U << 16) | (1U << 31)),
	      "txd1: 802.11, 24-byte header, fixed rate");
	CHECK(le32(txwi) >> 25 == 0x10, "management queue");
	CHECK(le32(txwi + 8) == 0x0b, "auth subtype");
	CHECK(le32(txwi + 12) & (1U << 28), "fixed rate disables BA");
	CHECK(le32(txwi + 24) == (0x1cU | (15U << 16)), "txd6 rate index");

	/* EAPOL as 802.11 data at the minimum rate */
	frame[0] = 0x08;
	frame[1] = 0x01;
	p.q_idx = 1;
	p.min_rate = true;
	mt7925_mac_write_txwi(txwi, frame, 40, 0, 0, &p);
	CHECK(le32(txwi + 4) & (1U << 31), "EAPOL at fixed rate");
	CHECK(le32(txwi + 8) == 0x20, "data type, subtype 0");
	p.min_rate = false;
	mt7925_mac_write_txwi(txwi, frame, 40, 0, 0, &p);
	CHECK(!(le32(txwi + 4) & (1U << 31)), "unicast data uses RA");

	/* RX: normal frame with groups 4 and 3 and a 2-byte pad */
	anx_memset(rx, 0, sizeof(rx));
	w = (2U << 27) | 100;
	anx_memcpy(rx, &w, 4);
	w = 1U | (1U << 18) | (1U << 19);
	anx_memcpy(rx + 4, &w, 4);
	w = (1U << 13) | (4U << 16);
	anx_memcpy(rx + 8, &w, 4);
	w = (36U << 8) | (1U << 16);
	anx_memcpy(rx + 12, &w, 4);
	rx[32 + 16 + 12] = 120;		/* P-RXV RCPI0 */
	ret = mt7925_mac_parse_rx(rx, 100, &ri);
	CHECK(ret == 0 && ri.pkt_type == MT_PKT_TYPE_NORMAL, "normal frame");
	CHECK(ri.wlan_idx == 1 && ri.unicast && !ri.hdr_trans, "rx fields");
	CHECK(ri.band == MT_BAND_5G && ri.channel == 36, "rx channel");
	CHECK(ri.has_rssi && ri.rssi == -50, "rx rssi");
	CHECK(ri.decrypted && ri.sec_mode == 4, "rx decrypted");
	CHECK(ri.payload_off == 66 && ri.payload_len == 34, "rx payload");

	w = (190U << 8);
	anx_memcpy(rx + 12, &w, 4);
	mt7925_mac_parse_rx(rx, 100, &ri);
	CHECK(ri.band == MT_BAND_6G && ri.channel == 37, "6 GHz mapping");

	w = 1U | (1U << 18) | (1U << 19) | (1U << 24);
	anx_memcpy(rx + 4, &w, 4);
	mt7925_mac_parse_rx(rx, 100, &ri);
	CHECK(!ri.decrypted, "CLM means not decrypted");

	CHECK(mt7925_mac_parse_rx(rx, 50, &ri) != 0, "truncated groups");
	w = (1U << 23);
	anx_memcpy(rx + 8, &w, 4);
	CHECK(mt7925_mac_parse_rx(rx, 100, &ri) != 0, "A-MSDU error dropped");

	/* Classification */
	w = (7U << 27);
	anx_memcpy(rx, &w, 4);
	mt7925_mac_parse_rx(rx, 100, &ri);
	CHECK(ri.pkt_type == MT_PKT_TYPE_RX_EVENT, "event");
	w = (7U << 27) | (1U << 16);
	anx_memcpy(rx, &w, 4);
	anx_memset(rx + 8, 0, 4);
	/*
	 * Type 7 with flag 1 is exactly the software frame pattern, so the
	 * frame test wins before mt7925_queue_rx_skb() looks at the flag.
	 */
	mt7925_mac_parse_rx(rx, 100, &ri);
	CHECK(ri.pkt_type == MT_PKT_TYPE_NORMAL, "event flagged as frame");
	w = 0x3801U << 16;
	anx_memcpy(rx, &w, 4);
	mt7925_mac_parse_rx(rx, 100, &ri);
	CHECK(ri.pkt_type == MT_PKT_TYPE_NORMAL, "software frame type");

	/* TX free report: ids 5, 7, 9 */
	anx_memset(rx, 0, sizeof(rx));
	w = (6U << 27) | (3U << 16);
	anx_memcpy(rx, &w, 4);
	w = 4U << 16;
	anx_memcpy(rx + 4, &w, 4);
	w = (1U << 31) | (1U << 12);
	anx_memcpy(rx + 8, &w, 4);
	w = (1U << 30) | (2U << 28);
	anx_memcpy(rx + 12, &w, 4);
	w = 5U | (7U << 15);
	anx_memcpy(rx + 16, &w, 4);
	w = 9U | (0x7fffU << 15);
	anx_memcpy(rx + 20, &w, 4);
	mt7925_mac_parse_rx(rx, 24, &ri);
	CHECK(ri.pkt_type == MT_PKT_TYPE_TXRX_NOTIFY, "tx free type");
	ret = mt7925_mac_parse_tx_free(rx, 24, release_cb, acc);
	CHECK(ret == 3 && acc[0] == 3 && acc[1] == 21, "tx free ids");
	CHECK(acc[2] == 6, "tx free status follows the header");
	CHECK(mt7925_mac_parse_tx_free(rx, 20, release_cb, acc) == -1,
	      "truncated tx free");
	w = 3U << 16;
	anx_memcpy(rx + 4, &w, 4);
	CHECK(mt7925_mac_parse_tx_free(rx, 24, release_cb, acc) == -1,
	      "old report version");
	CHECK(mt7925_mac_lmac_queue(MT_AC_BE) == 1 &&
	      mt7925_mac_lmac_queue(MT_AC_VO) == 3, "lmac mapping");
	return 0;
}

/* ------------------------------------------------------------------ */
/* 802.11 frames                                                        */
/* ------------------------------------------------------------------ */

static uint32_t make_beacon(uint8_t *f, uint16_t cap, const uint8_t *rsn,
			    uint32_t rsn_len)
{
	static const uint8_t ies[] = {
		0, 11, 'A', 'n', 'u', 'n', 'i', 'x', '-', 'T', 'e', 's', 't',
		1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
		3, 1, 6,
		5, 4, 0, 3, 0, 0,
		50, 4, 0x30, 0x48, 0x60, 0x6c,
		221, 7, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00,
	};
	uint32_t pos;

	anx_memset(f, 0, 36);
	f[0] = 0x80;
	anx_memset(f + 4, 0xff, 6);
	anx_memcpy(f + 10, ap_mac, 6);
	anx_memcpy(f + 16, ap_mac, 6);
	f[32] = 100;
	f[34] = (uint8_t)cap;
	f[35] = (uint8_t)(cap >> 8);
	pos = 36;
	anx_memcpy(f + pos, ies, sizeof(ies));
	pos += sizeof(ies);
	anx_memcpy(f + pos, rsn, rsn_len);
	return pos + rsn_len;
}

static int test_ieee(void)
{
	static const uint8_t ap_rsn[] = {
		0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
		0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,
		0x0c, 0x00,
	};
	static const uint8_t preq_2g[] = {
		1, 8, 0x02, 0x04, 0x0b, 0x16, 0x0c, 0x12, 0x18, 0x24,
		50, 4, 0x30, 0x48, 0x60, 0x6c,
	};
	static const uint8_t preq_5g[] = {
		1, 8, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c,
	};
	uint8_t f[512], rsn[64], out[512];
	struct mt7925_bss bss, bss5;
	struct mt7925_rate_info ri;
	struct mt7925_mgmt_info mi;
	uint32_t len, pos;

	len = make_beacon(f, 0x0411, ap_rsn, sizeof(ap_rsn));
	CHECK(mt7925_ieee_parse_beacon(f, len, &bss) == 0, "parse beacon");
	CHECK(bss.ssid_len == 11 && anx_memcmp(bss.ssid, "Anunix-Test", 11)
	      == 0, "ssid");
	CHECK(anx_memcmp(bss.bssid, ap_mac, 6) == 0, "bssid");
	CHECK(bss.channel == 6 && bss.dtim_period == 3, "channel, dtim");
	CHECK(bss.beacon_int == 100 && bss.capability == 0x0411, "bint, cap");
	CHECK(bss.n_rates == 12 && bss.wmm, "rates, wmm");
	CHECK(bss.rsn_ie_len == 22 && bss.pairwise_ccmp && bss.akm_psk &&
	      bss.group_cipher == RSN_CIPHER_CCMP && bss.rsn_caps == 0x000c,
	      "rsn");
	CHECK(mt7925_ieee_bss_usable(&bss, true), "usable with psk");
	CHECK(!mt7925_ieee_bss_usable(&bss, false), "not usable open");

	{
		uint8_t r[sizeof(ap_rsn)];
		struct mt7925_bss b2;

		anx_memcpy(r, ap_rsn, sizeof(r));
		r[20] = 0xc0;			/* MFPR | MFPC */
		len = make_beacon(f, 0x0411, r, sizeof(r));
		mt7925_ieee_parse_beacon(f, len, &b2);
		CHECK(!mt7925_ieee_bss_usable(&b2, true), "PMF required");
		anx_memcpy(r, ap_rsn, sizeof(r));
		r[19] = RSN_AKM_SAE;
		len = make_beacon(f, 0x0411, r, sizeof(r));
		mt7925_ieee_parse_beacon(f, len, &b2);
		CHECK(b2.akm_sae && !b2.akm_psk &&
		      !mt7925_ieee_bss_usable(&b2, true), "SAE only");
		anx_memcpy(r, ap_rsn, sizeof(r));
		r[13] = RSN_CIPHER_TKIP;
		len = make_beacon(f, 0x0411, r, sizeof(r));
		mt7925_ieee_parse_beacon(f, len, &b2);
		CHECK(!mt7925_ieee_bss_usable(&b2, true), "TKIP pairwise");
		len = make_beacon(f, 0x0401, r, 0);
		mt7925_ieee_parse_beacon(f, len, &b2);
		CHECK(mt7925_ieee_bss_usable(&b2, false), "open network");
		f[0] = 0x40;
		CHECK(mt7925_ieee_parse_beacon(f, len, &b2) != 0,
		      "probe request rejected");
	}

	/* Rates */
	bss.band = MT_BAND_2G;
	mt7925_ieee_rates(&bss, &ri);
	CHECK(ri.supp == 0x0fff && ri.basic == 0x000f, "2 GHz rate maps");
	CHECK(ri.ra_legacy == 0x3fcf && ri.basic_rate_idx == 11,
	      "2 GHz RA and fixed rate");
	CHECK(ri.phy_type == 0x03 && ri.phymode == 0x06, "2 GHz phy");

	bss5 = bss;
	bss5.band = MT_BAND_5G;
	bss5.n_rates = 8;
	{
		static const uint8_t r5[8] = {
			0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c
		};
		anx_memcpy(bss5.rates, r5, 8);
	}
	mt7925_ieee_rates(&bss5, &ri);
	CHECK(ri.supp == 0x00ff && ri.basic == 0x0015, "5 GHz rate maps");
	CHECK(ri.ra_legacy == 0x3fc0 && ri.basic_rate_idx == 15,
	      "5 GHz RA and fixed rate");
	CHECK(ri.phy_type == 0x08 && ri.phymode == 0x01, "5 GHz phy");
	bss5.rates[0] = 0x0c;
	bss5.rates[2] = 0x18;
	bss5.rates[4] = 0x30;
	mt7925_ieee_rates(&bss5, &ri);
	CHECK(ri.basic == 0 && ri.basic_rate_idx == 15, "no basic rates");

	CHECK(mt7925_ieee_rate_table_value(0) == 0x00, "1M table value");
	CHECK(mt7925_ieee_rate_table_value(4) == 0x4b, "6M table value");
	CHECK(mt7925_ieee_rate_table_value(11) == 0x4c, "54M table value");

	/* Our RSN element follows the AP's group cipher */
	CHECK(mt7925_ieee_build_rsn(&bss, rsn, sizeof(rsn)) == 22, "rsn len");
	CHECK(anx_memcmp(rsn, wpa_v_sta_rsn, 22) == 0, "rsn bytes");
	bss.group_cipher = RSN_CIPHER_TKIP;
	mt7925_ieee_build_rsn(&bss, rsn, sizeof(rsn));
	CHECK(rsn[7] == RSN_CIPHER_TKIP, "tkip group");
	bss.group_cipher = RSN_CIPHER_CCMP;
	mt7925_ieee_build_rsn(&bss, rsn, sizeof(rsn));

	/* Association request */
	len = mt7925_ieee_build_assoc_req(out, sizeof(out), &bss, own_mac,
					  rsn, 22);
	CHECK(out[0] == 0 && out[1] == 0, "assoc fc");
	CHECK(anx_memcmp(out + 4, ap_mac, 6) == 0 &&
	      anx_memcmp(out + 10, own_mac, 6) == 0 &&
	      anx_memcmp(out + 16, ap_mac, 6) == 0, "assoc addresses");
	CHECK(le16(out + 24) == 0x0431 && le16(out + 26) == 10,
	      "capability, listen interval");
	pos = 28;
	CHECK(out[pos] == 0 && out[pos + 1] == 11, "ssid ie");
	pos += 13;
	CHECK(out[pos] == 1 && out[pos + 1] == 8 &&
	      anx_memcmp(out + pos + 2, preq_2g + 2, 8) == 0, "rates ie");
	pos += 10;
	CHECK(out[pos] == 50 && out[pos + 1] == 4, "ext rates ie");
	pos += 6;
	CHECK(anx_memcmp(out + pos, rsn, 22) == 0, "rsn ie");
	CHECK(len == pos + 22, "assoc length");

	bss.capability |= WLAN_CAPABILITY_SPECTRUM_MGMT;
	len = mt7925_ieee_build_assoc_req(out, sizeof(out), &bss, own_mac,
					  rsn, 22);
	CHECK(le16(out + 24) & WLAN_CAPABILITY_SPECTRUM_MGMT, "spectrum cap");
	pos = 28 + 13 + 10 + 6;
	CHECK(out[pos] == 33 && out[pos + 1] == 2 && out[pos + 3] == 20,
	      "power capability");
	pos += 4;
	CHECK(out[pos] == 36 && out[pos + 1] == 22 && out[pos + 2] == 1 &&
	      out[pos + 3] == 1 && out[pos + 22] == 11, "supported channels");
	pos += 24;
	CHECK(out[pos] == 48 && len == pos + 22, "rsn after channels");
	bss.capability &= (uint16_t)~WLAN_CAPABILITY_SPECTRUM_MGMT;

	/* Only rates the AP also supports */
	bss.n_rates = 4;
	len = mt7925_ieee_build_assoc_req(out, sizeof(out), &bss, own_mac,
					  NULL, 0);
	CHECK(out[41] == 1 && out[42] == 4 && out[47] != 50,
	      "no extended rates for a b-only AP");
	CHECK(len == 47, "open assoc length");
	CHECK(le16(out + 24) == 0x0431, "privacy still mirrors AP");

	/* Auth, deauth, probe IEs */
	len = mt7925_ieee_build_auth(out, sizeof(out), ap_mac, own_mac);
	CHECK(len == 30 && out[0] == 0xb0 && le16(out + 24) == 0 &&
	      le16(out + 26) == 1 && le16(out + 28) == 0, "auth frame");
	len = mt7925_ieee_build_deauth(out, sizeof(out), ap_mac, own_mac, 3);
	CHECK(len == 26 && out[0] == 0xc0 && le16(out + 24) == 3, "deauth");
	len = mt7925_ieee_build_preq_ies(MT_BAND_2G, out, sizeof(out));
	CHECK(len == sizeof(preq_2g) &&
	      anx_memcmp(out, preq_2g, len) == 0, "2 GHz probe ies");
	len = mt7925_ieee_build_preq_ies(MT_BAND_5G, out, sizeof(out));
	CHECK(len == sizeof(preq_5g) &&
	      anx_memcmp(out, preq_5g, len) == 0, "5 GHz probe ies");

	/* EAPOL data frame */
	{
		const uint8_t payload[4] = { 1, 3, 0, 0 };

		len = mt7925_ieee_build_data(out, sizeof(out), ap_mac, own_mac,
					     ap_mac, 0x888e, payload, 4, false);
		CHECK(len == 36 && le16(out) == 0x0108, "data fc");
		CHECK(out[24] == 0xaa && out[29] == 0 && out[30] == 0x88 &&
		      out[31] == 0x8e && out[32] == 1, "snap + payload");
		len = mt7925_ieee_build_data(out, sizeof(out), ap_mac, own_mac,
					     ap_mac, 0x888e, payload, 4, true);
		CHECK(le16(out) == 0x4108, "protected data fc");
	}

	/* 802.11 to Ethernet */
	{
		uint8_t d[64], eth[64];
		static const uint8_t sa[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 };

		anx_memset(d, 0, sizeof(d));
		d[0] = 0x08;
		d[1] = 0x02;			/* FromDS */
		anx_memcpy(d + 4, own_mac, 6);
		anx_memcpy(d + 10, ap_mac, 6);
		anx_memcpy(d + 16, sa, 6);
		d[24] = 0xaa; d[25] = 0xaa; d[26] = 0x03;
		d[30] = 0x08; d[31] = 0x00;
		d[32] = 0x45;
		len = mt7925_ieee_data_to_eth(d, 40, eth, sizeof(eth));
		CHECK(len == 22, "eth length");
		CHECK(anx_memcmp(eth, own_mac, 6) == 0 &&
		      anx_memcmp(eth + 6, sa, 6) == 0, "eth addresses");
		CHECK(eth[12] == 0x08 && eth[13] == 0 && eth[14] == 0x45,
		      "eth type and payload");

		anx_memmove(d + 26, d + 24, 16);
		d[0] = 0x88;			/* QoS data */
		d[24] = 0; d[25] = 0;
		len = mt7925_ieee_data_to_eth(d, 42, eth, sizeof(eth));
		CHECK(len == 22 && eth[14] == 0x45, "qos data");

		d[0] = 0xc8;			/* QoS null */
		CHECK(mt7925_ieee_data_to_eth(d, 42, eth, sizeof(eth)) == 0,
		      "null data ignored");
		d[0] = 0x88;
		d[26] = 0x42;
		CHECK(mt7925_ieee_data_to_eth(d, 42, eth, sizeof(eth)) == 0,
		      "non-SNAP ignored");
	}

	/* Management parsing */
	{
		uint8_t m[64];

		anx_memset(m, 0, sizeof(m));
		m[0] = 0xb0;
		anx_memcpy(m + 10, ap_mac, 6);
		m[26] = 2;
		CHECK(mt7925_ieee_parse_mgmt(m, 30, &mi) == 0 &&
		      mi.stype == IEEE80211_STYPE_AUTH && mi.auth_seq == 2 &&
		      mi.status == 0 &&
		      anx_memcmp(mi.sa, ap_mac, 6) == 0, "auth response");

		m[0] = 0x10;
		m[24] = 0x31; m[25] = 0x04;
		m[26] = 0; m[27] = 0;
		m[28] = 0x01; m[29] = 0xc0;
		m[30] = 221; m[31] = 7;
		m[32] = 0x00; m[33] = 0x50; m[34] = 0xf2; m[35] = 0x02;
		m[36] = 0x01; m[37] = 0x01;
		CHECK(mt7925_ieee_parse_mgmt(m, 39, &mi) == 0 &&
		      mi.stype == IEEE80211_STYPE_ASSOC_RESP && mi.aid == 1 &&
		      mi.status == 0 && mi.assoc_wmm, "assoc response");

		m[0] = 0xc0;
		m[24] = 15; m[25] = 0;
		CHECK(mt7925_ieee_parse_mgmt(m, 26, &mi) == 0 &&
		      mi.stype == IEEE80211_STYPE_DEAUTH && mi.status == 15,
		      "deauth reason");
		m[0] = 0x08;
		CHECK(mt7925_ieee_parse_mgmt(m, 26, &mi) != 0, "data rejected");
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* WPA2 handshakes                                                      */
/* ------------------------------------------------------------------ */

static int test_eapol(void)
{
	static const uint8_t ieee_pmk[32] = {
		0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef,
		0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
		0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2,
		0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e,
	};
	static struct mt7925_eapol e;
	uint8_t out[MT7925_EAPOL_MAX], pmk[32];
	uint32_t act, len;
	char hex[65];
	uint32_t i;

	/* IEEE 802.11-2020 J.4.2 */
	anx_pbkdf2_hmac_sha1("password", 8, "IEEE", 4, 4096, pmk, 32);
	CHECK(anx_memcmp(pmk, ieee_pmk, 32) == 0, "PBKDF2 vector");

	CHECK(mt7925_eapol_init(&e, "short", (const uint8_t *)"x", 1,
				wpa_v_aa, wpa_v_spa, wpa_v_sta_rsn, 22) != 0,
	      "short passphrase rejected");
	for (i = 0; i < 32; i++) {
		static const char digits[] = "0123456789abcdef";

		hex[2 * i] = digits[wpa_v_pmk[i] >> 4];
		hex[2 * i + 1] = digits[wpa_v_pmk[i] & 0xf];
	}
	hex[64] = '\0';
	CHECK(mt7925_eapol_init(&e, hex, (const uint8_t *)"x", 1, wpa_v_aa,
				wpa_v_spa, wpa_v_sta_rsn, 22) == 0 &&
	      anx_memcmp(e.pmk, wpa_v_pmk, 32) == 0, "hex PMK");
	hex[3] = 'g';
	CHECK(mt7925_eapol_init(&e, hex, (const uint8_t *)"x", 1, wpa_v_aa,
				wpa_v_spa, wpa_v_sta_rsn, 22) != 0,
	      "bad hex rejected");

	CHECK(mt7925_eapol_init(&e, WPA_V_PASSPHRASE,
				(const uint8_t *)WPA_V_SSID,
				(uint8_t)anx_strlen(WPA_V_SSID), wpa_v_aa,
				wpa_v_spa, wpa_v_sta_rsn, 22) == 0, "init");
	CHECK(anx_memcmp(e.pmk, wpa_v_pmk, 32) == 0, "PMK");
	anx_memcpy(e.snonce, wpa_v_snonce, 32);
	e.fixed_snonce = true;

	/* A message 3 before message 1 has no PTK to check against */
	act = mt7925_eapol_rx(&e, wpa_v_m3, sizeof(wpa_v_m3), out, &len);
	CHECK(act == 0 && e.last_error == MT7925_EAPOL_ERR_NO_PTK,
	      "M3 before M1");

	act = mt7925_eapol_rx(&e, wpa_v_m1, sizeof(wpa_v_m1), out, &len);
	CHECK(act == MT7925_EAPOL_SEND, "M1 answered");
	CHECK(anx_memcmp(e.ptk, wpa_v_ptk, 48) == 0, "PTK");
	CHECK(len == sizeof(wpa_v_m2) &&
	      anx_memcmp(out, wpa_v_m2, len) == 0, "M2 bytes");

	act = mt7925_eapol_rx(&e, wpa_v_m3_bad, sizeof(wpa_v_m3_bad), out,
			      &len);
	CHECK(act == 0 && e.last_error == MT7925_EAPOL_ERR_MIC,
	      "bad MIC dropped");

	act = mt7925_eapol_rx(&e, wpa_v_m3, sizeof(wpa_v_m3), out, &len);
	CHECK(act == (MT7925_EAPOL_SEND | MT7925_EAPOL_INSTALL_PTK |
		      MT7925_EAPOL_INSTALL_GTK), "M3 installs keys");
	CHECK(len == sizeof(wpa_v_m4) &&
	      anx_memcmp(out, wpa_v_m4, len) == 0, "M4 bytes");
	CHECK(anx_memcmp(mt7925_eapol_tk(&e), wpa_v_tk, 16) == 0, "TK");
	CHECK(e.gtk_len == 16 && e.gtk_idx == 1 &&
	      anx_memcmp(e.gtk, wpa_v_gtk1, 16) == 0, "GTK from M3");

	act = mt7925_eapol_rx(&e, wpa_v_m3, sizeof(wpa_v_m3), out, &len);
	CHECK(act == 0 && e.last_error == MT7925_EAPOL_ERR_REPLAY,
	      "replayed M3 dropped");

	act = mt7925_eapol_rx(&e, wpa_v_g1, sizeof(wpa_v_g1), out, &len);
	CHECK(act == (MT7925_EAPOL_SEND | MT7925_EAPOL_INSTALL_GTK),
	      "group rekey");
	CHECK(len == sizeof(wpa_v_g2) &&
	      anx_memcmp(out, wpa_v_g2, len) == 0, "group message 2 bytes");
	CHECK(e.gtk_len == 16 && e.gtk_idx == 2 &&
	      anx_memcmp(e.gtk, wpa_v_gtk2, 16) == 0, "rekeyed GTK");

	CHECK(mt7925_eapol_rx(&e, wpa_v_m1, 50, out, &len) == 0 &&
	      e.last_error == MT7925_EAPOL_ERR_FORMAT, "short frame");

	mt7925_eapol_clear(&e);
	CHECK(!e.ptk_valid && all_equal(e.ptk, 48, 0) &&
	      all_equal(e.pmk, 32, 0), "keys cleared");
	return 0;
}

int test_mt7925(void)
{
	if (test_layouts())
		return -1;
	if (test_uni_txd())
		return -2;
	if (test_uni_bss())
		return -3;
	if (test_uni_sta())
		return -4;
	if (test_uni_misc())
		return -5;
	if (test_mac())
		return -6;
	if (test_ieee())
		return -7;
	if (test_eapol())
		return -8;
	return 0;
}
