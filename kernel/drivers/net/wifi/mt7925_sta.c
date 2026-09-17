/*
 * mt7925_sta.c — MT7925 station: scan, join, WPA2 handshake and keys.
 *
 * This file stands in for mac80211's MLME and for wpa_supplicant, calling
 * the driver in the order Linux v6.19 does for a non-MLD, non-HT station:
 *
 *   sta_add      mt7925_mac_link_sta_add():  BSS (disabled), STA_REC NONE
 *   auth         mt7925_mgd_prepare_tx() channel grant, frame, grant abort
 *   assoc        same, then mt7925_mac_link_sta_assoc(): BSS (enabled),
 *                STA_REC ASSOC; mt7925_vif_cfg_changed(): own STA_REC,
 *                beacon filter; mt7925_link_info_changed(): slot, EDCA
 *   keys         mt7925_set_link_key(): BSS with cipher, STA_REC key
 *   authorized   mt7925_sta_set_decap_offload(): header translation on
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/delay.h>
#include "mt7925_drv.h"

#define ROC_DURATION_MS		1000	/* mgd_prepare_tx default: HZ */
#define MGMT_TRIES		3	/* IEEE80211_AUTH_MAX_TRIES */
#define MGMT_WAIT_MS		400
#define SCAN_WAIT_MS		12000
#define HANDSHAKE_MS		12000
#define JOIN_CANDIDATES		3

#define REASON_DEAUTH_LEAVING	3

static void notify_disconnect(struct mt7925_dev *dev);

/* ------------------------------------------------------------------ */
/* Scanning                                                             */
/* ------------------------------------------------------------------ */

void mt7925_sta_scan_result(struct mt7925_dev *dev, const uint8_t *frame,
			    uint32_t len, const struct mt7925_bss *seen)
{
	uint32_t i;

	(void)frame;
	(void)len;
	for (i = 0; i < dev->n_scan; i++) {
		if (anx_memcmp(dev->scan[i].bssid, seen->bssid, 6) != 0)
			continue;
		/* Probe responses carry the SSID a hidden beacon omits. */
		if (seen->ssid_len || !dev->scan[i].ssid_len)
			dev->scan[i] = *seen;
		else
			dev->scan[i].rssi = seen->rssi;
		return;
	}
	if (dev->n_scan < MT7925_SCAN_MAX)
		dev->scan[dev->n_scan++] = *seen;
}

static int do_scan(struct mt7925_dev *dev, const char *ssid)
{
	uint8_t len = ssid ? (uint8_t)anx_strlen(ssid) : 0;
	anx_mt7925_state_t prev = dev->state;
	int ret;

	dev->n_scan = 0;
	dev->scan_done = false;
	dev->scan_active = true;
	dev->state = MT7925_STATE_SCANNING;

	ret = mt7925_mcu_hw_scan(dev, (const uint8_t *)ssid, len);
	if (ret == ANX_OK &&
	    !mt7925_wait_flag(dev, &dev->scan_done, SCAN_WAIT_MS)) {
		kprintf("mt7925: scan did not finish\n");
		ret = ANX_ETIMEDOUT;
	}

	dev->scan_active = false;
	dev->state = prev;
	return ret;
}

static const char *security_name(const struct mt7925_bss *b)
{
	if (b->rsn_ie_len) {
		if (b->akm_psk && b->akm_sae)
			return "WPA2/WPA3";
		if (b->akm_psk)
			return "WPA2";
		if (b->akm_sae)
			return "WPA3";
		return "RSN";
	}
	return (b->capability & WLAN_CAPABILITY_PRIVACY) ? "WEP" : "open";
}

static void print_bss(const struct mt7925_bss *b)
{
	char ssid[33];

	anx_memcpy(ssid, b->ssid, b->ssid_len);
	ssid[b->ssid_len] = '\0';
	kprintf("  %02x:%02x:%02x:%02x:%02x:%02x  ch %3u  %d dBm  %-9s  %s\n",
		b->bssid[0], b->bssid[1], b->bssid[2],
		b->bssid[3], b->bssid[4], b->bssid[5],
		b->channel, b->rssi, security_name(b),
		b->ssid_len ? ssid : "(hidden)");
}

int mt7925_sta_scan_print(struct mt7925_dev *dev)
{
	uint32_t i;
	int ret;

	if (dev->state >= MT7925_STATE_ASSOC) {
		kprintf("wifi: disconnect before scanning\n");
		return ANX_EBUSY;
	}
	ret = do_scan(dev, NULL);
	if (ret)
		return ret;
	kprintf("wifi: %u networks\n", dev->n_scan);
	for (i = 0; i < dev->n_scan; i++)
		print_bss(&dev->scan[i]);
	return ANX_OK;
}

static bool ssid_matches(const struct mt7925_bss *b, const char *ssid)
{
	size_t len = anx_strlen(ssid);

	return b->ssid_len == len && anx_memcmp(b->ssid, ssid, len) == 0;
}

/* The strongest usable BSSs for ssid, best first; returns the count. */
static uint32_t pick_candidates(struct mt7925_dev *dev, const char *ssid,
				bool have_psk, uint32_t *idx, uint32_t max)
{
	uint32_t all[MT7925_SCAN_MAX];
	uint32_t i, j, n = 0;

	for (i = 0; i < dev->n_scan; i++) {
		const struct mt7925_bss *b = &dev->scan[i];

		if (!ssid_matches(b, ssid))
			continue;
		if (!mt7925_ieee_bss_usable(b, have_psk)) {
			kprintf("mt7925: skipping ");
			print_bss(b);
			continue;
		}
		if (b->band == MT_BAND_2G ? !dev->has_2g : !dev->has_5g)
			continue;
		all[n++] = i;
	}

	/* Selection sort by RSSI; the list holds at most 32 entries. */
	for (i = 0; i < n && i < max; i++) {
		uint32_t best = i, tmp;

		for (j = i + 1; j < n; j++)
			if (dev->scan[all[j]].rssi > dev->scan[all[best]].rssi)
				best = j;
		tmp = all[i];
		all[i] = all[best];
		all[best] = tmp;
		idx[i] = all[i];
	}
	return n < max ? n : max;
}

/* ------------------------------------------------------------------ */
/* Firmware records                                                     */
/* ------------------------------------------------------------------ */

static void bss_params(const struct mt7925_dev *dev, bool enable,
		       struct mt7925_bss_params *p)
{
	anx_memset(p, 0, sizeof(*p));
	p->bss_idx = MT7925_BSS_IDX;
	p->omac_idx = MT7925_OMAC_IDX;
	p->band_idx = dev->band_idx;
	anx_memcpy(p->own_addr, dev->mac, 6);
	anx_memcpy(p->bssid, dev->bss.bssid, 6);
	p->bcn_interval = dev->bss.beacon_int;
	p->dtim_period = dev->bss.dtim_period;
	p->band = dev->bss.band;
	p->channel = dev->bss.channel;
	p->center_chan = dev->bss.channel;
	p->bw = MT_CMD_CBW_20MHZ;
	p->nss = dev->nss ? dev->nss : 1;
	p->phymode = dev->rates.phymode;
	p->bmc_wlan_idx = MT7925_WCID_BSS;
	p->sta_wlan_idx = MT7925_WCID_AP;
	p->cipher = dev->cipher;
	p->rate_idx = dev->rates.basic_rate_idx;
	/* phy->slottime: long until the association changes ERP_SLOT */
	p->slot_time = 20;
	p->qos = false;
	p->enable = enable;
}

static void ap_sta_params(const struct mt7925_dev *dev, uint8_t state,
			  bool newly, struct mt7925_sta_params *p)
{
	anx_memset(p, 0, sizeof(*p));
	p->bss_idx = MT7925_BSS_IDX;
	p->wlan_idx = MT7925_WCID_AP;
	p->muar_idx = MT7925_OMAC_IDX;
	p->enable = true;
	p->newly = newly;
	p->has_peer = true;
	p->state = state;
	anx_memcpy(p->peer_addr, dev->bss.bssid, 6);
	p->aid = dev->aid;
	p->qos = false;
	p->phy_type = dev->rates.phy_type;
	p->basic_rates = dev->rates.basic;
	p->ra_legacy = dev->rates.ra_legacy;
	p->to_ds = true;
	/* Until the port is authorized mac80211 keeps decap offload off. */
	p->dis_rx_hdr_tran = !(dev->state >= MT7925_STATE_CONNECTED);
}

/* The interface's own entry, updated with no peer (link_sta == NULL). */
static void bss_sta_params(struct mt7925_sta_params *p)
{
	anx_memset(p, 0, sizeof(*p));
	p->bss_idx = MT7925_BSS_IDX;
	p->wlan_idx = MT7925_WCID_BSS;
	p->muar_idx = MT7925_MUAR_BSS;
	p->enable = true;
	p->state = MT_STA_INFO_STATE_ASSOC;
	p->to_ds = true;
	p->dis_rx_hdr_tran = true;
}

/*
 * mt7925_mac_link_sta_remove(), preceded, when the station had
 * associated, by the vif side of the disassociation.
 */
static void remove_station(struct mt7925_dev *dev)
{
	struct mt7925_sta_params sp;
	struct mt7925_bss_params bp;

	if (dev->state >= MT7925_STATE_ASSOC) {
		bss_sta_params(&sp);
		mt7925_mcu_sta_update(&sp);
		mt7925_mcu_set_beacon_filter(dev, false);
	}

	ap_sta_params(dev, MT_STA_INFO_STATE_NONE, false, &sp);
	sp.enable = false;
	mt7925_mcu_sta_update(&sp);
	mt7925_mac_wtbl_clear(MT7925_WCID_AP);

	dev->cipher = MT_CIPHER_NONE;
	bss_params(dev, false, &bp);
	mt7925_mcu_add_bss_info(&bp);
}

/* ------------------------------------------------------------------ */
/* Management exchanges                                                 */
/* ------------------------------------------------------------------ */

/*
 * Wait for a management frame of subtype want from the AP. A deauth or
 * disassoc from it ends the wait. Queued EAPOL frames are left in place.
 */
static int wait_mgmt(struct mt7925_dev *dev, uint16_t want, uint32_t ms,
		     struct mt7925_mgmt_info *info)
{
	struct mt7925_frame f;
	struct mt7925_timer t;

	mt7925_timer_start(&t, ms);
	do {
		mt7925_service(dev);
		while (dev->q_head != dev->q_tail &&
		       !dev->queue[dev->q_tail].is_eapol) {
			mt7925_queue_pop(dev, &f);
			if (mt7925_ieee_parse_mgmt(f.data, f.len, info) != 0 ||
			    anx_memcmp(info->sa, dev->bss.bssid, 6) != 0)
				continue;
			if (info->stype == want)
				return ANX_OK;
			if (info->stype == IEEE80211_STYPE_DEAUTH ||
			    info->stype == IEEE80211_STYPE_DISASSOC) {
				kprintf("mt7925: AP sent %s, reason %u\n",
					info->stype == IEEE80211_STYPE_DEAUTH ?
					"deauth" : "disassoc", info->status);
				dev->disconnect_reason = info->status;
				return ANX_ECONNRESET;
			}
		}
	} while (!mt7925_timer_expired(&t));
	return ANX_ETIMEDOUT;
}

static int exchange(struct mt7925_dev *dev, const uint8_t *frame,
		    uint32_t len, uint16_t want, const char *what,
		    struct mt7925_mgmt_info *info)
{
	uint32_t try;
	int ret = ANX_ETIMEDOUT;

	for (try = 1; try <= MGMT_TRIES; try++) {
		ret = mt7925_mcu_set_roc(dev, dev->bss.channel, dev->bss.band,
					 ROC_DURATION_MS);
		if (ret)
			return ret;
		ret = mt7925_tx_mgmt(dev, frame, len, MT7925_WCID_AP);
		if (ret == ANX_OK)
			ret = wait_mgmt(dev, want, MGMT_WAIT_MS, info);
		mt7925_mcu_abort_roc(dev);
		if (ret != ANX_ETIMEDOUT)
			break;
		kprintf("mt7925: %s try %u timed out\n", what, try);
	}
	return ret;
}

/* ------------------------------------------------------------------ */
/* Keys                                                                 */
/* ------------------------------------------------------------------ */

static int install_ptk(struct mt7925_dev *dev)
{
	struct mt7925_key_params k;
	int ret;

	/* mt7925_set_link_key(): the first key sets the BSS cipher. */
	if (dev->cipher == MT_CIPHER_NONE) {
		struct mt7925_bss_params bp;

		dev->cipher = MT_CIPHER_AES_CCMP;
		bss_params(dev, true, &bp);
		ret = mt7925_mcu_add_bss_info(&bp);
		if (ret)
			return ret;
	}

	anx_memset(&k, 0, sizeof(k));
	k.bss_idx = MT7925_BSS_IDX;
	k.wlan_idx = MT7925_WCID_AP;
	k.muar_idx = MT7925_OMAC_IDX;
	k.add = true;
	k.pairwise = true;
	anx_memcpy(k.peer_addr, dev->bss.bssid, 6);
	k.cipher = MT_CIPHER_AES_CCMP;
	k.key_id = 0;
	k.key_len = 16;
	k.key = mt7925_eapol_tk(&dev->eapol);
	return mt7925_mcu_add_key(&k);
}

static int install_gtk(struct mt7925_dev *dev)
{
	struct mt7925_key_params k;

	anx_memset(&k, 0, sizeof(k));
	k.bss_idx = MT7925_BSS_IDX;
	k.wlan_idx = MT7925_WCID_BSS;
	k.muar_idx = MT7925_MUAR_BSS;
	k.add = true;
	k.pairwise = false;
	anx_memcpy(k.peer_addr, dev->bss.bssid, 6);
	k.cipher = dev->bss.group_cipher == RSN_CIPHER_TKIP ?
		   MT_CIPHER_TKIP : MT_CIPHER_AES_CCMP;
	k.key_id = dev->eapol.gtk_idx;
	k.key_len = dev->eapol.gtk_len;
	k.key = dev->eapol.gtk;
	return mt7925_mcu_add_key(&k);
}

/* Run one received EAPOL frame through the supplicant and act on it. */
static int handle_eapol(struct mt7925_dev *dev, const struct mt7925_frame *f,
			bool *ptk_done)
{
	static uint8_t out[MT7925_EAPOL_MAX];
	uint32_t act, out_len;
	int ret;

	act = mt7925_eapol_rx(&dev->eapol, f->data, f->len, out, &out_len);
	if (!act) {
		kprintf("mt7925: EAPOL frame dropped (reason %u)\n",
			dev->eapol.last_error);
		return ANX_OK;
	}

	/*
	 * Message 4 goes out in the clear even when message 3 was a
	 * retransmission arriving after the key went in: the AP installs
	 * its copy only once message 4 arrives, so it could not decrypt it.
	 */
	if (act & MT7925_EAPOL_SEND) {
		ret = mt7925_tx_eapol(dev, out, out_len,
				      dev->keyed &&
				      !(act & MT7925_EAPOL_INSTALL_PTK));
		if (ret)
			return ret;
	}
	if (act & MT7925_EAPOL_INSTALL_PTK)
		kprintf("mt7925: EAPOL message 3 received, message 4 sent\n");
	else if (act & MT7925_EAPOL_INSTALL_GTK)
		kprintf("mt7925: group key message received, reply sent\n");
	else
		kprintf("mt7925: EAPOL message 1 received, message 2 sent\n");
	if (act & MT7925_EAPOL_INSTALL_PTK) {
		ret = install_ptk(dev);
		if (ret) {
			kprintf("mt7925: pairwise key install failed (%d)\n",
				ret);
			return ret;
		}
		*ptk_done = true;
	}
	if (act & MT7925_EAPOL_INSTALL_GTK) {
		ret = install_gtk(dev);
		if (ret) {
			kprintf("mt7925: group key install failed (%d)\n", ret);
			return ret;
		}
		kprintf("mt7925: group key %u installed\n", dev->eapol.gtk_idx);
	}
	return ANX_OK;
}

static int handshake(struct mt7925_dev *dev)
{
	struct mt7925_frame f;
	struct mt7925_timer t;
	bool ptk_done = false;
	int ret;

	kprintf("mt7925: waiting for the 4-way handshake\n");
	mt7925_timer_start(&t, HANDSHAKE_MS);
	while (!ptk_done && !mt7925_timer_expired(&t)) {
		mt7925_service(dev);
		while (!ptk_done && mt7925_queue_pop(dev, &f)) {
			struct mt7925_mgmt_info info;

			if (f.is_eapol) {
				ret = handle_eapol(dev, &f, &ptk_done);
				if (ret)
					return ret;
				continue;
			}
			if (mt7925_ieee_parse_mgmt(f.data, f.len, &info) == 0 &&
			    anx_memcmp(info.sa, dev->bss.bssid, 6) == 0 &&
			    (info.stype == IEEE80211_STYPE_DEAUTH ||
			     info.stype == IEEE80211_STYPE_DISASSOC)) {
				dev->disconnect_reason = info.status;
				kprintf("mt7925: AP ended the handshake, "
					"reason %u%s\n", info.status,
					info.status == 15 || info.status == 2 ?
					" (wrong passphrase?)" : "");
				return ANX_ECONNRESET;
			}
		}
	}
	if (!ptk_done) {
		kprintf("mt7925: 4-way handshake timed out (last drop reason "
			"%u)\n", dev->eapol.last_error);
		return ANX_ETIMEDOUT;
	}
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Join                                                                 */
/* ------------------------------------------------------------------ */

static void port_authorized(struct mt7925_dev *dev)
{
	struct mt7925_sta_params sp;

	dev->keyed = dev->cipher != MT_CIPHER_NONE;
	dev->state = MT7925_STATE_CONNECTED;
	dev->trace_budget = 32;

	/* mt7925_sta_set_decap_offload(): header translation for the AP */
	ap_sta_params(dev, MT_STA_INFO_STATE_ASSOC, false, &sp);
	mt7925_mcu_sta_hdr_trans(&sp);

	kprintf("mt7925: connected to \"%s\" on channel %u\n",
		dev->ssid, dev->bss.channel);
	if (mt7925_on_connect)
		mt7925_on_connect(dev->ssid);
}

static void default_edca(struct mt7925_edca_params ac[4], bool use_11b)
{
	uint32_t i;

	for (i = 0; i < 4; i++) {
		ac[i].aifs = 2;
		ac[i].cw_min = use_11b ? 31 : 15;
		ac[i].cw_max = 1023;
		ac[i].txop = 0;
	}
}

static int join(struct mt7925_dev *dev, const struct mt7925_bss *bss,
		const char *psk)
{
	static uint8_t frame[512];
	struct mt7925_mgmt_info info;
	struct mt7925_bss_params bp;
	struct mt7925_sta_params sp;
	struct mt7925_edca_params ac[4];
	uint8_t rsn[64];
	uint32_t rsn_len = 0, len;
	bool use_11b;
	int ret;

	dev->bss = *bss;
	dev->aid = 0;
	dev->cipher = MT_CIPHER_NONE;
	dev->keyed = false;
	dev->beacon_lost = false;
	dev->disconnect_reason = 0;
	dev->q_head = dev->q_tail = 0;
	mt7925_ieee_rates(bss, &dev->rates);

	kprintf("mt7925: joining ");
	print_bss(bss);

	if (psk) {
		rsn_len = mt7925_ieee_build_rsn(bss, rsn, sizeof(rsn));
		if (mt7925_eapol_init(&dev->eapol, psk, bss->ssid,
				      bss->ssid_len, bss->bssid, dev->mac,
				      rsn, (uint8_t)rsn_len) != 0) {
			kprintf("mt7925: passphrase must be 8-63 characters "
				"or 64 hex digits\n");
			return ANX_EINVAL;
		}
	}

	/* mt7925_mac_link_sta_add() */
	mt7925_mac_wtbl_clear(MT7925_WCID_AP);
	bss_params(dev, false, &bp);
	ret = mt7925_mcu_add_bss_info(&bp);
	if (ret)
		goto fail;
	ap_sta_params(dev, MT_STA_INFO_STATE_NONE, true, &sp);
	ret = mt7925_mcu_sta_update(&sp);
	if (ret)
		goto fail;

	/* Open System authentication */
	len = mt7925_ieee_build_auth(frame, sizeof(frame), bss->bssid,
				     dev->mac);
	ret = exchange(dev, frame, len, IEEE80211_STYPE_AUTH,
		       "authentication", &info);
	if (ret)
		goto fail;
	if (info.auth_alg != 0 || info.auth_seq != 2 || info.status != 0) {
		kprintf("mt7925: authentication refused (alg %u seq %u "
			"status %u)\n", info.auth_alg, info.auth_seq,
			info.status);
		ret = ANX_EPERM;
		goto fail;
	}
	kprintf("mt7925: authenticated\n");

	/* Association */
	len = mt7925_ieee_build_assoc_req(frame, sizeof(frame), bss, dev->mac,
					  psk ? rsn : NULL, rsn_len);
	if (!len) {
		ret = ANX_EINVAL;
		goto fail;
	}
	ret = exchange(dev, frame, len, IEEE80211_STYPE_ASSOC_RESP,
		       "association", &info);
	if (ret)
		goto fail;
	if (info.status != 0) {
		kprintf("mt7925: association refused, status %u\n",
			info.status);
		ret = ANX_EPERM;
		goto fail;
	}
	dev->aid = info.aid;
	kprintf("mt7925: associated, aid %u\n", dev->aid);

	/* mt7925_mac_link_sta_assoc() */
	bss_params(dev, true, &bp);
	ret = mt7925_mcu_add_bss_info(&bp);
	if (ret)
		goto fail;
	mt7925_mac_wtbl_clear(MT7925_WCID_AP);
	ap_sta_params(dev, MT_STA_INFO_STATE_ASSOC, false, &sp);
	ret = mt7925_mcu_sta_update(&sp);
	if (ret)
		goto fail;

	/* mt7925_vif_cfg_changed(BSS_CHANGED_ASSOC) */
	bss_sta_params(&sp);
	mt7925_mcu_sta_update(&sp);
	mt7925_mcu_set_beacon_filter(dev, true);

	/* mt7925_link_info_changed(): ERP slot, QoS defaults */
	use_11b = bss->band == MT_BAND_2G && !(dev->rates.supp & 0x0ff0);
	mt7925_mcu_set_timing(bss->band != MT_BAND_2G ||
			      (bss->capability & WLAN_CAPABILITY_SHORT_SLOT_TIME) ?
			      9 : 20);
	default_edca(ac, use_11b);
	mt7925_mcu_set_tx(ac);

	dev->state = MT7925_STATE_ASSOC;

	if (psk) {
		ret = handshake(dev);
		if (ret)
			goto fail_assoc;
	}
	port_authorized(dev);
	return ANX_OK;

fail_assoc:
	mt7925_sta_disconnect(dev, REASON_DEAUTH_LEAVING);
	return ret;
fail:
	remove_station(dev);
	dev->state = MT7925_STATE_FW_UP;
	mt7925_eapol_clear(&dev->eapol);
	return ret;
}

int mt7925_sta_connect(struct mt7925_dev *dev, const char *ssid,
		       const char *psk)
{
	uint32_t idx[JOIN_CANDIDATES], n, i;
	bool have_psk = psk && psk[0];
	int ret;

	if (dev->state >= MT7925_STATE_ASSOC)
		mt7925_sta_disconnect(dev, REASON_DEAUTH_LEAVING);

	anx_strlcpy(dev->ssid, ssid, sizeof(dev->ssid));
	anx_strlcpy(dev->psk, have_psk ? psk : "", sizeof(dev->psk));

	kprintf("mt7925: scanning for \"%s\"\n", ssid);
	ret = do_scan(dev, ssid);
	if (ret)
		return ret;
	kprintf("mt7925: scan found %u networks\n", dev->n_scan);

	n = pick_candidates(dev, ssid, have_psk, idx, JOIN_CANDIDATES);
	if (!n) {
		kprintf("mt7925: no usable network named \"%s\"\n", ssid);
		for (i = 0; i < dev->n_scan; i++)
			print_bss(&dev->scan[i]);
		return ANX_ENOENT;
	}

	ret = ANX_ENOENT;
	for (i = 0; i < n; i++) {
		struct mt7925_bss bss = dev->scan[idx[i]];

		ret = join(dev, &bss, have_psk ? dev->psk : NULL);
		if (ret == ANX_OK)
			return ANX_OK;
		/* A rejected passphrase fails the same way on every BSS. */
		if (ret == ANX_EINVAL)
			break;
	}
	kprintf("mt7925: could not connect to \"%s\" (%d)\n", ssid, ret);
	return ret;
}

void mt7925_sta_disconnect(struct mt7925_dev *dev, uint16_t reason)
{
	uint8_t frame[64];
	uint32_t len;

	if (dev->state < MT7925_STATE_ASSOC)
		return;

	len = mt7925_ieee_build_deauth(frame, sizeof(frame), dev->bss.bssid,
				       dev->mac, reason);
	if (len)
		mt7925_tx_mgmt(dev, frame, len, MT7925_WCID_AP);
	anx_delay_ms(20);

	remove_station(dev);
	dev->state = MT7925_STATE_FW_UP;
	dev->keyed = false;
	mt7925_eapol_clear(&dev->eapol);
	kprintf("mt7925: disconnected\n");
	notify_disconnect(dev);
}

static void notify_disconnect(struct mt7925_dev *dev)
{
	(void)dev;
	if (mt7925_on_disconnect)
		mt7925_on_disconnect();
}

/* ------------------------------------------------------------------ */
/* Connected-state work                                                 */
/* ------------------------------------------------------------------ */

/* Forget the association the AP or the firmware already ended. */
static void connection_lost(struct mt7925_dev *dev, const char *why)
{
	kprintf("mt7925: connection lost: %s\n", why);
	remove_station(dev);
	dev->state = MT7925_STATE_FW_UP;
	dev->keyed = false;
	mt7925_eapol_clear(&dev->eapol);
	notify_disconnect(dev);
}

void mt7925_sta_poll(struct mt7925_dev *dev)
{
	struct mt7925_frame f;
	bool ptk_done = false;

	if (dev->state < MT7925_STATE_ASSOC || mt7925_mcu_busy())
		return;

	if (dev->beacon_lost) {
		dev->beacon_lost = false;
		connection_lost(dev, "beacons stopped");
		return;
	}

	while (mt7925_queue_pop(dev, &f)) {
		struct mt7925_mgmt_info info;

		if (f.is_eapol) {
			if (handle_eapol(dev, &f, &ptk_done) != ANX_OK)
				kprintf("mt7925: rekey failed\n");
			continue;
		}
		if (mt7925_ieee_parse_mgmt(f.data, f.len, &info) != 0 ||
		    anx_memcmp(info.sa, dev->bss.bssid, 6) != 0)
			continue;
		if (info.stype == IEEE80211_STYPE_DEAUTH ||
		    info.stype == IEEE80211_STYPE_DISASSOC) {
			dev->disconnect_reason = info.status;
			connection_lost(dev, info.stype ==
					IEEE80211_STYPE_DEAUTH ?
					"deauthenticated" : "disassociated");
			return;
		}
	}
}
