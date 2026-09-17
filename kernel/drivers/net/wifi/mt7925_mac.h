/*
 * mt7925_mac.h — MT7925 per-frame descriptors (connac3 TXD/TXP and RXD).
 *
 * Every data or management frame the host sends is described by a 32-byte
 * TXD followed by a 32-byte TXP that points at the frame; every frame the
 * chip delivers starts with an RXD whose optional groups decide where the
 * payload begins. Both are pure encodings, tested on the host.
 *
 * Sources, Linux v6.19 drivers/net/wireless/mediatek/mt76:
 *   mt76_connac3_mac.h, mt7925/mac.c (mt7925_mac_write_txwi,
 *   mt7925_mac_fill_rx, mt7925_queue_rx_skb, mt7925_mac_tx_free),
 *   mt76_connac_mac.c (mt76_connac_write_hw_txp), mt76_connac.h.
 */

#ifndef ANX_MT7925_MAC_H
#define ANX_MT7925_MAC_H

#include <anx/types.h>

#define MT_TXD_SIZE			32
#define MT_TXP_SIZE			32
#define MT_TXWI_SIZE			(MT_TXD_SIZE + MT_TXP_SIZE)

/* LMAC queues (mt76_connac3_mac.h) */
#define MT_LMAC_AC00			0x00
#define MT_LMAC_ALTX0			0x10

/* mac80211 access categories */
#define MT_AC_VO			0
#define MT_AC_VI			1
#define MT_AC_BE			2
#define MT_AC_BK			3

/* rx_pkt_type (mt76_connac.h) */
#define MT_PKT_TYPE_TXS			0
#define MT_PKT_TYPE_NORMAL		2
#define MT_PKT_TYPE_TXRX_NOTIFY		6
#define MT_PKT_TYPE_RX_EVENT		7
#define MT_PKT_TYPE_NORMAL_MCU		8

/* cipher in RXD2 SEC_MODE (mt76_connac_mac.h enum mt76_cipher_type) */
#define MT_RX_CIPHER_NONE		0

#define MT_TXFREE_MSDU_INVALID		0x7fff

struct mt7925_txd_params {
	uint16_t wlan_idx;
	uint8_t  omac_idx;
	uint8_t  band_idx;		/* 0xff until a channel grant says otherwise */
	uint8_t  q_idx;			/* MT_LMAC_* */
	bool     is_8023;
	bool     protect;		/* a key is installed for this frame */
	bool     mcast;
	bool     no_ack;
	bool     min_rate;		/* IEEE80211_TX_CTL_USE_MINRATE */
	uint8_t  tid;
	uint8_t  rate_idx;		/* fixed-rate table index */
	uint8_t  pid;
};

/*
 * mt7925e_tx_prepare_skb(): write TXD and TXP into txwi (MT_TXWI_SIZE
 * bytes). frame is the frame itself (Ethernet II when p->is_8023, else
 * 802.11); frame_phys is where the device finds it.
 */
void mt7925_mac_write_txwi(uint8_t *txwi, const uint8_t *frame,
			   uint32_t frame_len, uint32_t frame_phys,
			   uint16_t token, const struct mt7925_txd_params *p);

struct mt7925_rx_info {
	uint8_t  pkt_type;		/* MT_PKT_TYPE_* after SW-type fixups */
	uint16_t wlan_idx;
	bool     hdr_trans;		/* payload is Ethernet II */
	bool     unicast;
	bool     fcs_err;
	bool     icv_err;
	bool     decrypted;
	uint8_t  sec_mode;
	uint8_t  channel;
	uint8_t  band;			/* MT_BAND_* */
	bool     has_rssi;
	int8_t   rssi;			/* dBm, chain 0 */
	uint32_t payload_off;
	uint32_t payload_len;
};

/*
 * mt7925_rx_check()/mt7925_queue_rx_skb() classification plus
 * mt7925_mac_fill_rx() header walk. Returns 0 when info is valid; for
 * pkt_type other than NORMAL only pkt_type is filled.
 */
int mt7925_mac_parse_rx(const uint8_t *buf, uint32_t len,
			struct mt7925_rx_info *info);

/*
 * Walk a TXRX_NOTIFY (tx free) report and call release(token, stat) for
 * every MSDU id it returns; stat is the delivery status of the header
 * before the id, 0 when the peer acknowledged. Returns the number of ids
 * released, or -1 when the report is malformed.
 */
int mt7925_mac_parse_tx_free(const uint8_t *buf, uint32_t len,
			     void (*release)(uint16_t token, uint8_t stat,
					     void *arg),
			     void *arg);

/* mt76_connac_lmac_mapping(): LMAC uses the reverse order of mac80211 ACs */
static inline uint8_t mt7925_mac_lmac_queue(uint8_t ac)
{
	return (uint8_t)(3 - ac);
}

#endif /* ANX_MT7925_MAC_H */
