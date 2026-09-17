/*
 * mt7925_mac.c — MT7925 per-frame descriptors.
 */

#include <anx/types.h>
#include <anx/string.h>
#include "mt7925_mac.h"
#include "mt7925_uni.h"

/* TXD fields, mt76_connac3_mac.h */
#define TXD0_Q_IDX_SHIFT	25
#define TXD0_PKT_FMT_SHIFT	23
#define TXD0_TX_BYTES_MASK	0xffffU
#define TX_TYPE_CT		0

#define TXD1_FIXED_RATE		(1U << 31)
#define TXD1_OWN_MAC_SHIFT	25
#define TXD1_OWN_MAC_MASK	0x3fU
#define TXD1_TID_SHIFT		21
#define TXD1_TID_MASK		0xfU
#define TXD1_ETH_802_3		(1U << 20)
#define TXD1_HDR_INFO_SHIFT	16
#define TXD1_HDR_INFO_MASK	0x1fU
#define TXD1_HDR_FORMAT_SHIFT	14
#define TXD1_TGID_SHIFT		12
#define TXD1_TGID_MASK		0x3U
#define TXD1_WLAN_IDX_MASK	0xfffU
#define HDR_FORMAT_802_3	0
#define HDR_FORMAT_802_11	2

#define TXD2_FRAME_TYPE_SHIFT	4
#define TXD2_SUB_TYPE_MASK	0xfU

#define TXD3_BA_DISABLE		(1U << 28)
#define TXD3_REM_TX_COUNT_SHIFT	11
#define TXD3_BCM		(1U << 4)
#define TXD3_PROTECT_FRAME	(1U << 1)
#define TXD3_NO_ACK		(1U << 0)

#define TXD5_TX_STATUS_HOST	(1U << 10)
#define TXD5_PID_MASK		0xffU
#define PACKET_ID_FIRST		3

#define TXD6_TX_RATE_SHIFT	16
#define TXD6_TX_RATE_MASK	0x3fU
#define TXD6_MSDU_CNT_SHIFT	4
#define TXD6_DIS_MAT		(1U << 3)
#define TXD6_DAS		(1U << 2)

#define MSDU_ID_VALID		(1U << 15)
#define TXD_LEN_LAST		(1U << 15)
#define TXD_LEN_MASK		0x0fffU

/* RXD fields */
#define RXD0_PKT_TYPE(v)	(((v) >> 27) & 0x1f)
#define RXD0_PKT_FLAG(v)	(((v) >> 16) & 0xf)
#define RXD0_SW_TYPE(v)		(((v) >> 16) & 0xffff)
#define SW_PKT_TYPE_MAP		0x380f
#define SW_PKT_TYPE_FRAME	0x3801

#define RXD1_WLAN_IDX(v)	((v) & 0xfff)
#define RXD1_GROUP_1		(1U << 16)
#define RXD1_GROUP_2		(1U << 17)
#define RXD1_GROUP_3		(1U << 18)
#define RXD1_GROUP_4		(1U << 19)
#define RXD1_GROUP_5		(1U << 20)
#define RXD1_CM			(1U << 23)
#define RXD1_CLM		(1U << 24)
#define RXD1_ICV_ERR		(1U << 25)

#define RXD2_HDR_TRANS		(1U << 7)
#define RXD2_HDR_OFFSET(v)	(((v) >> 13) & 0x7)
#define RXD2_SEC_MODE(v)	(((v) >> 16) & 0x1f)
#define RXD2_AMSDU_ERR		(1U << 23)
#define RXD2_MAX_LEN_ERROR	(1U << 24)
#define RXD2_HDR_TRANS_ERROR	(1U << 25)
#define RXD2_FRAG		(1U << 27)

#define RXD3_CH_FREQ(v)		(((v) >> 8) & 0xff)
#define RXD3_ADDR_TYPE(v)	(((v) >> 16) & 0x3)
#define RXD3_U2M		1
#define RXD3_FCS_ERR		(1U << 24)

#define TXFREE0_MSDU_CNT(v)	(((v) >> 16) & 0x3ff)
#define TXFREE1_VER(v)		(((v) >> 16) & 0xf)
#define TXFREE_INFO_PAIR	(1U << 31)
#define TXFREE_INFO_HEADER	(1U << 30)
#define TXFREE_INFO_STAT(v)	(((v) >> 28) & 0x3)

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/* ------------------------------------------------------------------ */
/* TX                                                                   */
/* ------------------------------------------------------------------ */

void mt7925_mac_write_txwi(uint8_t *txwi, const uint8_t *frame,
			   uint32_t frame_len, uint32_t frame_phys,
			   uint16_t token, const struct mt7925_txd_params *p)
{
	uint32_t txd[8] = { 0 };
	uint8_t *txp = txwi + MT_TXD_SIZE;
	bool fixed = false;
	uint32_t i;

	txd[0] = ((frame_len + MT_TXD_SIZE) & TXD0_TX_BYTES_MASK) |
		 ((uint32_t)TX_TYPE_CT << TXD0_PKT_FMT_SHIFT) |
		 ((uint32_t)p->q_idx << TXD0_Q_IDX_SHIFT);

	txd[1] = (p->wlan_idx & TXD1_WLAN_IDX_MASK) |
		 (((uint32_t)p->omac_idx & TXD1_OWN_MAC_MASK) <<
		  TXD1_OWN_MAC_SHIFT);
	if (p->band_idx)
		txd[1] |= ((uint32_t)p->band_idx & TXD1_TGID_MASK) <<
			  TXD1_TGID_SHIFT;

	txd[3] = 15U << TXD3_REM_TX_COUNT_SHIFT;
	if (p->protect)
		txd[3] |= TXD3_PROTECT_FRAME;
	if (p->no_ack)
		txd[3] |= TXD3_NO_ACK;

	txd[5] = p->pid & TXD5_PID_MASK;
	if (p->pid >= PACKET_ID_FIRST) {
		txd[5] |= TXD5_TX_STATUS_HOST;
		txd[3] |= TXD3_BA_DISABLE;
	}

	txd[6] = TXD6_DAS | (1U << TXD6_MSDU_CNT_SHIFT) | TXD6_DIS_MAT;

	if (p->is_8023) {
		/* mt7925_mac_write_txwi_8023(), non-WMM peer */
		uint16_t ethertype = (uint16_t)((frame[12] << 8) | frame[13]);

		txd[1] |= ((uint32_t)HDR_FORMAT_802_3 << TXD1_HDR_FORMAT_SHIFT) |
			  (((uint32_t)p->tid & TXD1_TID_MASK) << TXD1_TID_SHIFT);
		if (ethertype >= 0x0600)
			txd[1] |= TXD1_ETH_802_3;
		txd[2] |= (0x08U >> 2) << TXD2_FRAME_TYPE_SHIFT;
	} else {
		/* mt7925_mac_write_txwi_80211() */
		uint16_t fc = (uint16_t)(frame[0] | (frame[1] << 8));
		uint8_t type = (uint8_t)((fc & 0x000c) >> 2);
		uint8_t stype = (uint8_t)((fc & 0x00f0) >> 4);
		uint8_t tid = p->tid;
		uint32_t hdrlen = 24;

		if (type == 0)
			tid = 0;		/* MT_TX_NORMAL */
		else if (fc & 0x0080)
			hdrlen += 2;		/* QoS data */

		txd[1] |= ((uint32_t)HDR_FORMAT_802_11 << TXD1_HDR_FORMAT_SHIFT) |
			  (((hdrlen / 2) & TXD1_HDR_INFO_MASK) <<
			   TXD1_HDR_INFO_SHIFT) |
			  (((uint32_t)tid & TXD1_TID_MASK) << TXD1_TID_SHIFT);
		if (type != 2 || p->mcast || p->min_rate)
			fixed = true;

		txd[2] |= ((uint32_t)type << TXD2_FRAME_TYPE_SHIFT) |
			  (stype & TXD2_SUB_TYPE_MASK);
		if (p->mcast)
			txd[3] |= TXD3_BCM;
	}

	if (fixed) {
		txd[1] |= TXD1_FIXED_RATE;
		txd[6] |= ((uint32_t)p->rate_idx & TXD6_TX_RATE_MASK) <<
			  TXD6_TX_RATE_SHIFT;
		txd[3] |= TXD3_BA_DISABLE;
	}

	for (i = 0; i < 8; i++)
		wr32(txwi + i * 4, txd[i]);

	/* mt76_connac_write_hw_txp(): one buffer, marked last */
	anx_memset(txp, 0, MT_TXP_SIZE);
	wr16(txp, (uint16_t)(token | MSDU_ID_VALID));
	wr32(txp + 8, frame_phys);
	wr16(txp + 12, (uint16_t)((frame_len & TXD_LEN_MASK) | TXD_LEN_LAST));
}

/* ------------------------------------------------------------------ */
/* RX                                                                   */
/* ------------------------------------------------------------------ */

static uint8_t classify(uint32_t rxd0)
{
	uint8_t type = (uint8_t)RXD0_PKT_TYPE(rxd0);

	if (type != MT_PKT_TYPE_NORMAL &&
	    (RXD0_SW_TYPE(rxd0) & SW_PKT_TYPE_MAP) == SW_PKT_TYPE_FRAME)
		type = MT_PKT_TYPE_NORMAL;
	if (type == MT_PKT_TYPE_RX_EVENT && RXD0_PKT_FLAG(rxd0) == 0x1)
		type = MT_PKT_TYPE_NORMAL_MCU;
	return type;
}

int mt7925_mac_parse_rx(const uint8_t *buf, uint32_t len,
			struct mt7925_rx_info *info)
{
	uint32_t rxd1, rxd2, rxd3, off = 32, gap;
	uint8_t ch;

	anx_memset(info, 0, sizeof(*info));
	if (len < 4)
		return -1;
	info->pkt_type = classify(rd32(buf));
	if (info->pkt_type != MT_PKT_TYPE_NORMAL &&
	    info->pkt_type != MT_PKT_TYPE_NORMAL_MCU)
		return 0;
	if (len < 32)
		return -1;

	rxd1 = rd32(buf + 4);
	rxd2 = rd32(buf + 8);
	rxd3 = rd32(buf + 12);

	if (rxd2 & (RXD2_AMSDU_ERR | RXD2_MAX_LEN_ERROR))
		return -1;
	info->hdr_trans = (rxd2 & RXD2_HDR_TRANS) != 0;
	if (info->hdr_trans && (rxd1 & RXD1_CM))
		return -1;
	/* A translated fragment would need its 802.11 header rebuilt. */
	if (info->hdr_trans && (rxd2 & RXD2_FRAG))
		return -1;

	info->icv_err = (rxd1 & RXD1_ICV_ERR) != 0;
	info->fcs_err = (rxd3 & RXD3_FCS_ERR) != 0;
	info->wlan_idx = (uint16_t)RXD1_WLAN_IDX(rxd1);
	info->unicast = RXD3_ADDR_TYPE(rxd3) == RXD3_U2M;
	info->sec_mode = (uint8_t)RXD2_SEC_MODE(rxd2);
	info->decrypted = info->sec_mode != MT_RX_CIPHER_NONE &&
			  !(rxd1 & (RXD1_CLM | RXD1_CM));

	/* mt792x_get_status_freq_info() */
	ch = (uint8_t)RXD3_CH_FREQ(rxd3);
	if (ch > 180) {
		info->band = MT_BAND_6G;
		info->channel = (uint8_t)((ch - 181) * 4 + 1);
	} else {
		info->band = ch > 14 ? MT_BAND_5G : MT_BAND_2G;
		info->channel = ch;
	}

	if (rxd1 & RXD1_GROUP_4) {
		off += 16;
		if (off >= len)
			return -1;
	}
	if (rxd1 & RXD1_GROUP_1) {
		off += 16;
		if (off >= len)
			return -1;
	}
	if (rxd1 & RXD1_GROUP_2) {
		off += 16;
		if (off >= len)
			return -1;
	}
	if (rxd1 & RXD1_GROUP_3) {
		uint32_t v3;

		if (off + 16 > len)
			return -1;
		v3 = rd32(buf + off + 12);
		info->has_rssi = true;
		info->rssi = (int8_t)(((int)(v3 & 0xff) - 220) / 2);
		off += 16;
		if (off >= len)
			return -1;
		if (rxd1 & RXD1_GROUP_5) {
			off += 96;
			if (off >= len)
				return -1;
		}
	}

	gap = off + 2 * RXD2_HDR_OFFSET(rxd2);
	if (gap >= len)
		return -1;

	/*
	 * On a translation failure the chip inserts a 2-byte length after
	 * the VLAN tag. Linux shifts the header over it; this driver has no
	 * VLAN users, so the frame is dropped instead.
	 */
	if (info->hdr_trans && (rxd2 & RXD2_HDR_TRANS_ERROR) &&
	    len >= gap + 14 && buf[gap + 12] == 0x81 && buf[gap + 13] == 0x00)
		return -1;

	info->payload_off = gap;
	info->payload_len = len - gap;
	return 0;
}

int mt7925_mac_parse_tx_free(const uint8_t *buf, uint32_t len,
			     void (*release)(uint16_t token, uint8_t stat,
					     void *arg),
			     void *arg)
{
	uint32_t total, count = 0, pos;
	uint8_t stat = 0;

	if (len < 8 || TXFREE1_VER(rd32(buf + 4)) < 4)
		return -1;
	total = TXFREE0_MSDU_CNT(rd32(buf));

	for (pos = 8; count < total; pos += 4) {
		uint32_t info, i;

		if (pos + 4 > len)
			return -1;
		info = rd32(buf + pos);
		if (info & TXFREE_INFO_PAIR)
			continue;
		if (info & TXFREE_INFO_HEADER) {
			stat = (uint8_t)TXFREE_INFO_STAT(info);
			continue;
		}

		for (i = 0; i < 2; i++) {
			uint16_t msdu = (uint16_t)((info >> (15 * i)) &
						   MT_TXFREE_MSDU_INVALID);

			if (msdu == MT_TXFREE_MSDU_INVALID)
				continue;
			count++;
			release(msdu, stat, arg);
		}
	}
	return (int)count;
}
