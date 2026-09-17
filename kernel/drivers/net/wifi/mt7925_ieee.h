/*
 * mt7925_ieee.h — The 802.11 station frames the MT7925 driver builds and
 * parses itself.
 *
 * Linux leaves this to mac80211 and wpa_supplicant. Anunix has neither, so
 * the join frames, the element parser and the 802.11-to-Ethernet conversion
 * live here. Nothing in this file touches hardware.
 *
 * Frame contents follow Linux v6.19 net/mac80211/mlme.c for a non-HT,
 * non-QoS association; RSN handling follows IEEE 802.11-2020 9.4.2.24.
 */

#ifndef ANX_MT7925_IEEE_H
#define ANX_MT7925_IEEE_H

#include <anx/types.h>

#define IEEE80211_HDRLEN		24
#define IEEE80211_FC_MGMT_AUTH		0x00b0
#define IEEE80211_FC_MGMT_DEAUTH	0x00c0
#define IEEE80211_FC_MGMT_ASSOC_REQ	0x0000

/* Frame-control fields (host order of the little-endian word) */
#define IEEE80211_FCTL_FTYPE		0x000c
#define IEEE80211_FCTL_STYPE		0x00f0
#define IEEE80211_FCTL_TODS		0x0100
#define IEEE80211_FCTL_FROMDS		0x0200
#define IEEE80211_FCTL_PROTECTED	0x4000
#define IEEE80211_FCTL_ORDER		0x8000
#define IEEE80211_FTYPE_MGMT		0x0000
#define IEEE80211_FTYPE_DATA		0x0008
#define IEEE80211_STYPE_ASSOC_RESP	0x0010
#define IEEE80211_STYPE_REASSOC_RESP	0x0030
#define IEEE80211_STYPE_PROBE_RESP	0x0050
#define IEEE80211_STYPE_BEACON		0x0080
#define IEEE80211_STYPE_DISASSOC	0x00a0
#define IEEE80211_STYPE_AUTH		0x00b0
#define IEEE80211_STYPE_DEAUTH		0x00c0
#define IEEE80211_STYPE_QOS_DATA	0x0080
#define IEEE80211_STYPE_NULLFUNC	0x0040

#define WLAN_CAPABILITY_ESS		0x0001
#define WLAN_CAPABILITY_PRIVACY		0x0010
#define WLAN_CAPABILITY_SHORT_PREAMBLE	0x0020
#define WLAN_CAPABILITY_SPECTRUM_MGMT	0x0100
#define WLAN_CAPABILITY_SHORT_SLOT_TIME	0x0400

#define WLAN_EID_SSID			0
#define WLAN_EID_SUPP_RATES		1
#define WLAN_EID_DS_PARAMS		3
#define WLAN_EID_TIM			5
#define WLAN_EID_PWR_CAPABILITY		33
#define WLAN_EID_SUPPORTED_CHANNELS	36
#define WLAN_EID_RSN			48
#define WLAN_EID_EXT_SUPP_RATES		50
#define WLAN_EID_HT_OPERATION		61
#define WLAN_EID_VENDOR			221

/* RSN suites, 00-0f-ac:n */
#define RSN_CIPHER_TKIP			2
#define RSN_CIPHER_CCMP			4
#define RSN_AKM_PSK			2
#define RSN_AKM_SAE			8
#define RSN_CAP_MFPR			0x0040

/* Rates in the MT7925 band tables (mt76_rates[]) */
#define MT7925_RATES_2G			12
#define MT7925_RATES_5G			8

#define MT7925_BSS_MAX_RATES		16
#define MT7925_RSN_IE_MAX		64

struct mt7925_bss {
	uint8_t  bssid[6];
	uint8_t  ssid[32];
	uint8_t  ssid_len;
	uint8_t  band;			/* MT_BAND_* */
	uint8_t  channel;
	int8_t   rssi;			/* dBm; 0 when unknown */
	uint16_t capability;
	uint16_t beacon_int;
	uint8_t  dtim_period;
	uint8_t  rates[MT7925_BSS_MAX_RATES];	/* 500 kb/s units, bit 7 = basic */
	uint8_t  n_rates;

	/* RSN, when rsn_ie_len != 0 */
	uint8_t  rsn_ie[MT7925_RSN_IE_MAX];
	uint8_t  rsn_ie_len;
	uint8_t  group_cipher;		/* RSN_CIPHER_* */
	bool     pairwise_ccmp;
	bool     akm_psk;
	bool     akm_sae;
	uint16_t rsn_caps;
	bool     wmm;
};

/* Parse a beacon or probe response (802.11 header included). */
int mt7925_ieee_parse_beacon(const uint8_t *frame, uint32_t len,
			     struct mt7925_bss *out);

/* True when a BSS can be joined with a WPA2-PSK passphrase (or is open). */
bool mt7925_ieee_bss_usable(const struct mt7925_bss *bss, bool have_psk);

/* Build our RSN element for this BSS (CCMP pairwise, PSK). Returns length. */
uint32_t mt7925_ieee_build_rsn(const struct mt7925_bss *bss, uint8_t *out,
			       uint32_t cap);

/* Open System authentication, transaction 1. Returns frame length. */
uint32_t mt7925_ieee_build_auth(uint8_t *buf, uint32_t cap,
				const uint8_t bssid[6], const uint8_t sa[6]);

/* Association request for a legacy station. Returns frame length. */
uint32_t mt7925_ieee_build_assoc_req(uint8_t *buf, uint32_t cap,
				     const struct mt7925_bss *bss,
				     const uint8_t sa[6],
				     const uint8_t *rsn_ie,
				     uint32_t rsn_ie_len);

/* Deauthentication. Returns frame length. */
uint32_t mt7925_ieee_build_deauth(uint8_t *buf, uint32_t cap,
				  const uint8_t bssid[6], const uint8_t sa[6],
				  uint16_t reason);

/*
 * A non-QoS ToDS data frame carrying payload behind an RFC 1042 header,
 * as ieee80211_build_hdr() sends control-port (EAPOL) frames. Returns the
 * frame length.
 */
uint32_t mt7925_ieee_build_data(uint8_t *buf, uint32_t cap,
				const uint8_t bssid[6], const uint8_t sa[6],
				const uint8_t da[6], uint16_t ethertype,
				const uint8_t *payload, uint32_t len,
				bool protected);

/* Supported-rates elements for probe requests in one band. */
uint32_t mt7925_ieee_build_preq_ies(uint8_t band, uint8_t *buf,
				    uint32_t cap);

struct mt7925_mgmt_info {
	uint16_t stype;			/* IEEE80211_STYPE_* */
	uint8_t  sa[6];
	uint8_t  da[6];
	uint8_t  bssid[6];
	uint16_t auth_alg;
	uint16_t auth_seq;
	uint16_t status;		/* auth/assoc status or deauth reason */
	uint16_t aid;
	bool     assoc_wmm;
};

/* Parse the fixed fields of auth/assoc response/deauth/disassoc frames. */
int mt7925_ieee_parse_mgmt(const uint8_t *frame, uint32_t len,
			   struct mt7925_mgmt_info *out);

/*
 * Convert an 802.11 data frame (FromDS) to Ethernet II in place of out.
 * Returns the Ethernet length, or 0 when the frame is not convertible.
 */
uint32_t mt7925_ieee_data_to_eth(const uint8_t *frame, uint32_t len,
				 uint8_t *out, uint32_t cap);

/*
 * Rate bookkeeping. For a band's rate table (2 GHz: CCK+OFDM, 5 GHz: OFDM)
 * compute the bitmap of rates the BSS supports, the bitmap of its basic
 * rates, the RA legacy mask and the fixed-rate table index for bc/mc.
 */
struct mt7925_rate_info {
	uint16_t supp;			/* bitmap over the band table */
	uint16_t basic;
	uint16_t ra_legacy;		/* RA_LEGACY_* layout */
	uint8_t  basic_rate_idx;	/* MT792x_BASIC_RATES_TBL + i */
	uint8_t  phy_type;		/* MT_PHY_TYPE_BIT_* */
	uint8_t  phymode;		/* MT_PHY_MODE_* */
};

void mt7925_ieee_rates(const struct mt7925_bss *bss,
		       struct mt7925_rate_info *out);

/* Fixed-rate table value for mt76_rates[i] (MT_TX_RATE_MODE | IDX). */
uint16_t mt7925_ieee_rate_table_value(uint32_t i);

#endif /* ANX_MT7925_IEEE_H */
