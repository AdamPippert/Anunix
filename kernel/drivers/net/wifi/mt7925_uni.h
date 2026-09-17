/*
 * mt7925_uni.h — MT7925 unified-command (UNI) wire formats and builders.
 *
 * Once the WM firmware is running, the MT7925 takes "unified" commands: a
 * 48-byte descriptor, then a command-specific fixed header, then TLVs. This
 * file owns the byte layout of every command the station path sends. The
 * builders here touch no hardware, so the host test suite checks them field
 * by field against the Linux v6.19 mt76 structures they are ported from
 * (tests/harness/mt76_linux_layout.h).
 *
 * Sources, Linux v6.19 drivers/net/wireless/mediatek/mt76:
 *   mt76_connac_mcu.h, mt7925/mcu.h, mt7925/mcu.c, mt76_connac_mcu.c.
 */

#ifndef ANX_MT7925_UNI_H
#define ANX_MT7925_UNI_H

#include <anx/types.h>

/* ------------------------------------------------------------------ */
/* Command identifiers (mt76_connac_mcu.h)                              */
/* ------------------------------------------------------------------ */

#define MT_UNI_CMD_DEV_INFO_UPDATE	0x01
#define MT_UNI_CMD_BSS_INFO_UPDATE	0x02
#define MT_UNI_CMD_STA_REC_UPDATE	0x03
#define MT_UNI_CMD_EDCA_UPDATE		0x04
#define MT_UNI_CMD_BAND_CONFIG		0x08
#define MT_UNI_CMD_WSYS_CONFIG		0x0b
#define MT_UNI_CMD_CHIP_CONFIG		0x0e
#define MT_UNI_CMD_SET_DOMAIN_INFO	0x15
#define MT_UNI_CMD_SCAN_REQ		0x16
#define MT_UNI_CMD_ROC			0x27
#define MT_UNI_CMD_SET_POWER_LIMIT	0x2c
#define MT_UNI_CMD_EFUSE_CTRL		0x2d

/* Descriptor option bits (MCU_CMD_UNI_EXT_ACK / MCU_CMD_UNI_QUERY_ACK) */
#define MT_UNI_OPT_ACK			0x01
#define MT_UNI_OPT_UNI			0x02
#define MT_UNI_OPT_SET			0x04
#define MT_UNI_OPT_SET_ACK		(MT_UNI_OPT_ACK | MT_UNI_OPT_UNI | MT_UNI_OPT_SET)
#define MT_UNI_OPT_QUERY_ACK		(MT_UNI_OPT_ACK | MT_UNI_OPT_UNI)

/* Unified events (mt76_connac_mcu.h) */
#define MT_UNI_EVENT_RESULT		0x01
#define MT_UNI_EVENT_HIF_CTRL		0x03
#define MT_UNI_EVENT_FW_LOG_2_HOST	0x04
#define MT_UNI_EVENT_COREDUMP		0x0a
#define MT_UNI_EVENT_BSS_BEACON_LOSS	0x0c
#define MT_UNI_EVENT_SCAN_DONE		0x0e
#define MT_UNI_EVENT_ROC		0x27
#define MT_UNI_EVENT_TX_DONE		0x2d
#define MT_UNI_EVENT_RSSI_MONITOR	0x41

/* mt7925_mcu_rxd.option */
#define MT_UNI_EVENT_OPT_UNSOLICITED	0x04

/* TLV tags */
#define MT_UNI_CHIP_CONFIG_CHIP_CFG	0x2
#define MT_UNI_CHIP_CONFIG_NIC_CAPA	0x3
#define MT_UNI_BAND_CONFIG_RTS_THRESHOLD 0x08
#define MT_UNI_BAND_CONFIG_RX_FILTER	0x0c
#define MT_UNI_WSYS_CONFIG_FW_LOG_CTRL	0x0
#define MT_UNI_EFUSE_ACCESS		0x1
#define MT_UNI_EFUSE_BUFFER_MODE	0x2
#define MT_UNI_POWER_LIMIT_TABLE	0x1
#define MT_UNI_POWER_LIMIT_CLC		0x3
#define MT_UNI_DOMAIN_CHANNELS		0x2
#define MT_DEV_INFO_ACTIVE		0

#define MT_UNI_BSS_INFO_BASIC		0
#define MT_UNI_BSS_INFO_RLM		2
#define MT_UNI_BSS_INFO_11V_MBSSID	6
#define MT_UNI_BSS_INFO_RATE		11
#define MT_UNI_BSS_INFO_QBSS		15
#define MT_UNI_BSS_INFO_SEC		16
#define MT_UNI_BSS_INFO_BCNFT		22
#define MT_UNI_BSS_INFO_IFS_TIME	23
#define MT_UNI_BSS_INFO_MLD		26
#define MT_UNI_BSS_INFO_PM_DISABLE	27

#define MT_STA_REC_BASIC		0x00
#define MT_STA_REC_RA			0x01
#define MT_STA_REC_STATE		0x07
#define MT_STA_REC_PHY			0x15
#define MT_STA_REC_MLD			0x20
#define MT_STA_REC_MLD_OFF		0x23
#define MT_STA_REC_REMOVE		0x25
#define MT_STA_REC_KEY_V3		0x27
#define MT_STA_REC_HDR_TRANS		0x2b

#define MT_UNI_SCAN_REQ			1
#define MT_UNI_SCAN_SSID		10
#define MT_UNI_SCAN_BSSID		11
#define MT_UNI_SCAN_CHANNEL		12
#define MT_UNI_SCAN_IE			13
#define MT_UNI_SCAN_MISC		14
#define MT_SCAN_FUNC_SPLIT_SCAN		(1U << 5)

#define MT_UNI_ROC_ACQUIRE		0
#define MT_UNI_ROC_ABORT		1
#define MT_ROC_REQ_JOIN			0
#define MT_UNI_EVENT_ROC_GRANT		0

#define MT_UNI_EVENT_SCAN_DONE_BASIC	0

/* NIC capability tags (mt76_connac_mcu.h) */
#define MT_NIC_CAP_MAC_ADDR		7
#define MT_NIC_CAP_PHY			8
#define MT_NIC_CAP_6G			0x18
#define MT_NIC_CAP_CHIP_CAP		0x20

/* Connection types and states */
#define MT_STA_TYPE_STA			(1U << 0)
#define MT_STA_TYPE_AP			(1U << 1)
#define MT_NETWORK_INFRA		(1U << 16)
#define MT_CONNECTION_INFRA_STA		(MT_STA_TYPE_STA | MT_NETWORK_INFRA)
#define MT_CONNECTION_INFRA_AP		(MT_STA_TYPE_AP | MT_NETWORK_INFRA)
#define MT_CONN_STATE_DISCONNECT	0
#define MT_CONN_STATE_PORT_SECURE	2

/* mt76_sta_info_state */
#define MT_STA_INFO_STATE_NONE		0
#define MT_STA_INFO_STATE_ASSOC		2

#define MT_EXTRA_INFO_VER		(1U << 0)
#define MT_EXTRA_INFO_NEW		(1U << 1)

/* PHY mode bits for bss basic (mt76_connac_get_phy_mode) */
#define MT_PHY_MODE_A			(1U << 0)
#define MT_PHY_MODE_B			(1U << 1)
#define MT_PHY_MODE_G			(1U << 2)

/* PHY type bits for sta phy (PHY_TYPE_BIT_*) */
#define MT_PHY_TYPE_BIT_HR_DSSS		(1U << 0)
#define MT_PHY_TYPE_BIT_ERP		(1U << 1)
#define MT_PHY_TYPE_BIT_OFDM		(1U << 3)
#define MT_PHY_TYPE_ERP_INDEX		1
#define MT_PHY_TYPE_OFDM_INDEX		3

#define MT_HR_DSSS_ERP_BASIC_RATE	0x000f
#define MT_OFDM_BASIC_RATE		0x0540	/* BIT(6) | BIT(8) | BIT(10) */

/* connac3 cipher ids (mt7925/mcu.h) */
#define MT_CIPHER_NONE			0
#define MT_CIPHER_TKIP			2
#define MT_CIPHER_AES_CCMP		4

/* bss_sec_tlv modes (mt7925/mt7925.h) */
#define MT_SEC_MODE_OPEN		0
#define MT_SEC_MODE_WPA2_PSK		7

#define MT_CMD_CBW_20MHZ		0

/* Band numbering used by every UNI command. */
#define MT_BAND_2G			1
#define MT_BAND_5G			2
#define MT_BAND_6G			3

/* Rate-control legacy bitmap layout (RA_LEGACY_*) */
#define MT_RA_LEGACY_CCK_MASK		0x000f
#define MT_RA_LEGACY_OFDM_SHIFT		6

#define MT_CHAN_FLAG_NO_IR		(1U << 1)
#define MT_CHAN_FLAG_RADAR		(1U << 3)

#define MT_SKU_POWER_LIMIT		449
#define MT_EEPROM_BLOCK_SIZE		16

/* ------------------------------------------------------------------ */
/* Wire structures                                                      */
/* ------------------------------------------------------------------ */

/* struct mt76_connac2_mcu_uni_txd */
struct mt7925_uni_txd {
	uint32_t txd[8];
	uint16_t len;
	uint16_t cid;
	uint8_t  rsv;
	uint8_t  pkt_type;
	uint8_t  frag_n;
	uint8_t  seq;
	uint16_t checksum;
	uint8_t  s2d_index;
	uint8_t  option;
	uint8_t  rsv1[4];
} __attribute__((packed));

/* struct mt7925_mcu_rxd; the event payload starts at sizeof(). */
struct mt7925_mcu_rxd {
	uint32_t rxd[8];
	uint16_t len;
	uint16_t pkt_type_id;
	uint8_t  eid;
	uint8_t  seq;
	uint8_t  option;
	uint8_t  rsv;
	uint8_t  ext_eid;
	uint8_t  rsv1[2];
	uint8_t  s2d_index;
} __attribute__((packed));

/* struct mt7925_mcu_uni_event */
struct mt7925_uni_event {
	uint8_t  cid;
	uint8_t  pad[3];
	uint32_t status;
} __attribute__((packed));

struct mt7925_tlv {
	uint16_t tag;
	uint16_t len;
} __attribute__((packed));

/* struct sta_req_hdr */
struct mt7925_sta_req_hdr {
	uint8_t  bss_idx;
	uint8_t  wlan_idx_lo;
	uint16_t tlv_num;
	uint8_t  is_tlv_append;
	uint8_t  muar_idx;
	uint8_t  wlan_idx_hi;
	uint8_t  rsv;
} __attribute__((packed));

/* struct sta_rec_basic */
struct mt7925_sta_rec_basic {
	uint16_t tag;
	uint16_t len;
	uint32_t conn_type;
	uint8_t  conn_state;
	uint8_t  qos;
	uint16_t aid;
	uint8_t  peer_addr[6];
	uint16_t extra_info;
} __attribute__((packed));

/* struct sta_rec_phy */
struct mt7925_sta_rec_phy {
	uint16_t tag;
	uint16_t len;
	uint16_t basic_rate;
	uint8_t  phy_type;
	uint8_t  ampdu;
	uint8_t  rts_policy;
	uint8_t  rcpi;
	uint8_t  max_ampdu_len;
	uint8_t  rsv[1];
} __attribute__((packed));

/* struct sta_rec_ra_info */
struct mt7925_sta_rec_ra {
	uint16_t tag;
	uint16_t len;
	uint16_t legacy;
	uint8_t  rx_mcs_bitmask[10];
} __attribute__((packed));

/* struct sta_rec_state_v2 (local to mt7925_mcu_sta_state_v2_tlv) */
struct mt7925_sta_rec_state {
	uint16_t tag;
	uint16_t len;
	uint8_t  state;
	uint8_t  rsv[3];
	uint32_t flags;
	uint8_t  vht_opmode;
	uint8_t  action;
	uint8_t  rsv2[2];
} __attribute__((packed));

/* struct sta_rec_mld */
struct mt7925_sta_rec_mld {
	uint16_t tag;
	uint16_t len;
	uint8_t  mac_addr[6];
	uint16_t primary_id;
	uint16_t secondary_id;
	uint16_t wlan_id;
	uint8_t  link_num;
	uint8_t  rsv[3];
	struct {
		uint16_t wlan_id;
		uint8_t  bss_idx;
		uint8_t  rsv;
	} __attribute__((packed)) link[2];
} __attribute__((packed));

/* struct sta_rec_hdr_trans */
struct mt7925_sta_rec_hdr_trans {
	uint16_t tag;
	uint16_t len;
	uint8_t  from_ds;
	uint8_t  to_ds;
	uint8_t  dis_rx_hdr_tran;
	uint8_t  rsv;
} __attribute__((packed));

/* struct sta_rec_remove */
struct mt7925_sta_rec_remove {
	uint16_t tag;
	uint16_t len;
	uint8_t  action;
	uint8_t  pad[3];
} __attribute__((packed));

/* struct sta_rec_sec_uni */
struct mt7925_sta_rec_key {
	uint16_t tag;
	uint16_t len;
	uint8_t  add;
	uint8_t  tx_key;
	uint8_t  key_type;
	uint8_t  is_authenticator;
	uint8_t  peer_addr[6];
	uint8_t  bss_idx;
	uint8_t  cipher_id;
	uint8_t  key_id;
	uint8_t  key_len;
	uint8_t  wlan_idx;
	uint8_t  mgmt_prot;
	uint8_t  key[32];
	uint8_t  key_rsc[16];
} __attribute__((packed));

/* struct bss_req_hdr */
struct mt7925_bss_req_hdr {
	uint8_t  bss_idx;
	uint8_t  rsv[3];
} __attribute__((packed));

/* struct mt76_connac_bss_basic_tlv */
struct mt7925_bss_basic {
	uint16_t tag;
	uint16_t len;
	uint8_t  active;
	uint8_t  omac_idx;
	uint8_t  hw_bss_idx;
	uint8_t  band_idx;
	uint32_t conn_type;
	uint8_t  conn_state;
	uint8_t  wmm_idx;
	uint8_t  bssid[6];
	uint16_t bmc_tx_wlan_idx;
	uint16_t bcn_interval;
	uint8_t  dtim_period;
	uint8_t  phymode;
	uint16_t sta_idx;
	uint16_t nonht_basic_phy;
	uint8_t  phymode_ext;
	uint8_t  link_idx;
} __attribute__((packed));

/* bss_sec_tlv (local to mt7925_mcu_bss_sec_tlv) */
struct mt7925_bss_sec {
	uint16_t tag;
	uint16_t len;
	uint8_t  mode;
	uint8_t  status;
	uint8_t  cipher;
	uint8_t  rsv;
} __attribute__((packed));

/* struct bss_rate_tlv */
struct mt7925_bss_rate {
	uint16_t tag;
	uint16_t len;
	uint8_t  rsv1[2];
	uint16_t basic_rate;
	uint16_t bc_trans;
	uint16_t mc_trans;
	uint8_t  short_preamble;
	uint8_t  bc_fixed_rate;
	uint8_t  mc_fixed_rate;
	uint8_t  rsv2;
} __attribute__((packed));

/* struct mt76_connac_bss_qos_tlv */
struct mt7925_bss_qos {
	uint16_t tag;
	uint16_t len;
	uint8_t  qos;
	uint8_t  pad[3];
} __attribute__((packed));

/* struct bss_mld_tlv */
struct mt7925_bss_mld {
	uint16_t tag;
	uint16_t len;
	uint8_t  group_mld_id;
	uint8_t  own_mld_id;
	uint8_t  mac_addr[6];
	uint8_t  remap_idx;
	uint8_t  link_id;
	uint8_t  eml_enable;
	uint8_t  max_link_num;
	uint8_t  hybrid_mode;
	uint8_t  rsv[3];
} __attribute__((packed));

/* struct bss_ifs_time_tlv */
struct mt7925_bss_ifs {
	uint16_t tag;
	uint16_t len;
	uint8_t  slot_valid;
	uint8_t  sifs_valid;
	uint8_t  rifs_valid;
	uint8_t  eifs_valid;
	uint16_t slot_time;
	uint16_t sifs_time;
	uint16_t rifs_time;
	uint16_t eifs_time;
	uint8_t  eifs_cck_valid;
	uint8_t  rsv;
	uint16_t eifs_cck_time;
} __attribute__((packed));

/* struct bss_rlm_tlv */
struct mt7925_bss_rlm {
	uint16_t tag;
	uint16_t len;
	uint8_t  control_channel;
	uint8_t  center_chan;
	uint8_t  center_chan2;
	uint8_t  bw;
	uint8_t  tx_streams;
	uint8_t  rx_streams;
	uint8_t  ht_op_info;
	uint8_t  sco;
	uint8_t  band;
	uint8_t  pad[3];
} __attribute__((packed));

/* struct bss_info_uni_mbssid */
struct mt7925_bss_mbssid {
	uint16_t tag;
	uint16_t len;
	uint8_t  max_indicator;
	uint8_t  mbss_idx;
	uint8_t  tx_bss_omac_idx;
	uint8_t  rsv;
} __attribute__((packed));

/* bcnft_tlv (local to mt7925_mcu_uni_bss_bcnft) */
struct mt7925_bss_bcnft {
	uint16_t tag;
	uint16_t len;
	uint16_t bcn_interval;
	uint8_t  dtim_period;
	uint8_t  bmc_delivered_ac;
	uint8_t  bmc_triggered_ac;
	uint8_t  pad[3];
} __attribute__((packed));

/* dev_req in mt76_connac_mcu_uni_add_dev */
struct mt7925_dev_info_req {
	uint8_t  omac_idx;
	uint8_t  band_idx;
	uint16_t pad;
	uint16_t tag;
	uint16_t len;
	uint8_t  active;
	uint8_t  link_idx;
	uint8_t  omac_addr[6];
} __attribute__((packed));

/* struct roc_acquire_tlv */
struct mt7925_roc_acquire {
	uint16_t tag;
	uint16_t len;
	uint8_t  bss_idx;
	uint8_t  tokenid;
	uint8_t  control_channel;
	uint8_t  sco;
	uint8_t  band;
	uint8_t  bw;
	uint8_t  center_chan;
	uint8_t  center_chan2;
	uint8_t  bw_from_ap;
	uint8_t  center_chan_from_ap;
	uint8_t  center_chan2_from_ap;
	uint8_t  reqtype;
	uint32_t maxinterval;
	uint8_t  dbdcband;
	uint8_t  rsv[3];
} __attribute__((packed));

/* roc_abort_tlv (local to mt7925_mcu_abort_roc) */
struct mt7925_roc_abort {
	uint16_t tag;
	uint16_t len;
	uint8_t  bss_idx;
	uint8_t  tokenid;
	uint8_t  dbdcband;
	uint8_t  rsv[5];
} __attribute__((packed));

/* struct mt7925_roc_grant_tlv */
struct mt7925_roc_grant {
	uint16_t tag;
	uint16_t len;
	uint8_t  bss_idx;
	uint8_t  tokenid;
	uint8_t  status;
	uint8_t  primarychannel;
	uint8_t  rfsco;
	uint8_t  rfband;
	uint8_t  channelwidth;
	uint8_t  centerfreqseg1;
	uint8_t  centerfreqseg2;
	uint8_t  reqtype;
	uint8_t  dbdcband;
	uint8_t  rsv[1];
	uint32_t max_interval;
} __attribute__((packed));

/* struct scan_hdr_tlv */
struct mt7925_scan_hdr {
	uint8_t  seq_num;
	uint8_t  bss_idx;
	uint8_t  pad[2];
} __attribute__((packed));

/* struct scan_req_tlv */
struct mt7925_scan_req {
	uint16_t tag;
	uint16_t len;
	uint8_t  scan_type;
	uint8_t  probe_req_num;
	uint8_t  scan_func;
	uint8_t  src_mask;
	uint16_t channel_min_dwell_time;
	uint16_t channel_dwell_time;
	uint16_t timeout_value;
	uint16_t probe_delay_time;
	uint32_t func_mask_ext;
} __attribute__((packed));

/* struct mt76_connac_mcu_scan_ssid */
struct mt7925_scan_ssid_entry {
	uint32_t ssid_len;
	uint8_t  ssid[32];
} __attribute__((packed));

#define MT_SCAN_MAX_SSIDS		10

/* struct scan_ssid_tlv */
struct mt7925_scan_ssid {
	uint16_t tag;
	uint16_t len;
	uint8_t  ssid_type;
	uint8_t  ssids_num;
	uint8_t  is_short_ssid;
	uint8_t  pad;
	struct mt7925_scan_ssid_entry ssids[MT_SCAN_MAX_SSIDS];
} __attribute__((packed));

/* struct scan_bssid_tlv */
struct mt7925_scan_bssid {
	uint16_t tag;
	uint16_t len;
	uint8_t  bssid[6];
	uint8_t  match_ch;
	uint8_t  match_ssid_ind;
	uint8_t  rcpi;
	uint8_t  match_short_ssid_ind;
	uint8_t  pad[2];
} __attribute__((packed));

#define MT_SCAN_MAX_CHANNELS		64

/* struct scan_chan_info_tlv */
struct mt7925_scan_chan {
	uint16_t tag;
	uint16_t len;
	uint8_t  channel_type;
	uint8_t  channels_num;
	uint8_t  pad[2];
	struct {
		uint8_t band;
		uint8_t channel_num;
	} __attribute__((packed)) channels[MT_SCAN_MAX_CHANNELS];
} __attribute__((packed));

/* struct scan_ie_tlv (fixed part) */
struct mt7925_scan_ie {
	uint16_t tag;
	uint16_t len;
	uint16_t ies_len;
	uint8_t  band;
	uint8_t  pad;
} __attribute__((packed));

/* struct scan_misc_tlv */
struct mt7925_scan_misc {
	uint16_t tag;
	uint16_t len;
	uint8_t  random_mac[6];
	uint8_t  rsv[2];
} __attribute__((packed));

/* struct edca */
struct mt7925_edca {
	uint16_t tag;
	uint16_t len;
	uint8_t  queue;
	uint8_t  set;
	uint8_t  cw_min;
	uint8_t  cw_max;
	uint16_t txop;
	uint8_t  aifs;
	uint8_t  rsv;
} __attribute__((packed));

/* struct mt76_connac_config inside the chip-config request */
struct mt7925_chip_config_req {
	uint8_t  rsv[4];
	uint16_t tag;
	uint16_t len;
	uint16_t id;
	uint8_t  type;
	uint8_t  resp_type;
	uint16_t data_size;
	uint16_t resv;
	uint8_t  data[320];
} __attribute__((packed));

/* Band-config request with an 8-byte body (RTS threshold). */
struct mt7925_rts_req {
	uint8_t  band_idx;
	uint8_t  rsv[3];
	uint16_t tag;
	uint16_t len;
	uint32_t len_thresh;
	uint32_t pkt_thresh;
} __attribute__((packed));

/* mt7925_mcu_set_rxfilter request */
struct mt7925_rxfilter_req {
	uint8_t  band_idx;
	uint8_t  rsv1[3];
	uint16_t tag;
	uint16_t len;
	uint8_t  mode;
	uint8_t  rsv2[3];
	uint32_t fif;
	uint32_t bit_map;
	uint8_t  bit_op;
	uint8_t  pad[51];
} __attribute__((packed));

/* mt7925_mcu_fw_log_2_host request */
struct mt7925_fw_log_req {
	uint8_t  rsv[4];
	uint16_t tag;
	uint16_t len;
	uint8_t  ctrl;
	uint8_t  interval;
	uint8_t  rsv2[2];
} __attribute__((packed));

/* mt7925_mcu_set_eeprom request */
struct mt7925_eeprom_mode_req {
	uint8_t  rsv[4];
	uint16_t tag;
	uint16_t len;
	uint8_t  buffer_mode;
	uint8_t  format;
	uint16_t buf_len;
} __attribute__((packed));

/* mt7925_mcu_read_eeprom request and the offset of data[] in its reply */
struct mt7925_efuse_read_req {
	uint8_t  rsv[4];
	uint16_t tag;
	uint16_t len;
	uint32_t addr;
	uint32_t valid;
	uint8_t  data[MT_EEPROM_BLOCK_SIZE];
} __attribute__((packed));
#define MT_EFUSE_READ_EVT_DATA_OFFSET	48

/* NIC capability query and reply header */
struct mt7925_nic_cap_req {
	uint8_t  rsv[4];
	uint16_t tag;
	uint16_t len;
} __attribute__((packed));

struct mt7925_nic_cap_hdr {
	uint16_t n_element;
	uint8_t  rsv[2];
} __attribute__((packed));

/* SET_DOMAIN_INFO header */
struct mt7925_domain_req {
	uint8_t  alpha2[4];
	uint8_t  bw_2g;
	uint8_t  bw_5g;
	uint8_t  bw_6g;
	uint8_t  pad;
	uint16_t tag;
	uint16_t len;
	uint8_t  n_2ch;
	uint8_t  n_5ch;
	uint8_t  n_6ch;
	uint8_t  pad2;
} __attribute__((packed));

/* struct mt76_connac_mcu_chan */
struct mt7925_domain_chan {
	uint16_t hw_value;
	uint16_t pad;
	uint32_t flags;
} __attribute__((packed));

/* struct mt7925_tx_power_limit_tlv */
struct mt7925_power_limit_req {
	uint8_t  rsv[4];
	uint16_t tag;
	uint16_t len;
	uint8_t  ver;
	uint8_t  pad0;
	uint16_t rsv1;
	uint8_t  n_chan;
	uint8_t  band;
	uint8_t  last_msg;
	uint8_t  limit_type;
	uint8_t  alpha2[4];
	uint8_t  pad2[32];
} __attribute__((packed));

/* struct mt7925_sku_tlv */
struct mt7925_sku {
	uint8_t  channel;
	int8_t   pwr_limit[MT_SKU_POWER_LIMIT];
} __attribute__((packed));

/* __mt7925_mcu_set_clc request */
struct mt7925_clc_req {
	uint8_t  rsv[4];
	uint16_t tag;
	uint16_t len;
	uint8_t  ver;
	uint8_t  pad0;
	uint16_t size;
	uint8_t  idx;
	uint8_t  env;
	uint8_t  acpi_conf;
	uint8_t  pad1;
	uint8_t  alpha2[2];
	uint8_t  type[2];
	uint8_t  rsvd[64];
} __attribute__((packed));

/* CLC blob layout inside the WM image (mt7925/mt7925.h) */
struct mt7925_clc_hdr {
	uint32_t len;
	uint8_t  idx;
	uint8_t  ver;
	uint8_t  t0_nr_country;		/* t0.nr_country, or t2.type */
	uint8_t  t0_type;
	uint8_t  t0_nr_seg;
	uint8_t  t0_rsv[7];
} __attribute__((packed));

struct mt7925_clc_rule {
	uint8_t  alpha2[2];
	uint8_t  type[2];
	uint8_t  seg_idx;
	uint8_t  flag;
	uint8_t  rsv[2];
} __attribute__((packed));

struct mt7925_clc_segment {
	uint8_t  idx;
	uint8_t  rsv1[3];
	uint32_t offset;
	uint32_t len;
	uint8_t  rsv2[4];
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* Message builder                                                      */
/* ------------------------------------------------------------------ */

/*
 * A command body under construction. Every mt76_connac_mcu_add_tlv() call
 * increments the little-endian count at bytes 2..3 of the body, whatever the
 * fixed header calls those bytes; mt7925_msg_tlv() does the same.
 */
struct mt7925_msg {
	uint8_t  *buf;
	uint32_t  len;
	uint32_t  cap;
	bool      overflow;
};

/* Start an empty message over caller storage. */
void mt7925_msg_init(struct mt7925_msg *m, uint8_t *buf, uint32_t cap);

/* Append len zeroed bytes; returns them, or NULL on overflow. */
void *mt7925_msg_put(struct mt7925_msg *m, uint32_t len);

/* Append a zeroed TLV with tag/len set and bump the body's TLV count. */
void *mt7925_msg_tlv(struct mt7925_msg *m, uint16_t tag, uint16_t len);

/* ------------------------------------------------------------------ */
/* Command parameters and builders                                      */
/* ------------------------------------------------------------------ */

/* Fill the unified descriptor in front of a body of body_len bytes. */
void mt7925_uni_fill_txd(struct mt7925_uni_txd *txd, uint16_t cid,
			 uint8_t option, uint8_t seq, uint32_t body_len);

struct mt7925_bss_params {
	uint8_t  bss_idx;
	uint8_t  omac_idx;
	uint8_t  band_idx;
	uint8_t  wmm_idx;
	uint8_t  link_idx;
	uint8_t  own_addr[6];
	uint8_t  bssid[6];
	uint16_t bcn_interval;
	uint8_t  dtim_period;
	uint8_t  band;			/* MT_BAND_* */
	uint8_t  channel;
	uint8_t  center_chan;
	uint8_t  bw;			/* MT_CMD_CBW_* */
	uint8_t  nss;
	uint8_t  phymode;
	uint8_t  phymode_ext;
	uint16_t bmc_wlan_idx;
	uint16_t sta_wlan_idx;
	uint8_t  cipher;		/* MT_CIPHER_* */
	uint8_t  rate_idx;		/* fixed-rate table index for bc/mc */
	uint16_t slot_time;
	bool     qos;
	bool     enable;
};

/* mt7925_mcu_add_bss_info(): basic, sec, rate, qos, mld, ifs [, rlm, mbssid]. */
void mt7925_build_bss_info(struct mt7925_msg *m,
			   const struct mt7925_bss_params *p);

/* The BSS_INFO_UPDATE half of mt76_connac_mcu_uni_add_dev(). */
void mt7925_build_bss_add(struct mt7925_msg *m,
			  const struct mt7925_bss_params *p);

/* The DEV_INFO_UPDATE half of mt76_connac_mcu_uni_add_dev(). */
void mt7925_build_dev_info(struct mt7925_msg *m, uint8_t omac_idx,
			   uint8_t band_idx, uint8_t link_idx,
			   const uint8_t addr[6], bool active);

/* mt7925_mcu_uni_bss_bcnft() */
void mt7925_build_bss_bcnft(struct mt7925_msg *m, uint8_t bss_idx,
			    uint16_t bcn_interval, uint8_t dtim_period);

/* The PM_DISABLE request of mt7925_mcu_set_bss_pm() */
void mt7925_build_bss_pm_disable(struct mt7925_msg *m, uint8_t bss_idx);

/* mt7925_mcu_set_timing() */
void mt7925_build_bss_timing(struct mt7925_msg *m, uint8_t bss_idx,
			     uint16_t slot_time);

struct mt7925_sta_params {
	uint8_t  bss_idx;
	uint16_t wlan_idx;
	uint8_t  muar_idx;
	bool     enable;
	bool     newly;
	bool     has_peer;		/* link_sta != NULL */
	uint8_t  state;			/* MT_STA_INFO_STATE_* */
	uint8_t  peer_addr[6];
	uint16_t aid;
	bool     qos;
	uint8_t  phy_type;		/* MT_PHY_TYPE_BIT_* */
	uint16_t basic_rates;		/* bitmap over the band's rate table */
	uint16_t ra_legacy;		/* RA_LEGACY_* layout */
	bool     to_ds;
	bool     dis_rx_hdr_tran;
};

/* mt7925_mcu_sta_cmd() for a legacy (non-HT) peer. */
void mt7925_build_sta_rec(struct mt7925_msg *m,
			  const struct mt7925_sta_params *p);

/* mt7925_mcu_wtbl_update_hdr_trans() */
void mt7925_build_sta_hdr_trans(struct mt7925_msg *m,
				const struct mt7925_sta_params *p);

struct mt7925_key_params {
	uint8_t  bss_idx;
	uint16_t wlan_idx;
	uint8_t  muar_idx;
	bool     add;
	bool     pairwise;
	uint8_t  peer_addr[6];
	uint8_t  cipher;		/* MT_CIPHER_* */
	uint8_t  key_id;
	uint8_t  key_len;
	const uint8_t *key;
};

/* mt7925_mcu_add_key() */
void mt7925_build_sta_key(struct mt7925_msg *m,
			  const struct mt7925_key_params *p);

/* mt7925_mcu_set_roc() / mt7925_mcu_abort_roc() */
void mt7925_build_roc(struct mt7925_msg *m, uint8_t bss_idx, uint8_t token,
		      uint8_t channel, uint8_t band, uint32_t duration_ms);
void mt7925_build_roc_abort(struct mt7925_msg *m, uint8_t bss_idx,
			    uint8_t token);

struct mt7925_scan_params {
	uint8_t  seq_num;
	uint8_t  bss_idx;
	const uint8_t *ssid;
	uint8_t  ssid_len;
	uint8_t  n_channels;
	const uint8_t *chan_band;	/* MT_BAND_* per channel */
	const uint8_t *chan_num;
	const uint8_t *ies_2g;
	uint16_t ies_2g_len;
	const uint8_t *ies_5g;
	uint16_t ies_5g_len;
};

/* mt7925_mcu_hw_scan() */
void mt7925_build_scan(struct mt7925_msg *m,
		       const struct mt7925_scan_params *p);

struct mt7925_edca_params {
	uint8_t  aifs;
	uint16_t cw_min;
	uint16_t cw_max;
	uint16_t txop;
};

/* mt7925_mcu_set_tx(); params indexed by mac80211 AC (VO, VI, BE, BK). */
void mt7925_build_edca(struct mt7925_msg *m, uint8_t bss_idx,
		       const struct mt7925_edca_params ac[4]);

/* mt7925_mcu_chip_config(); cmd is a NUL-terminated firmware string. */
void mt7925_build_chip_config(struct mt7925_msg *m, const char *cmd);

void mt7925_build_nic_cap(struct mt7925_msg *m);
void mt7925_build_fw_log(struct mt7925_msg *m, uint8_t ctrl);
void mt7925_build_eeprom_mode(struct mt7925_msg *m);
void mt7925_build_efuse_read(struct mt7925_msg *m, uint32_t offset);
void mt7925_build_rts(struct mt7925_msg *m, uint8_t band_idx, uint32_t thresh);

/* mt7925_mcu_set_rxfilter() */
void mt7925_build_rxfilter(struct mt7925_msg *m, uint8_t band_idx,
			   uint32_t fif, uint8_t bit_op, uint32_t bit_map);

struct mt7925_chan_entry {
	uint8_t  band;			/* MT_BAND_* */
	uint8_t  channel;
	uint32_t flags;			/* MT_CHAN_FLAG_* */
};

/* mt7925_mcu_set_channel_domain() */
void mt7925_build_domain(struct mt7925_msg *m, const char alpha2[2],
			 const struct mt7925_chan_entry *chans, uint32_t n);

/*
 * One batch of mt7925_mcu_rate_txpower_band(). power[i] is the per-rate
 * target for chans[i] in half-dBm; band is MT_BAND_*.
 */
void mt7925_build_power_limit(struct mt7925_msg *m, const char alpha2[2],
			      uint8_t band, const uint8_t *chans,
			      const int8_t *power, uint8_t n, bool last);

/* __mt7925_mcu_set_clc() for one matched rule. */
void mt7925_build_clc(struct mt7925_msg *m, uint8_t ver, uint8_t idx,
		      uint8_t env, const uint8_t alpha2[2],
		      const uint8_t type[2], const uint8_t *seg,
		      uint32_t seg_len);

/* mt7925_mcu_build_sku() with every limit at the same target power. */
void mt7925_fill_sku(int8_t *sku, uint8_t band, int8_t power);

/* Walk TLVs of an event body. Returns the next TLV or NULL at the end. */
const struct mt7925_tlv *mt7925_tlv_next(const uint8_t *body, uint32_t len,
					 uint32_t *pos);

#endif /* ANX_MT7925_UNI_H */
