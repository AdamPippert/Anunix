/*
 * mt7925_uni.c — Builders for MT7925 unified commands.
 *
 * Each builder reproduces one Linux v6.19 mt76 function's output byte for
 * byte, for the subset of options a legacy (non-HT) station uses. The Linux
 * function is named above each builder; field order follows it so the two
 * can be read side by side.
 */

#include <anx/types.h>
#include <anx/string.h>
#include "mt7925_uni.h"

#define MCU_PKT_ID		0xa0
#define MCU_S2D_H2N		0
#define TXD0_TX_BYTES_MASK	0xffffU
#define TXD0_PKT_FMT_SHIFT	23
#define TXD0_Q_IDX_SHIFT	25
#define TX_TYPE_CMD		2
#define TX_MCU_PORT_RX_Q0	0x20
#define TXD1_HDR_FORMAT_SHIFT	14
#define HDR_FORMAT_CMD		1

/* ------------------------------------------------------------------ */
/* Message plumbing                                                     */
/* ------------------------------------------------------------------ */

void mt7925_msg_init(struct mt7925_msg *m, uint8_t *buf, uint32_t cap)
{
	m->buf = buf;
	m->len = 0;
	m->cap = cap;
	m->overflow = false;
}

void *mt7925_msg_put(struct mt7925_msg *m, uint32_t len)
{
	uint8_t *p;

	if (m->overflow || m->len + len > m->cap) {
		m->overflow = true;
		return NULL;
	}
	p = m->buf + m->len;
	anx_memset(p, 0, len);
	m->len += len;
	return p;
}

void *mt7925_msg_tlv(struct mt7925_msg *m, uint16_t tag, uint16_t len)
{
	struct mt7925_tlv *t = mt7925_msg_put(m, len);
	uint16_t n;

	if (!t)
		return NULL;
	t->tag = tag;
	t->len = len;

	/* mt76_connac_mcu_add_nested_tlv(): struct sta_ntlv_hdr at skb->data */
	if (m->len >= 4) {
		n = (uint16_t)(m->buf[2] | (m->buf[3] << 8));
		n++;
		m->buf[2] = (uint8_t)n;
		m->buf[3] = (uint8_t)(n >> 8);
	}
	return t;
}

/* mt7925_mcu_fill_message(), unified branch */
void mt7925_uni_fill_txd(struct mt7925_uni_txd *txd, uint16_t cid,
			 uint8_t option, uint8_t seq, uint32_t body_len)
{
	uint32_t total = sizeof(*txd) + body_len;

	anx_memset(txd, 0, sizeof(*txd));
	txd->txd[0] = (total & TXD0_TX_BYTES_MASK) |
		      ((uint32_t)TX_TYPE_CMD << TXD0_PKT_FMT_SHIFT) |
		      ((uint32_t)TX_MCU_PORT_RX_Q0 << TXD0_Q_IDX_SHIFT);
	txd->txd[1] = (uint32_t)HDR_FORMAT_CMD << TXD1_HDR_FORMAT_SHIFT;
	txd->len       = (uint16_t)(total - sizeof(txd->txd));
	txd->cid       = cid;
	txd->s2d_index = MCU_S2D_H2N;
	txd->pkt_type  = MCU_PKT_ID;
	txd->seq       = seq;
	txd->option    = option;

	/* HIF_CTRL and CHIP_CONFIG are sent without an ack request. */
	if (cid == MT_UNI_CMD_CHIP_CONFIG)
		txd->option &= (uint8_t)~MT_UNI_OPT_ACK;
}

const struct mt7925_tlv *mt7925_tlv_next(const uint8_t *body, uint32_t len,
					 uint32_t *pos)
{
	const struct mt7925_tlv *t;

	if (*pos + sizeof(*t) > len)
		return NULL;
	t = (const struct mt7925_tlv *)(body + *pos);
	if (t->len < sizeof(*t) || *pos + t->len > len)
		return NULL;
	*pos += t->len;
	return t;
}

/* ------------------------------------------------------------------ */
/* BSS                                                                  */
/* ------------------------------------------------------------------ */

static void bss_hdr(struct mt7925_msg *m, uint8_t bss_idx)
{
	struct mt7925_bss_req_hdr *h = mt7925_msg_put(m, sizeof(*h));

	if (h)
		h->bss_idx = bss_idx;
}

/* mt7925_mcu_bss_basic_tlv() */
static void bss_basic_tlv(struct mt7925_msg *m,
			  const struct mt7925_bss_params *p)
{
	struct mt7925_bss_basic *b;

	b = mt7925_msg_tlv(m, MT_UNI_BSS_INFO_BASIC, sizeof(*b));
	if (!b)
		return;
	b->hw_bss_idx = p->omac_idx > 0x10 ? 0 : p->omac_idx;
	b->phymode_ext = p->phymode_ext;
	b->nonht_basic_phy = p->band == MT_BAND_2G ? MT_PHY_TYPE_ERP_INDEX
						   : MT_PHY_TYPE_OFDM_INDEX;
	anx_memcpy(b->bssid, p->bssid, 6);
	b->phymode = p->phymode;
	b->bcn_interval = p->bcn_interval;
	b->dtim_period = p->dtim_period;
	b->bmc_tx_wlan_idx = p->bmc_wlan_idx;
	b->link_idx = p->link_idx;
	b->sta_idx = p->sta_wlan_idx;
	b->omac_idx = p->omac_idx;
	b->band_idx = p->band_idx;
	b->wmm_idx = p->wmm_idx;
	b->conn_state = !p->enable;
	b->conn_type = MT_CONNECTION_INFRA_STA;
	b->active = 1;
}

/* mt7925_mcu_bss_sec_tlv() */
static void bss_sec_tlv(struct mt7925_msg *m, uint8_t cipher)
{
	struct mt7925_bss_sec *s;

	s = mt7925_msg_tlv(m, MT_UNI_BSS_INFO_SEC, sizeof(*s));
	if (!s)
		return;
	switch (cipher) {
	case MT_CIPHER_AES_CCMP:
		s->mode = MT_SEC_MODE_WPA2_PSK;
		s->status = 6;
		break;
	case MT_CIPHER_TKIP:
		s->mode = MT_SEC_MODE_WPA2_PSK;
		s->status = 4;
		break;
	default:
		s->mode = MT_SEC_MODE_OPEN;
		s->status = 1;
		break;
	}
	s->cipher = cipher;
}

/* mt7925_mcu_bss_bmc_tlv() */
static void bss_rate_tlv(struct mt7925_msg *m,
			 const struct mt7925_bss_params *p)
{
	struct mt7925_bss_rate *r;

	r = mt7925_msg_tlv(m, MT_UNI_BSS_INFO_RATE, sizeof(*r));
	if (!r)
		return;
	r->basic_rate = p->band == MT_BAND_2G ? MT_HR_DSSS_ERP_BASIC_RATE
					      : MT_OFDM_BASIC_RATE;
	r->short_preamble = p->band == MT_BAND_2G;
	r->bc_fixed_rate = p->rate_idx;
	r->mc_fixed_rate = p->rate_idx;
}

/* mt7925_mcu_bss_qos_tlv() */
static void bss_qos_tlv(struct mt7925_msg *m, bool qos)
{
	struct mt7925_bss_qos *q;

	q = mt7925_msg_tlv(m, MT_UNI_BSS_INFO_QBSS, sizeof(*q));
	if (q)
		q->qos = qos;
}

/* mt7925_mcu_bss_mld_tlv() for a non-MLD interface */
static void bss_mld_tlv(struct mt7925_msg *m,
			const struct mt7925_bss_params *p)
{
	struct mt7925_bss_mld *d;

	d = mt7925_msg_tlv(m, MT_UNI_BSS_INFO_MLD, sizeof(*d));
	if (!d)
		return;
	d->link_id = 0xff;
	d->group_mld_id = 0xff;
	d->own_mld_id = (uint8_t)(p->bss_idx + 32);
	d->remap_idx = 0xff;
	d->eml_enable = 0;
	anx_memcpy(d->mac_addr, p->own_addr, 6);
}

/* mt7925_mcu_bss_ifs_tlv() */
static void bss_ifs_tlv(struct mt7925_msg *m, uint16_t slot_time)
{
	struct mt7925_bss_ifs *f;

	f = mt7925_msg_tlv(m, MT_UNI_BSS_INFO_IFS_TIME, sizeof(*f));
	if (!f)
		return;
	f->slot_valid = 1;
	f->slot_time = slot_time;
}

/* mt7925_mcu_bss_rlm_tlv() for a 20 MHz channel */
static void bss_rlm_tlv(struct mt7925_msg *m,
			const struct mt7925_bss_params *p)
{
	struct mt7925_bss_rlm *r;

	r = mt7925_msg_tlv(m, MT_UNI_BSS_INFO_RLM, sizeof(*r));
	if (!r)
		return;
	r->control_channel = p->channel;
	r->center_chan = p->center_chan;
	r->center_chan2 = 0;
	r->tx_streams = p->nss;
	r->rx_streams = p->nss;
	r->band = p->band;
	r->bw = p->bw;
	r->ht_op_info = p->bw == MT_CMD_CBW_20MHZ ? 0 : 4;
	if (r->control_channel < r->center_chan)
		r->sco = 1;
	else if (r->control_channel > r->center_chan)
		r->sco = 3;
}

/* mt7925_mcu_bss_mbssid_tlv() for a transmitted BSSID */
static void bss_mbssid_tlv(struct mt7925_msg *m)
{
	mt7925_msg_tlv(m, MT_UNI_BSS_INFO_11V_MBSSID,
		       sizeof(struct mt7925_bss_mbssid));
}

/* mt7925_mcu_add_bss_info() */
void mt7925_build_bss_info(struct mt7925_msg *m,
			   const struct mt7925_bss_params *p)
{
	bss_hdr(m, p->bss_idx);
	bss_basic_tlv(m, p);		/* bss_basic must be first */
	bss_sec_tlv(m, p->cipher);
	bss_rate_tlv(m, p);
	bss_qos_tlv(m, p->qos);
	bss_mld_tlv(m, p);
	bss_ifs_tlv(m, p->slot_time);
	if (p->enable) {
		bss_rlm_tlv(m, p);
		bss_mbssid_tlv(m);
	}
}

/* basic_req in mt76_connac_mcu_uni_add_dev() */
void mt7925_build_bss_add(struct mt7925_msg *m,
			  const struct mt7925_bss_params *p)
{
	struct mt7925_bss_req_hdr *h;
	struct mt7925_bss_basic *b;

	h = mt7925_msg_put(m, sizeof(*h));
	b = mt7925_msg_put(m, sizeof(*b));
	if (!h || !b)
		return;
	h->bss_idx = p->bss_idx;
	b->tag = MT_UNI_BSS_INFO_BASIC;
	b->len = sizeof(*b);
	b->omac_idx = p->omac_idx;
	b->band_idx = p->band_idx;
	b->wmm_idx = p->wmm_idx;
	b->active = p->enable;
	b->bmc_tx_wlan_idx = p->bmc_wlan_idx;
	b->sta_idx = p->sta_wlan_idx;
	b->conn_state = 1;
	b->link_idx = p->link_idx;
	b->conn_type = MT_CONNECTION_INFRA_STA;
	b->hw_bss_idx = p->omac_idx > 0x10 ? 0 : p->omac_idx;
}

/* dev_req in mt76_connac_mcu_uni_add_dev() */
void mt7925_build_dev_info(struct mt7925_msg *m, uint8_t omac_idx,
			   uint8_t band_idx, uint8_t link_idx,
			   const uint8_t addr[6], bool active)
{
	struct mt7925_dev_info_req *r = mt7925_msg_put(m, sizeof(*r));

	if (!r)
		return;
	r->omac_idx = omac_idx;
	r->band_idx = band_idx;
	r->tag = MT_DEV_INFO_ACTIVE;
	r->len = sizeof(*r) - 4;
	r->active = active;
	r->link_idx = link_idx;
	anx_memcpy(r->omac_addr, addr, 6);
}

/* mt7925_mcu_uni_bss_bcnft() */
void mt7925_build_bss_bcnft(struct mt7925_msg *m, uint8_t bss_idx,
			    uint16_t bcn_interval, uint8_t dtim_period)
{
	struct mt7925_bss_req_hdr *h = mt7925_msg_put(m, sizeof(*h));
	struct mt7925_bss_bcnft *b = mt7925_msg_put(m, sizeof(*b));

	if (!h || !b)
		return;
	h->bss_idx = bss_idx;
	b->tag = MT_UNI_BSS_INFO_BCNFT;
	b->len = sizeof(*b);
	b->bcn_interval = bcn_interval;
	b->dtim_period = dtim_period;
}

/* req1 in mt7925_mcu_set_bss_pm() */
void mt7925_build_bss_pm_disable(struct mt7925_msg *m, uint8_t bss_idx)
{
	struct mt7925_bss_req_hdr *h = mt7925_msg_put(m, sizeof(*h));
	struct mt7925_tlv *t = mt7925_msg_put(m, sizeof(*t));

	if (!h || !t)
		return;
	h->bss_idx = bss_idx;
	t->tag = MT_UNI_BSS_INFO_PM_DISABLE;
	t->len = sizeof(*t);
}

/* mt7925_mcu_set_timing() */
void mt7925_build_bss_timing(struct mt7925_msg *m, uint8_t bss_idx,
			     uint16_t slot_time)
{
	bss_hdr(m, bss_idx);
	bss_ifs_tlv(m, slot_time);
}

/* ------------------------------------------------------------------ */
/* STA records                                                          */
/* ------------------------------------------------------------------ */

/* __mt76_connac_mcu_alloc_sta_req() */
static void sta_hdr(struct mt7925_msg *m, const struct mt7925_sta_params *p)
{
	struct mt7925_sta_req_hdr *h = mt7925_msg_put(m, sizeof(*h));

	if (!h)
		return;
	h->bss_idx = p->bss_idx;
	h->wlan_idx_lo = (uint8_t)(p->wlan_idx & 0xff);
	h->wlan_idx_hi = (uint8_t)((p->wlan_idx >> 8) & 0x7);
	h->is_tlv_append = 1;
	h->muar_idx = p->muar_idx;
}

/* mt76_connac_mcu_sta_basic_tlv() for a station interface's AP */
static void sta_basic_tlv(struct mt7925_msg *m,
			  const struct mt7925_sta_params *p)
{
	struct mt7925_sta_rec_basic *b;

	b = mt7925_msg_tlv(m, MT_STA_REC_BASIC, sizeof(*b));
	if (!b)
		return;
	b->extra_info = MT_EXTRA_INFO_VER;
	if (p->newly)
		b->extra_info |= MT_EXTRA_INFO_NEW;
	b->conn_state = MT_CONN_STATE_PORT_SECURE;
	b->conn_type = MT_CONNECTION_INFRA_AP;
	b->aid = p->aid;
	anx_memcpy(b->peer_addr, p->peer_addr, 6);
	b->qos = p->qos;
}

/* mt7925_mcu_sta_phy_tlv() without HT/VHT/HE capabilities */
static void sta_phy_tlv(struct mt7925_msg *m,
			const struct mt7925_sta_params *p)
{
	struct mt7925_sta_rec_phy *ph;

	ph = mt7925_msg_tlv(m, MT_STA_REC_PHY, sizeof(*ph));
	if (!ph)
		return;
	ph->phy_type = p->phy_type;
	ph->basic_rate = p->basic_rates;
}

/* mt7925_mcu_sta_rate_ctrl_tlv() */
static void sta_ra_tlv(struct mt7925_msg *m,
		       const struct mt7925_sta_params *p)
{
	struct mt7925_sta_rec_ra *ra;

	ra = mt7925_msg_tlv(m, MT_STA_REC_RA, sizeof(*ra));
	if (ra)
		ra->legacy = p->ra_legacy;
}

/* mt7925_mcu_sta_state_v2_tlv() */
static void sta_state_tlv(struct mt7925_msg *m, uint8_t state)
{
	struct mt7925_sta_rec_state *s;

	s = mt7925_msg_tlv(m, MT_STA_REC_STATE, sizeof(*s));
	if (s)
		s->state = state;
}

/*
 * mt7925_mcu_sta_mld_tlv() for a non-MLD interface: valid_links is 0, so
 * link_num is 0 and no link entries are filled. The primary and wlan ids
 * are the station's own entry.
 */
static void sta_mld_tlv(struct mt7925_msg *m,
			const struct mt7925_sta_params *p)
{
	struct mt7925_sta_rec_mld *d;

	d = mt7925_msg_tlv(m, MT_STA_REC_MLD, sizeof(*d));
	if (!d)
		return;
	anx_memcpy(d->mac_addr, p->peer_addr, 6);
	d->primary_id = p->wlan_idx;
	d->wlan_id = p->wlan_idx;
	d->link_num = 0;
}

/* mt7925_mcu_sta_hdr_trans_tlv() */
static void sta_hdr_trans_tlv(struct mt7925_msg *m,
			      const struct mt7925_sta_params *p)
{
	struct mt7925_sta_rec_hdr_trans *h;

	h = mt7925_msg_tlv(m, MT_STA_REC_HDR_TRANS, sizeof(*h));
	if (!h)
		return;
	h->to_ds = p->to_ds;
	h->from_ds = !p->to_ds;
	h->dis_rx_hdr_tran = p->dis_rx_hdr_tran;
}

/* mt7925_mcu_sta_cmd() */
void mt7925_build_sta_rec(struct mt7925_msg *m,
			  const struct mt7925_sta_params *p)
{
	sta_hdr(m, p);

	if (p->enable && p->has_peer) {
		sta_basic_tlv(m, p);
		sta_phy_tlv(m, p);
		sta_ra_tlv(m, p);
		sta_state_tlv(m, p->state);
		if (p->state != MT_STA_INFO_STATE_NONE)
			sta_mld_tlv(m, p);
	}

	if (!p->enable) {
		struct mt7925_sta_rec_remove *r;

		r = mt7925_msg_tlv(m, MT_STA_REC_REMOVE, sizeof(*r));
		if (r)
			r->action = 0;
		mt7925_msg_tlv(m, MT_STA_REC_MLD_OFF, sizeof(struct mt7925_tlv));
	} else {
		sta_hdr_trans_tlv(m, p);
	}
}

/* mt7925_mcu_wtbl_update_hdr_trans() */
void mt7925_build_sta_hdr_trans(struct mt7925_msg *m,
				const struct mt7925_sta_params *p)
{
	sta_hdr(m, p);
	sta_hdr_trans_tlv(m, p);
}

/* mt7925_mcu_sta_key_tlv() */
void mt7925_build_sta_key(struct mt7925_msg *m,
			  const struct mt7925_key_params *p)
{
	struct mt7925_sta_params h = {
		.bss_idx = p->bss_idx,
		.wlan_idx = p->wlan_idx,
		.muar_idx = p->muar_idx,
	};
	struct mt7925_sta_rec_key *k;

	sta_hdr(m, &h);
	k = mt7925_msg_tlv(m, MT_STA_REC_KEY_V3, sizeof(*k));
	if (!k)
		return;
	k->bss_idx = p->bss_idx;
	k->is_authenticator = 0;
	k->mgmt_prot = 1;
	k->wlan_idx = (uint8_t)p->wlan_idx;
	if (p->pairwise) {
		k->tx_key = 1;
		k->key_type = 1;
	}
	anx_memcpy(k->peer_addr, p->peer_addr, 6);

	if (!p->add)
		return;

	k->add = 1;
	k->cipher_id = p->cipher;
	k->key_id = p->key_id;
	k->key_len = p->key_len;
	if (p->key_len <= sizeof(k->key))
		anx_memcpy(k->key, p->key, p->key_len);
	if (p->cipher == MT_CIPHER_TKIP && p->key_len >= 32) {
		/* Rx/Tx MIC keys are swapped */
		anx_memcpy(k->key + 16, p->key + 24, 8);
		anx_memcpy(k->key + 24, p->key + 16, 8);
	}
}

/* ------------------------------------------------------------------ */
/* Channel control and scanning                                         */
/* ------------------------------------------------------------------ */

/* mt7925_mcu_set_roc() for a 20 MHz join */
void mt7925_build_roc(struct mt7925_msg *m, uint8_t bss_idx, uint8_t token,
		      uint8_t channel, uint8_t band, uint32_t duration_ms)
{
	struct mt7925_roc_acquire *r;

	if (!mt7925_msg_put(m, 4))
		return;
	r = mt7925_msg_put(m, sizeof(*r));
	if (!r)
		return;
	r->tag = MT_UNI_ROC_ACQUIRE;
	r->len = sizeof(*r);
	r->tokenid = token;
	r->reqtype = MT_ROC_REQ_JOIN;
	r->maxinterval = duration_ms;
	r->bss_idx = bss_idx;
	r->control_channel = channel;
	r->bw = MT_CMD_CBW_20MHZ;
	r->bw_from_ap = MT_CMD_CBW_20MHZ;
	r->center_chan = channel;
	r->center_chan_from_ap = channel;
	r->dbdcband = 0xff;
	r->band = band;
}

/* mt7925_mcu_abort_roc() */
void mt7925_build_roc_abort(struct mt7925_msg *m, uint8_t bss_idx,
			    uint8_t token)
{
	struct mt7925_roc_abort *r;

	if (!mt7925_msg_put(m, 4))
		return;
	r = mt7925_msg_put(m, sizeof(*r));
	if (!r)
		return;
	r->tag = MT_UNI_ROC_ABORT;
	r->len = sizeof(*r);
	r->tokenid = token;
	r->bss_idx = bss_idx;
	r->dbdcband = 0xff;
}

static void scan_ie_tlv(struct mt7925_msg *m, uint8_t band,
			const uint8_t *ies, uint16_t len)
{
	struct mt7925_scan_ie *ie;

	if (!ies || !len)
		return;
	ie = mt7925_msg_tlv(m, MT_UNI_SCAN_IE, (uint16_t)(sizeof(*ie) + len));
	if (!ie)
		return;
	anx_memcpy((uint8_t *)(ie + 1), ies, len);
	ie->ies_len = len;
	ie->band = band;
}

/* mt7925_mcu_hw_scan() without 6 GHz RNR and without MAC randomisation */
void mt7925_build_scan(struct mt7925_msg *m,
		       const struct mt7925_scan_params *p)
{
	struct mt7925_scan_hdr *h;
	struct mt7925_scan_req *req;
	struct mt7925_scan_ssid *ssid;
	struct mt7925_scan_bssid *bssid;
	struct mt7925_scan_chan *ch;
	uint8_t n_ssids = 0, i, n;

	h = mt7925_msg_put(m, sizeof(*h));
	if (!h)
		return;
	h->seq_num = p->seq_num;
	h->bss_idx = p->bss_idx;

	req = mt7925_msg_tlv(m, MT_UNI_SCAN_REQ, sizeof(*req));
	if (!req)
		return;
	req->scan_type = p->ssid_len ? 1 : 0;
	req->probe_req_num = p->ssid_len ? 2 : 0;

	ssid = mt7925_msg_tlv(m, MT_UNI_SCAN_SSID, sizeof(*ssid));
	if (!ssid)
		return;
	if (p->ssid_len && p->ssid_len <= 32) {
		ssid->ssids[0].ssid_len = p->ssid_len;
		anx_memcpy(ssid->ssids[0].ssid, p->ssid, p->ssid_len);
		n_ssids = 1;
	}
	ssid->ssid_type = n_ssids ? (1U << 2) : (1U << 0);
	ssid->ssids_num = n_ssids;

	bssid = mt7925_msg_tlv(m, MT_UNI_SCAN_BSSID, sizeof(*bssid));
	if (!bssid)
		return;
	anx_memset(bssid->bssid, 0xff, 6);

	ch = mt7925_msg_tlv(m, MT_UNI_SCAN_CHANNEL, sizeof(*ch));
	if (!ch)
		return;
	n = p->n_channels > MT_SCAN_MAX_CHANNELS ? MT_SCAN_MAX_CHANNELS
						 : p->n_channels;
	ch->channels_num = n;
	for (i = 0; i < n; i++) {
		ch->channels[i].band = p->chan_band[i];
		ch->channels[i].channel_num = p->chan_num[i];
	}
	ch->channel_type = p->n_channels ? 4 : 0;

	req->scan_func |= MT_SCAN_FUNC_SPLIT_SCAN;

	mt7925_msg_tlv(m, MT_UNI_SCAN_MISC, sizeof(struct mt7925_scan_misc));

	/* Probe request IEs go last, 2 GHz before 5 GHz. */
	scan_ie_tlv(m, MT_BAND_2G, p->ies_2g, p->ies_2g_len);
	scan_ie_tlv(m, MT_BAND_5G, p->ies_5g, p->ies_5g_len);
}

/* Linux fls(): 1-based index of the highest set bit. */
static uint8_t fls16(uint16_t v)
{
	uint8_t n = 0;

	while (v) {
		n++;
		v >>= 1;
	}
	return n;
}

/* mt7925_mcu_set_tx() */
void mt7925_build_edca(struct mt7925_msg *m, uint8_t bss_idx,
		       const struct mt7925_edca_params ac[4])
{
	uint8_t i;

	bss_hdr(m, bss_idx);
	for (i = 0; i < 4; i++) {
		struct mt7925_edca *e;

		e = mt7925_msg_tlv(m, 0 /* MCU_EDCA_AC_PARAM */, sizeof(*e));
		if (!e)
			return;
		e->set = 0x0f;		/* WMM_PARAM_SET */
		e->queue = i;
		e->aifs = ac[i].aifs;
		e->txop = ac[i].txop;
		e->cw_min = ac[i].cw_min ? fls16(ac[i].cw_min) : 5;
		e->cw_max = ac[i].cw_max ? fls16(ac[i].cw_max) : 10;
	}
}

/* ------------------------------------------------------------------ */
/* Device-level configuration                                          */
/* ------------------------------------------------------------------ */

/* mt7925_mcu_chip_config() */
void mt7925_build_chip_config(struct mt7925_msg *m, const char *cmd)
{
	struct mt7925_chip_config_req *r = mt7925_msg_put(m, sizeof(*r));
	uint32_t len = (uint32_t)anx_strlen(cmd) + 1;

	if (!r)
		return;
	if (len > sizeof(r->data))
		len = sizeof(r->data);
	r->tag = MT_UNI_CHIP_CONFIG_CHIP_CFG;
	r->len = sizeof(*r) - 4;
	r->data_size = (uint16_t)len;
	anx_memcpy(r->data, cmd, len);
	r->data[sizeof(r->data) - 1] = 0;
}

/* mt7925_mcu_get_nic_capability() request */
void mt7925_build_nic_cap(struct mt7925_msg *m)
{
	struct mt7925_nic_cap_req *r = mt7925_msg_put(m, sizeof(*r));

	if (!r)
		return;
	r->tag = MT_UNI_CHIP_CONFIG_NIC_CAPA;
	r->len = sizeof(*r) - 4;
}

/* mt7925_mcu_fw_log_2_host() */
void mt7925_build_fw_log(struct mt7925_msg *m, uint8_t ctrl)
{
	struct mt7925_fw_log_req *r = mt7925_msg_put(m, sizeof(*r));

	if (!r)
		return;
	r->tag = MT_UNI_WSYS_CONFIG_FW_LOG_CTRL;
	r->len = sizeof(*r) - 4;
	r->ctrl = ctrl;
}

/* mt7925_mcu_set_eeprom() */
void mt7925_build_eeprom_mode(struct mt7925_msg *m)
{
	struct mt7925_eeprom_mode_req *r = mt7925_msg_put(m, sizeof(*r));

	if (!r)
		return;
	r->tag = MT_UNI_EFUSE_BUFFER_MODE;
	r->len = sizeof(*r) - 4;
	r->buffer_mode = 0;	/* EE_MODE_EFUSE */
	r->format = 1;		/* EE_FORMAT_WHOLE */
}

/* mt7925_mcu_read_eeprom() request */
void mt7925_build_efuse_read(struct mt7925_msg *m, uint32_t offset)
{
	struct mt7925_efuse_read_req *r = mt7925_msg_put(m, sizeof(*r));

	if (!r)
		return;
	r->tag = MT_UNI_EFUSE_ACCESS;
	r->len = sizeof(*r) - 4;
	r->addr = offset & ~(uint32_t)(MT_EEPROM_BLOCK_SIZE - 1);
}

/* mt7925_mcu_set_rts_thresh() */
void mt7925_build_rts(struct mt7925_msg *m, uint8_t band_idx, uint32_t thresh)
{
	struct mt7925_rts_req *r = mt7925_msg_put(m, sizeof(*r));

	if (!r)
		return;
	r->band_idx = band_idx;
	r->tag = MT_UNI_BAND_CONFIG_RTS_THRESHOLD;
	r->len = sizeof(*r) - 4;
	r->len_thresh = thresh;
	r->pkt_thresh = 0x2;
}

/* mt7925_mcu_set_rxfilter() */
void mt7925_build_rxfilter(struct mt7925_msg *m, uint8_t band_idx,
			   uint32_t fif, uint8_t bit_op, uint32_t bit_map)
{
	struct mt7925_rxfilter_req *r = mt7925_msg_put(m, sizeof(*r));

	if (!r)
		return;
	r->band_idx = band_idx;
	r->tag = MT_UNI_BAND_CONFIG_RX_FILTER;
	r->len = sizeof(*r) - 4;
	r->mode = fif ? 0 : 1;
	r->fif = fif;
	r->bit_map = bit_map;
	r->bit_op = bit_op;
}

/* mt7925_mcu_set_channel_domain() */
void mt7925_build_domain(struct mt7925_msg *m, const char alpha2[2],
			 const struct mt7925_chan_entry *chans, uint32_t n)
{
	struct mt7925_domain_req *r = mt7925_msg_put(m, sizeof(*r));
	uint8_t counts[4] = { 0, 0, 0, 0 };
	uint8_t band;
	uint32_t i;

	if (!r)
		return;
	r->bw_2g = 0;		/* BW_20_40M */
	r->bw_5g = 3;		/* BW_20_40_80_160M */
	r->bw_6g = 3;
	r->tag = MT_UNI_DOMAIN_CHANNELS;

	/* Channels are grouped 2 GHz, then 5 GHz, then 6 GHz. */
	for (band = MT_BAND_2G; band <= MT_BAND_6G; band++) {
		for (i = 0; i < n; i++) {
			struct mt7925_domain_chan *c;

			if (chans[i].band != band)
				continue;
			c = mt7925_msg_put(m, sizeof(*c));
			if (!c)
				return;
			c->hw_value = chans[i].channel;
			c->flags = chans[i].flags;
			counts[band]++;
		}
	}

	r->alpha2[0] = (uint8_t)alpha2[0];
	r->alpha2[1] = (uint8_t)alpha2[1];
	r->n_2ch = counts[MT_BAND_2G];
	r->n_5ch = counts[MT_BAND_5G];
	r->n_6ch = counts[MT_BAND_6G];
	r->len = (uint16_t)(8 + (counts[1] + counts[2] + counts[3]) *
			    sizeof(struct mt7925_domain_chan));
}

/*
 * mt7925_mcu_build_sku(). With no device-tree limits every entry of
 * struct mt76_power_limits holds the same target power, so the only
 * structure left is which spans are written and which keep 127.
 */
void mt7925_fill_sku(int8_t *sku, uint8_t band, int8_t power)
{
	uint32_t offset = 4, i;

	anx_memset(sku, 127, MT_SKU_POWER_LIMIT);

	if (band == MT_BAND_2G)
		for (i = 0; i < 4; i++)
			sku[i] = power;			/* cck */

	for (i = 0; i < 8; i++)
		sku[offset + i] = power;		/* ofdm */
	offset += 8 * 5;

	for (i = 0; i < 2 * 8; i++)
		sku[offset + i] = power;		/* ht */
	offset += 2 * 8;
	sku[offset++] = power;

	/* vht: each 12-byte row takes the 10 entries of limits->mcs[i] */
	for (i = 0; i < 4; i++) {
		uint32_t j;

		for (j = 0; j < 10; j++)
			sku[offset + j] = power;
		offset += 12;
	}

	for (i = 0; i < 7 * 12; i++)
		sku[offset + i] = power;		/* he */
	offset += 7 * 12;

	for (i = 0; i < 16 * 16 && offset + i < MT_SKU_POWER_LIMIT; i++)
		sku[offset + i] = power;		/* eht */
}

/* One batch of mt7925_mcu_rate_txpower_band() */
void mt7925_build_power_limit(struct mt7925_msg *m, const char alpha2[2],
			      uint8_t band, const uint8_t *chans,
			      const int8_t *power, uint8_t n, bool last)
{
	struct mt7925_power_limit_req *r = mt7925_msg_put(m, sizeof(*r));
	uint8_t i;

	if (!r)
		return;
	r->alpha2[0] = (uint8_t)alpha2[0];
	r->alpha2[1] = (uint8_t)alpha2[1];
	r->n_chan = n;
	r->tag = MT_UNI_POWER_LIMIT_TABLE;
	r->len = sizeof(*r);
	r->band = band;

	for (i = 0; i < n; i++) {
		struct mt7925_sku *s = mt7925_msg_put(m, sizeof(*s));

		if (!s)
			return;
		/* last_msg reflects the channel just written */
		r->last_msg = last && i == n - 1;
		s->channel = chans[i];
		mt7925_fill_sku(s->pwr_limit, band, power[i]);
	}
}

/* __mt7925_mcu_set_clc() for one rule */
void mt7925_build_clc(struct mt7925_msg *m, uint8_t ver, uint8_t idx,
		      uint8_t env, const uint8_t alpha2[2],
		      const uint8_t type[2], const uint8_t *seg,
		      uint32_t seg_len)
{
	struct mt7925_clc_req *r = mt7925_msg_put(m, sizeof(*r));
	uint8_t *d;

	if (!r)
		return;
	r->tag = MT_UNI_POWER_LIMIT_CLC;
	r->len = sizeof(*r) - 4;
	r->idx = idx;
	r->env = env;
	r->ver = ver;
	anx_memcpy(r->alpha2, alpha2, 2);
	anx_memcpy(r->type, type, 2);
	r->size = (uint16_t)seg_len;

	d = mt7925_msg_put(m, seg_len);
	if (d)
		anx_memcpy(d, seg, seg_len);
}
