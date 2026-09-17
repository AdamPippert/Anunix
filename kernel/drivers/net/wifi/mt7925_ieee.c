/*
 * mt7925_ieee.c — 802.11 station frames for the MT7925 driver.
 *
 * Frame bytes are written explicitly in little-endian order; nothing here
 * relies on the host being little-endian or on packed structures.
 */

#include <anx/types.h>
#include <anx/string.h>
#include "mt7925_ieee.h"
#include "mt7925_uni.h"

#define MT792X_BASIC_RATES_TBL	11
#define LISTEN_INTERVAL		10
#define MAX_TX_POWER_DBM	20

/*
 * mt76_rates[] (Linux v6.19 mt76/mac80211.c): rate in 500 kb/s units and
 * the hardware index. 2 GHz uses all twelve; 5 GHz starts at entry 4.
 */
static const struct {
	uint8_t units;
	uint16_t hw_value;
} rate_table[MT7925_RATES_2G] = {
	{   2, 0x000 }, {   4, 0x001 }, {  11, 0x002 }, {  22, 0x003 },
	{  12, 0x10b }, {  18, 0x10f }, {  24, 0x10a }, {  36, 0x10e },
	{  48, 0x109 }, {  72, 0x10d }, {  96, 0x108 }, { 108, 0x10c },
};

/* The US channel set the driver registers with the firmware. */
static const uint8_t chans_2g[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t chans_5g[] = {
	36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124,
	128, 132, 136, 140, 144, 149, 153, 157, 161, 165,
};

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static bool is_oui_ieee(const uint8_t *p)
{
	return p[0] == 0x00 && p[1] == 0x0f && p[2] == 0xac;
}

static uint32_t band_offset(uint8_t band)
{
	return band == MT_BAND_2G ? 0 : 4;
}

static uint32_t band_rates(uint8_t band)
{
	return band == MT_BAND_2G ? MT7925_RATES_2G : MT7925_RATES_5G;
}

/* ------------------------------------------------------------------ */
/* Element parsing                                                      */
/* ------------------------------------------------------------------ */

static void parse_rsn(struct mt7925_bss *bss, const uint8_t *ie, uint8_t len)
{
	const uint8_t *p = ie, *end = ie + len;
	uint16_t n, i;

	if (len < 2 || get_le16(p) != 1)
		return;
	p += 2;

	/* The element is usable only when every field we rely on is present. */
	if (p + 4 > end || !is_oui_ieee(p))
		return;
	bss->group_cipher = p[3];
	p += 4;

	if (p + 2 > end)
		return;
	n = get_le16(p);
	p += 2;
	for (i = 0; i < n; i++, p += 4) {
		if (p + 4 > end)
			return;
		if (is_oui_ieee(p) && p[3] == RSN_CIPHER_CCMP)
			bss->pairwise_ccmp = true;
	}

	if (p + 2 > end)
		return;
	n = get_le16(p);
	p += 2;
	for (i = 0; i < n; i++, p += 4) {
		if (p + 4 > end)
			return;
		if (!is_oui_ieee(p))
			continue;
		if (p[3] == RSN_AKM_PSK)
			bss->akm_psk = true;
		else if (p[3] == RSN_AKM_SAE)
			bss->akm_sae = true;
	}

	if (p + 2 <= end)
		bss->rsn_caps = get_le16(p);

	if (len + 2 <= sizeof(bss->rsn_ie)) {
		bss->rsn_ie[0] = WLAN_EID_RSN;
		bss->rsn_ie[1] = len;
		anx_memcpy(bss->rsn_ie + 2, ie, len);
		bss->rsn_ie_len = (uint8_t)(len + 2);
	}
}

static void add_rates(struct mt7925_bss *bss, const uint8_t *ie, uint8_t len)
{
	uint8_t i;

	for (i = 0; i < len && bss->n_rates < MT7925_BSS_MAX_RATES; i++)
		bss->rates[bss->n_rates++] = ie[i];
}

static bool is_wmm_ie(const uint8_t *ie, uint8_t len)
{
	return len >= 5 && ie[0] == 0x00 && ie[1] == 0x50 && ie[2] == 0xf2 &&
	       ie[3] == 0x02 && (ie[4] == 0x00 || ie[4] == 0x01);
}

static void parse_ies(struct mt7925_bss *bss, const uint8_t *ies,
		      uint32_t len)
{
	uint32_t pos = 0;

	while (pos + 2 <= len) {
		uint8_t id = ies[pos], elen = ies[pos + 1];
		const uint8_t *e = ies + pos + 2;

		if (pos + 2 + elen > len)
			break;

		switch (id) {
		case WLAN_EID_SSID:
			if (elen <= 32) {
				anx_memcpy(bss->ssid, e, elen);
				bss->ssid_len = elen;
			}
			break;
		case WLAN_EID_SUPP_RATES:
		case WLAN_EID_EXT_SUPP_RATES:
			add_rates(bss, e, elen);
			break;
		case WLAN_EID_DS_PARAMS:
			if (elen >= 1)
				bss->channel = e[0];
			break;
		case WLAN_EID_TIM:
			if (elen >= 2)
				bss->dtim_period = e[1];
			break;
		case WLAN_EID_HT_OPERATION:
			if (elen >= 1 && !bss->channel)
				bss->channel = e[0];
			break;
		case WLAN_EID_RSN:
			parse_rsn(bss, e, elen);
			break;
		case WLAN_EID_VENDOR:
			if (is_wmm_ie(e, elen))
				bss->wmm = true;
			break;
		default:
			break;
		}
		pos += 2 + (uint32_t)elen;
	}
}

int mt7925_ieee_parse_beacon(const uint8_t *frame, uint32_t len,
			     struct mt7925_bss *out)
{
	uint16_t fc, stype;

	if (len < IEEE80211_HDRLEN + 12)
		return -1;
	fc = get_le16(frame);
	stype = fc & IEEE80211_FCTL_STYPE;
	if ((fc & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_MGMT ||
	    (stype != IEEE80211_STYPE_BEACON &&
	     stype != IEEE80211_STYPE_PROBE_RESP))
		return -1;

	anx_memset(out, 0, sizeof(*out));
	anx_memcpy(out->bssid, frame + 16, 6);
	out->beacon_int = get_le16(frame + IEEE80211_HDRLEN + 8);
	out->capability = get_le16(frame + IEEE80211_HDRLEN + 10);
	out->dtim_period = 1;
	parse_ies(out, frame + IEEE80211_HDRLEN + 12,
		  len - IEEE80211_HDRLEN - 12);
	return 0;
}

bool mt7925_ieee_bss_usable(const struct mt7925_bss *bss, bool have_psk)
{
	if (!have_psk)
		return !(bss->capability & WLAN_CAPABILITY_PRIVACY) &&
		       !bss->rsn_ie_len;

	if (!bss->rsn_ie_len || !bss->pairwise_ccmp || !bss->akm_psk)
		return false;
	if (bss->rsn_caps & RSN_CAP_MFPR)
		return false;
	return bss->group_cipher == RSN_CIPHER_CCMP ||
	       bss->group_cipher == RSN_CIPHER_TKIP;
}

/* ------------------------------------------------------------------ */
/* Frame construction                                                   */
/* ------------------------------------------------------------------ */

uint32_t mt7925_ieee_build_rsn(const struct mt7925_bss *bss, uint8_t *out,
			       uint32_t cap)
{
	static const uint8_t tmpl[22] = {
		WLAN_EID_RSN, 20,
		0x01, 0x00,			/* version */
		0x00, 0x0f, 0xac, 0x00,		/* group cipher, set below */
		0x01, 0x00,
		0x00, 0x0f, 0xac, RSN_CIPHER_CCMP,
		0x01, 0x00,
		0x00, 0x0f, 0xac, RSN_AKM_PSK,
		0x00, 0x00,			/* capabilities */
	};

	if (cap < sizeof(tmpl))
		return 0;
	anx_memcpy(out, tmpl, sizeof(tmpl));
	out[7] = bss->group_cipher;
	return sizeof(tmpl);
}

static uint32_t put_hdr(uint8_t *buf, uint16_t fc, const uint8_t da[6],
			const uint8_t sa[6], const uint8_t bssid[6])
{
	anx_memset(buf, 0, IEEE80211_HDRLEN);
	put_le16(buf, fc);
	anx_memcpy(buf + 4, da, 6);
	anx_memcpy(buf + 10, sa, 6);
	anx_memcpy(buf + 16, bssid, 6);
	return IEEE80211_HDRLEN;
}

uint32_t mt7925_ieee_build_auth(uint8_t *buf, uint32_t cap,
				const uint8_t bssid[6], const uint8_t sa[6])
{
	uint32_t pos;

	if (cap < IEEE80211_HDRLEN + 6)
		return 0;
	pos = put_hdr(buf, IEEE80211_FC_MGMT_AUTH, bssid, sa, bssid);
	put_le16(buf + pos, 0);		/* Open System */
	put_le16(buf + pos + 2, 1);	/* transaction sequence */
	put_le16(buf + pos + 4, 0);	/* status */
	return pos + 6;
}

uint32_t mt7925_ieee_build_deauth(uint8_t *buf, uint32_t cap,
				  const uint8_t bssid[6], const uint8_t sa[6],
				  uint16_t reason)
{
	uint32_t pos;

	if (cap < IEEE80211_HDRLEN + 2)
		return 0;
	pos = put_hdr(buf, IEEE80211_FC_MGMT_DEAUTH, bssid, sa, bssid);
	put_le16(buf + pos, reason);
	return pos + 2;
}

uint32_t mt7925_ieee_build_data(uint8_t *buf, uint32_t cap,
				const uint8_t bssid[6], const uint8_t sa[6],
				const uint8_t da[6], uint16_t ethertype,
				const uint8_t *payload, uint32_t len,
				bool protected)
{
	static const uint8_t rfc1042[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
	uint16_t fc = IEEE80211_FTYPE_DATA | IEEE80211_FCTL_TODS;
	uint32_t pos;

	if (cap < IEEE80211_HDRLEN + 8 + len)
		return 0;
	if (protected)
		fc |= IEEE80211_FCTL_PROTECTED;
	pos = put_hdr(buf, fc, bssid, sa, da);
	anx_memcpy(buf + pos, rfc1042, 6);
	buf[pos + 6] = (uint8_t)(ethertype >> 8);
	buf[pos + 7] = (uint8_t)ethertype;
	pos += 8;
	anx_memcpy(buf + pos, payload, len);
	return pos + len;
}

/* ieee80211_put_srates_elem() for the rates in mask, table order. */
static uint32_t put_srates(uint8_t *buf, uint32_t cap, uint8_t band,
			   uint16_t mask, uint8_t eid)
{
	uint32_t off = band_offset(band), n = band_rates(band), i;
	uint8_t list[MT7925_RATES_2G];
	uint32_t cnt = 0, first, take;

	for (i = 0; i < n; i++)
		if (mask & (1U << i))
			list[cnt++] = rate_table[off + i].units;

	if (eid == WLAN_EID_SUPP_RATES) {
		first = 0;
		take = cnt > 8 ? 8 : cnt;
	} else {
		if (cnt <= 8)
			return 0;
		first = 8;
		take = cnt - 8;
	}
	if (!take || cap < 2 + take)
		return 0;
	buf[0] = eid;
	buf[1] = (uint8_t)take;
	anx_memcpy(buf + 2, list + first, take);
	return 2 + take;
}

uint32_t mt7925_ieee_build_preq_ies(uint8_t band, uint8_t *buf, uint32_t cap)
{
	uint16_t all = (uint16_t)((1U << band_rates(band)) - 1);
	uint32_t pos;

	pos = put_srates(buf, cap, band, all, WLAN_EID_SUPP_RATES);
	pos += put_srates(buf + pos, cap - pos, band, all,
			  WLAN_EID_EXT_SUPP_RATES);
	return pos;
}

uint32_t mt7925_ieee_build_assoc_req(uint8_t *buf, uint32_t cap,
				     const struct mt7925_bss *bss,
				     const uint8_t sa[6],
				     const uint8_t *rsn_ie,
				     uint32_t rsn_ie_len)
{
	struct mt7925_rate_info ri;
	uint16_t capab = 0, mask;
	uint32_t pos, i;

	if (cap < IEEE80211_HDRLEN + 4 + 2 + bss->ssid_len + 32 +
		  rsn_ie_len + 64)
		return 0;

	pos = put_hdr(buf, IEEE80211_FC_MGMT_ASSOC_REQ, bss->bssid, sa,
		      bss->bssid);

	if (bss->capability & WLAN_CAPABILITY_PRIVACY)
		capab |= WLAN_CAPABILITY_PRIVACY;
	capab |= WLAN_CAPABILITY_ESS;
	if (bss->band == MT_BAND_2G)
		capab |= WLAN_CAPABILITY_SHORT_SLOT_TIME |
			 WLAN_CAPABILITY_SHORT_PREAMBLE;
	if (bss->capability & WLAN_CAPABILITY_SPECTRUM_MGMT)
		capab |= WLAN_CAPABILITY_SPECTRUM_MGMT;

	put_le16(buf + pos, capab);
	put_le16(buf + pos + 2, LISTEN_INTERVAL);
	pos += 4;

	buf[pos++] = WLAN_EID_SSID;
	buf[pos++] = bss->ssid_len;
	anx_memcpy(buf + pos, bss->ssid, bss->ssid_len);
	pos += bss->ssid_len;

	/* Only rates the AP also supports (ieee80211_assoc_add_rates()). */
	mt7925_ieee_rates(bss, &ri);
	mask = ri.supp ? ri.supp : (uint16_t)((1U << band_rates(bss->band)) - 1);
	pos += put_srates(buf + pos, cap - pos, bss->band, mask,
			  WLAN_EID_SUPP_RATES);
	pos += put_srates(buf + pos, cap - pos, bss->band, mask,
			  WLAN_EID_EXT_SUPP_RATES);

	if (capab & WLAN_CAPABILITY_SPECTRUM_MGMT) {
		const uint8_t *chans;
		uint32_t n;

		buf[pos++] = WLAN_EID_PWR_CAPABILITY;
		buf[pos++] = 2;
		buf[pos++] = 0;
		buf[pos++] = MAX_TX_POWER_DBM;

		if (bss->band == MT_BAND_2G) {
			chans = chans_2g;
			n = sizeof(chans_2g);
		} else {
			chans = chans_5g;
			n = sizeof(chans_5g);
		}
		if (pos + 2 + 2 * n > cap)
			return 0;
		buf[pos++] = WLAN_EID_SUPPORTED_CHANNELS;
		buf[pos++] = (uint8_t)(2 * n);
		for (i = 0; i < n; i++) {
			buf[pos++] = chans[i];
			buf[pos++] = 1;
		}
	}

	if (rsn_ie && rsn_ie_len) {
		if (pos + rsn_ie_len > cap)
			return 0;
		anx_memcpy(buf + pos, rsn_ie, rsn_ie_len);
		pos += rsn_ie_len;
	}
	return pos;
}

/* ------------------------------------------------------------------ */
/* Received management and data frames                                  */
/* ------------------------------------------------------------------ */

static bool ies_have_wmm(const uint8_t *ies, uint32_t len)
{
	uint32_t pos = 0;

	while (pos + 2 <= len) {
		uint8_t id = ies[pos], elen = ies[pos + 1];

		if (pos + 2 + elen > len)
			break;
		if (id == WLAN_EID_VENDOR && is_wmm_ie(ies + pos + 2, elen))
			return true;
		pos += 2 + (uint32_t)elen;
	}
	return false;
}

int mt7925_ieee_parse_mgmt(const uint8_t *frame, uint32_t len,
			   struct mt7925_mgmt_info *out)
{
	const uint8_t *body = frame + IEEE80211_HDRLEN;
	uint16_t fc;
	uint32_t blen;

	if (len < IEEE80211_HDRLEN + 2)
		return -1;
	fc = get_le16(frame);
	if ((fc & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_MGMT)
		return -1;

	anx_memset(out, 0, sizeof(*out));
	out->stype = fc & IEEE80211_FCTL_STYPE;
	anx_memcpy(out->da, frame + 4, 6);
	anx_memcpy(out->sa, frame + 10, 6);
	anx_memcpy(out->bssid, frame + 16, 6);
	blen = len - IEEE80211_HDRLEN;

	switch (out->stype) {
	case IEEE80211_STYPE_AUTH:
		if (blen < 6)
			return -1;
		out->auth_alg = get_le16(body);
		out->auth_seq = get_le16(body + 2);
		out->status = get_le16(body + 4);
		return 0;
	case IEEE80211_STYPE_ASSOC_RESP:
	case IEEE80211_STYPE_REASSOC_RESP:
		if (blen < 6)
			return -1;
		out->status = get_le16(body + 2);
		out->aid = get_le16(body + 4) & 0x3fff;
		out->assoc_wmm = ies_have_wmm(body + 6, blen - 6);
		return 0;
	case IEEE80211_STYPE_DEAUTH:
	case IEEE80211_STYPE_DISASSOC:
		out->status = get_le16(body);
		return 0;
	default:
		return 0;
	}
}

uint32_t mt7925_ieee_data_to_eth(const uint8_t *frame, uint32_t len,
				 uint8_t *out, uint32_t cap)
{
	static const uint8_t rfc1042[6] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00 };
	static const uint8_t bridge[6]  = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0xf8 };
	const uint8_t *da, *sa, *llc;
	uint32_t hdrlen = IEEE80211_HDRLEN, plen;
	uint16_t fc;

	if (len < IEEE80211_HDRLEN)
		return 0;
	fc = get_le16(frame);
	if ((fc & IEEE80211_FCTL_FTYPE) != IEEE80211_FTYPE_DATA)
		return 0;
	if (fc & IEEE80211_STYPE_NULLFUNC)
		return 0;
	if ((fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) ==
	    (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS))
		return 0;
	if (fc & IEEE80211_STYPE_QOS_DATA) {
		hdrlen += 2;
		if (fc & IEEE80211_FCTL_ORDER)
			hdrlen += 4;
	}
	if (len < hdrlen + 8)
		return 0;

	da = frame + 4;
	sa = (fc & IEEE80211_FCTL_FROMDS) ? frame + 16 : frame + 10;
	llc = frame + hdrlen;
	if (anx_memcmp(llc, rfc1042, 6) != 0 && anx_memcmp(llc, bridge, 6) != 0)
		return 0;

	plen = len - hdrlen - 6;	/* ethertype + payload */
	if (12 + plen > cap)
		return 0;
	anx_memmove(out + 12, llc + 6, plen);
	anx_memcpy(out, da, 6);
	anx_memcpy(out + 6, sa, 6);
	return 12 + plen;
}

/* ------------------------------------------------------------------ */
/* Rates                                                                */
/* ------------------------------------------------------------------ */

void mt7925_ieee_rates(const struct mt7925_bss *bss,
		       struct mt7925_rate_info *out)
{
	uint32_t off = band_offset(bss->band), n = band_rates(bss->band);
	uint32_t i, j, lowest = 0;

	anx_memset(out, 0, sizeof(*out));
	for (i = 0; i < bss->n_rates; i++) {
		uint8_t units = bss->rates[i] & 0x7f;

		for (j = 0; j < n; j++) {
			if (rate_table[off + j].units != units)
				continue;
			out->supp |= (uint16_t)(1U << j);
			if (bss->rates[i] & 0x80)
				out->basic |= (uint16_t)(1U << j);
		}
	}

	if (bss->band == MT_BAND_2G) {
		out->ra_legacy = (uint16_t)(((out->supp >> 4) <<
					     MT_RA_LEGACY_OFDM_SHIFT) |
					    (out->supp & MT_RA_LEGACY_CCK_MASK));
		out->phy_type = MT_PHY_TYPE_BIT_HR_DSSS | MT_PHY_TYPE_BIT_ERP;
		out->phymode = MT_PHY_MODE_B | MT_PHY_MODE_G;
	} else {
		out->ra_legacy = (uint16_t)(out->supp << MT_RA_LEGACY_OFDM_SHIFT);
		out->phy_type = MT_PHY_TYPE_BIT_OFDM;
		out->phymode = MT_PHY_MODE_A;
	}

	/* mt76_connac2_mac_tx_rate_val(): lowest basic rate, else the first */
	if (out->basic)
		while (!(out->basic & (1U << lowest)))
			lowest++;
	lowest += off;
	if (lowest >= MT7925_RATES_2G)
		lowest = off;
	out->basic_rate_idx = (uint8_t)(MT792X_BASIC_RATES_TBL + lowest);
}

uint16_t mt7925_ieee_rate_table_value(uint32_t i)
{
	uint16_t hw = rate_table[i].hw_value;

	/* MT_TX_RATE_MODE GENMASK(9, 6), MT_TX_RATE_IDX GENMASK(5, 0) */
	return (uint16_t)(((hw >> 8) << 6) | (hw & 0x3f));
}
