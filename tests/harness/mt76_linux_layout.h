/*
 * mt76_linux_layout.h — Wire structures copied from Linux v6.19
 * drivers/net/wireless/mediatek/mt76 (ISC / BSD-3-Clause-Clear), for
 * checking the MT7925 port's layouts field by field.
 *
 * Copied verbatim apart from the lnx_ prefix on each tag and, for structs
 * Linux declares inside a function, a name taken from that function. Only
 * the kernel type spellings are mapped below.
 */

#ifndef MT76_LINUX_LAYOUT_H
#define MT76_LINUX_LAYOUT_H

#include <anx/types.h>

typedef uint8_t  u8;
typedef int8_t   s8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define ETH_ALEN 6
#define IEEE80211_MAX_SSID_LEN 32
#define MT7925_RNR_SCAN_MAX_BSSIDS 10
#define MT7925_EEPROM_BLOCK_SIZE 16
#define MT_CONNAC3_SKU_POWER_LIMIT 449
#define MT_HW_TXP_MAX_MSDU_NUM 4
#define MT_HW_TXP_MAX_BUF_NUM 4

/* mt76_connac_mcu.h */
struct lnx_mt76_connac2_mcu_uni_txd {
	__le32 txd[8];

	/* DW1 */
	__le16 len;
	__le16 cid;

	/* DW2 */
	u8 rsv;
	u8 pkt_type;
	u8 frag_n;
	u8 seq;

	/* DW3 */
	__le16 checksum;
	u8 s2d_index;
	u8 option;

	/* DW4 */
	u8 rsv1[4];
} __packed __aligned(4);

/* mt7925/mcu.h */
struct lnx_mt7925_mcu_rxd {
	__le32 rxd[8];

	__le16 len;
	__le16 pkt_type_id;

	u8 eid;
	u8 seq;
	u8 option;
	u8 __rsv;

	u8 ext_eid;
	u8 __rsv1[2];
	u8 s2d_index;

	u8 tlv[];
};

struct lnx_mt7925_mcu_uni_event {
	u8 cid;
	u8 pad[3];
	__le32 status; /* 0: success, others: fail */
} __packed;

struct lnx_sta_req_hdr {
	u8 bss_idx;
	u8 wlan_idx_lo;
	__le16 tlv_num;
	u8 is_tlv_append;
	u8 muar_idx;
	u8 wlan_idx_hi;
	u8 rsv;
} __packed;

struct lnx_sta_rec_basic {
	__le16 tag;
	__le16 len;
	__le32 conn_type;
	u8 conn_state;
	u8 qos;
	__le16 aid;
	u8 peer_addr[ETH_ALEN];
	__le16 extra_info;
} __packed;

struct lnx_sta_rec_phy {
	__le16 tag;
	__le16 len;
	__le16 basic_rate;
	u8 phy_type;
	u8 ampdu;
	u8 rts_policy;
	u8 rcpi;
	u8 max_ampdu_len; /* connac3 */
	u8 rsv[1];
} __packed;

#define HT_MCS_MASK_NUM 10
struct lnx_sta_rec_ra_info {
	__le16 tag;
	__le16 len;
	__le16 legacy;
	u8 rx_mcs_bitmask[HT_MCS_MASK_NUM];
} __packed;

/* mt7925_mcu_sta_state_v2_tlv() */
struct lnx_sta_rec_state_v2 {
	__le16 tag;
	__le16 len;
	u8 state;
	u8 rsv[3];
	__le32 flags;
	u8 vht_opmode;
	u8 action;
	u8 rsv2[2];
} __packed;

struct lnx_sta_rec_mld {
	__le16 tag;
	__le16 len;
	u8 mac_addr[ETH_ALEN];
	__le16 primary_id;
	__le16 secondary_id;
	__le16 wlan_id;
	u8 link_num;
	u8 rsv[3];
	struct {
		__le16 wlan_id;
		u8 bss_idx;
		u8 rsv;
	} __packed link[2];
} __packed;

struct lnx_sta_rec_hdr_trans {
	__le16 tag;
	__le16 len;
	u8 from_ds;
	u8 to_ds;
	u8 dis_rx_hdr_tran;
	u8 rsv;
} __packed;

struct lnx_sta_rec_remove {
	__le16 tag;
	__le16 len;
	u8 action;
	u8 pad[3];
} __packed;

struct lnx_sta_rec_sec_uni {
	__le16 tag;
	__le16 len;
	u8 add;
	u8 tx_key;
	u8 key_type;
	u8 is_authenticator;
	u8 peer_addr[6];
	u8 bss_idx;
	u8 cipher_id;
	u8 key_id;
	u8 key_len;
	u8 wlan_idx;
	u8 mgmt_prot;
	u8 key[32];
	u8 key_rsc[16];
} __packed;

struct lnx_bss_req_hdr {
	u8 bss_idx;
	u8 __rsv[3];
} __packed;

struct lnx_mt76_connac_bss_basic_tlv {
	__le16 tag;
	__le16 len;
	u8 active;
	u8 omac_idx;
	u8 hw_bss_idx;
	u8 band_idx;
	__le32 conn_type;
	u8 conn_state;
	u8 wmm_idx;
	u8 bssid[ETH_ALEN];
	__le16 bmc_tx_wlan_idx;
	__le16 bcn_interval;
	u8 dtim_period;
	u8 phymode;
	__le16 sta_idx;
	__le16 nonht_basic_phy;
	u8 phymode_ext; /* bit(0) AX_6G */
	u8 link_idx;
} __packed;

/* mt7925_mcu_bss_sec_tlv() */
struct lnx_bss_sec_tlv {
	__le16 tag;
	__le16 len;
	u8 mode;
	u8 status;
	u8 cipher;
	u8 __rsv;
} __packed;

struct lnx_bss_rate_tlv {
	__le16 tag;
	__le16 len;
	u8 __rsv1[2];
	__le16 basic_rate;
	__le16 bc_trans;
	__le16 mc_trans;
	u8 short_preamble;
	u8 bc_fixed_rate;
	u8 mc_fixed_rate;
	u8 __rsv2;
} __packed;

struct lnx_mt76_connac_bss_qos_tlv {
	__le16 tag;
	__le16 len;
	u8 qos;
	u8 pad[3];
} __packed;

struct lnx_bss_mld_tlv {
	__le16 tag;
	__le16 len;
	u8 group_mld_id;
	u8 own_mld_id;
	u8 mac_addr[ETH_ALEN];
	u8 remap_idx;
	u8 link_id;
	u8 eml_enable;
	u8 max_link_num;
	u8 hybrid_mode;
	u8 __rsv[3];
} __packed;

struct lnx_bss_ifs_time_tlv {
	__le16 tag;
	__le16 len;
	u8 slot_valid;
	u8 sifs_valid;
	u8 rifs_valid;
	u8 eifs_valid;
	__le16 slot_time;
	__le16 sifs_time;
	__le16 rifs_time;
	__le16 eifs_time;
	u8 eifs_cck_valid;
	u8 rsv;
	__le16 eifs_cck_time;
} __packed;

struct lnx_bss_rlm_tlv {
	__le16 tag;
	__le16 len;
	u8 control_channel;
	u8 center_chan;
	u8 center_chan2;
	u8 bw;
	u8 tx_streams;
	u8 rx_streams;
	u8 ht_op_info;
	u8 sco;
	u8 band;
	u8 pad[3];
} __packed;

struct lnx_bss_info_uni_mbssid {
	__le16 tag;
	__le16 len;
	u8 max_indicator;
	u8 mbss_idx;
	u8 tx_bss_omac_idx;
	u8 rsv;
} __packed;

/* mt7925_mcu_uni_bss_bcnft() */
struct lnx_bcnft_req {
	struct {
		u8 bss_idx;
		u8 pad[3];
	} __packed hdr;
	struct bcnft_tlv {
		__le16 tag;
		__le16 len;
		__le16 bcn_interval;
		u8 dtim_period;
		u8 bmc_delivered_ac;
		u8 bmc_triggered_ac;
		u8 pad[3];
	} __packed bcnft;
} __packed;

/* mt76_connac_mcu_uni_add_dev() */
struct lnx_dev_req {
	struct {
		u8 omac_idx;
		u8 band_idx;
		__le16 pad;
	} __packed hdr;
	struct req_tlv {
		__le16 tag;
		__le16 len;
		u8 active;
		u8 link_idx; /* not link_id */
		u8 omac_addr[ETH_ALEN];
	} __packed tlv;
};

struct lnx_roc_acquire_tlv {
	__le16 tag;
	__le16 len;
	u8 bss_idx;
	u8 tokenid;
	u8 control_channel;
	u8 sco;
	u8 band;
	u8 bw;
	u8 center_chan;
	u8 center_chan2;
	u8 bw_from_ap;
	u8 center_chan_from_ap;
	u8 center_chan2_from_ap;
	u8 reqtype;
	__le32 maxinterval;
	u8 dbdcband;
	u8 rsv[3];
} __packed;

/* mt7925_mcu_abort_roc() */
struct lnx_roc_abort_tlv {
	__le16 tag;
	__le16 len;
	u8 bss_idx;
	u8 tokenid;
	u8 dbdcband;
	u8 rsv[5];
} __packed;

/* mt7925/mt7925.h */
struct lnx_mt7925_roc_grant_tlv {
	__le16 tag;
	__le16 len;
	u8 bss_idx;
	u8 tokenid;
	u8 status;
	u8 primarychannel;
	u8 rfsco;
	u8 rfband;
	u8 channelwidth;
	u8 centerfreqseg1;
	u8 centerfreqseg2;
	u8 reqtype;
	u8 dbdcband;
	u8 rsv[1];
	__le32 max_interval;
} __packed;

struct lnx_scan_hdr_tlv {
	/* fixed field */
	u8 seq_num;
	u8 bss_idx;
	u8 pad[2];
	/* tlv */
	u8 data[];
} __packed;

struct lnx_scan_req_tlv {
	__le16 tag;
	__le16 len;

	u8 scan_type;
	u8 probe_req_num;
	u8 scan_func;
	u8 src_mask;
	__le16 channel_min_dwell_time;
	__le16 channel_dwell_time; /* channel Dwell interval */
	__le16 timeout_value;
	__le16 probe_delay_time;
	__le32 func_mask_ext;
} __packed;

struct lnx_mt76_connac_mcu_scan_ssid {
	__le32 ssid_len;
	u8 ssid[IEEE80211_MAX_SSID_LEN];
} __packed;

struct lnx_scan_ssid_tlv {
	__le16 tag;
	__le16 len;

	u8 ssid_type;
	u8 ssids_num;
	u8 is_short_ssid;
	u8 pad;
	struct lnx_mt76_connac_mcu_scan_ssid ssids[MT7925_RNR_SCAN_MAX_BSSIDS];
} __packed;

struct lnx_scan_bssid_tlv {
	__le16 tag;
	__le16 len;

	u8 bssid[ETH_ALEN];
	u8 match_ch;
	u8 match_ssid_ind;
	u8 rcpi;
	u8 match_short_ssid_ind;
	u8 pad[2];
} __packed;

struct lnx_mt76_connac_mcu_scan_channel {
	u8 band;
	u8 channel_num;
} __packed;

struct lnx_scan_chan_info_tlv {
	__le16 tag;
	__le16 len;

	u8 channel_type;
	u8 channels_num;
	u8 pad[2];
	struct lnx_mt76_connac_mcu_scan_channel channels[64];
} __packed;

struct lnx_scan_ie_tlv {
	__le16 tag;
	__le16 len;

	__le16 ies_len;
	u8 band;
	u8 pad;
	u8 ies[];
};

struct lnx_scan_misc_tlv {
	__le16 tag;
	__le16 len;

	u8 random_mac[ETH_ALEN];
	u8 rsv[2];
};

struct lnx_edca {
	__le16 tag;
	__le16 len;

	u8 queue;
	u8 set;
	u8 cw_min;
	u8 cw_max;
	__le16 txop;
	u8 aifs;
	u8 __rsv;
};

struct lnx_mt76_connac_config {
	__le16 id;
	u8 type;
	u8 resp_type;
	__le16 data_size;
	__le16 resv;
	u8 data[320];
} __packed;

/* mt7925_mcu_chip_config() */
struct lnx_chip_config_req {
	u8 _rsv[4];
	__le16 tag;
	__le16 len;
	struct lnx_mt76_connac_config config;
} __packed;

/* mt7925_mcu_set_rts_thresh() */
struct lnx_rts_req {
	u8 band_idx;
	u8 _rsv[3];

	__le16 tag;
	__le16 len;
	__le32 len_thresh;
	__le32 pkt_thresh;
} __packed;

/* mt7925_mcu_set_rxfilter() */
struct lnx_rxfilter_req {
	u8 band_idx;
	u8 rsv1[3];

	__le16 tag;
	__le16 len;
	u8 mode;
	u8 rsv2[3];
	__le32 fif;
	__le32 bit_map; /* bit_* for bitmap update */
	u8 bit_op;
	u8 pad[51];
} __packed;

/* mt7925_mcu_fw_log_2_host() */
struct lnx_fw_log_req {
	u8 _rsv[4];

	__le16 tag;
	__le16 len;
	u8 ctrl;
	u8 interval;
	u8 _rsv2[2];
} __packed;

/* mt7925_mcu_set_eeprom() */
struct lnx_eeprom_req {
	u8 _rsv[4];

	__le16 tag;
	__le16 len;
	u8 buffer_mode;
	u8 format;
	__le16 buf_len;
} __packed;

/* mt7925_mcu_read_eeprom() */
struct lnx_efuse_read_req {
	u8 rsv[4];

	__le16 tag;
	__le16 len;

	__le32 addr;
	__le32 valid;
	u8 data[MT7925_EEPROM_BLOCK_SIZE];
} __packed;

struct lnx_efuse_read_evt {
	u8 rsv[4];

	__le16 tag;
	__le16 len;

	__le32 ver;
	__le32 addr;
	__le32 valid;
	__le32 size;
	__le32 magic_num;
	__le32 type;
	__le32 rsv1[4];
	u8 data[32];
} __packed;

/* mt7925_mcu_get_nic_capability() */
struct lnx_nic_cap_req {
	u8 _rsv[4];

	__le16 tag;
	__le16 len;
} __packed;

struct lnx_mt76_connac_cap_hdr {
	__le16 n_element;
	u8 rsv[2];
} __packed;

/* mt7925_mcu_set_channel_domain() */
struct lnx_domain_req {
	struct {
		u8 alpha2[4]; /* regulatory_request.alpha2 */
		u8 bw_2g;
		u8 bw_5g;
		u8 bw_6g;
		u8 pad;
	} __packed hdr;
	struct n_chan {
		__le16 tag;
		__le16 len;
		u8 n_2ch;
		u8 n_5ch;
		u8 n_6ch;
		u8 pad;
	} __packed n_ch;
};

struct lnx_mt76_connac_mcu_chan {
	__le16 hw_value;
	__le16 pad;
	__le32 flags;
} __packed;

struct lnx_mt7925_tx_power_limit_tlv {
	u8 rsv[4];

	__le16 tag;
	__le16 len;

	/* DW0 - common info*/
	u8 ver;
	u8 pad0;
	__le16 rsv1;
	/* DW1 - cmd hint */
	u8 n_chan; /* # channel */
	u8 band; /* 2.4GHz - 5GHz - 6GHz */
	u8 last_msg;
	u8 limit_type;
	/* DW3 */
	u8 alpha2[4]; /* regulatory_request.alpha2 */
	u8 pad2[32];

	u8 data[];
} __packed;

struct lnx_mt7925_sku_tlv {
	u8 channel;
	s8 pwr_limit[MT_CONNAC3_SKU_POWER_LIMIT];
} __packed;

/* __mt7925_mcu_set_clc() */
struct lnx_clc_req {
	u8 rsv[4];
	__le16 tag;
	__le16 len;

	u8 ver;
	u8 pad0;
	__le16 size;
	u8 idx;
	u8 env;
	u8 acpi_conf;
	u8 pad1;
	u8 alpha2[2];
	u8 type[2];
	u8 rsvd[64];
} __packed;

struct lnx_mt7925_clc_rule {
	u8 alpha2[2];
	u8 type[2];
	u8 seg_idx;
	u8 flag; /* UNII4~8 ctrl flag */
	u8 rsv[2];
} __packed;

struct lnx_mt7925_clc_segment {
	u8 idx;
	u8 rsv1[3];
	u32 offset;
	u32 len;
	u8 rsv2[4];
} __packed;

struct lnx_mt7925_clc_type0 {
	u8 nr_country;
	u8 type;
	u8 nr_seg;
	u8 rsv[7];
} __packed;

struct lnx_mt7925_clc_type2 {
	u8 type;
	u8 rsv[9];
} __packed;

struct lnx_mt7925_clc {
	__le32 len;
	u8 idx;
	u8 ver;
	union {
		struct lnx_mt7925_clc_type0 t0;
		struct lnx_mt7925_clc_type2 t2;
	};
	u8 data[];
} __packed;

/* mt76_connac.h */
struct lnx_mt76_connac_txp_ptr {
	__le32 buf0;
	__le16 len0;
	__le16 len1;
	__le32 buf1;
} __packed __aligned(4);

struct lnx_mt76_connac_hw_txp {
	__le16 msdu_id[MT_HW_TXP_MAX_MSDU_NUM];
	struct lnx_mt76_connac_txp_ptr ptr[MT_HW_TXP_MAX_BUF_NUM / 2];
} __packed __aligned(4);

#endif /* MT76_LINUX_LAYOUT_H */
