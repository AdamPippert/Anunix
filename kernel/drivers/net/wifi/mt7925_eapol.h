/*
 * mt7925_eapol.h — WPA2-PSK supplicant state for the MT7925 station.
 *
 * Implements the pairwise 4-way handshake and the group key handshake
 * (IEEE 802.11-2020 12.7.6 and 12.7.7) for AKM 00-0f-ac:2 with CCMP
 * pairwise keys: key descriptor version 2, HMAC-SHA1 MICs, AES key wrap.
 * The module consumes and produces EAPOL frames (starting at the EAPOL
 * header, after the Ethernet type) and tells the caller which keys to
 * install; it touches no hardware.
 */

#ifndef ANX_MT7925_EAPOL_H
#define ANX_MT7925_EAPOL_H

#include <anx/types.h>

#define MT7925_EAPOL_SEND		(1U << 0)
#define MT7925_EAPOL_INSTALL_PTK	(1U << 1)
#define MT7925_EAPOL_INSTALL_GTK	(1U << 2)

#define MT7925_EAPOL_MAX		512

struct mt7925_eapol {
	uint8_t  pmk[32];
	uint8_t  ptk[48];		/* KCK | KEK | TK */
	bool     ptk_valid;
	uint8_t  anonce[32];
	uint8_t  snonce[32];
	bool     fixed_snonce;		/* tests preset snonce */
	uint8_t  aa[6];
	uint8_t  spa[6];
	uint8_t  replay[8];
	bool     replay_valid;
	uint8_t  rsn_ie[64];
	uint8_t  rsn_ie_len;
	uint8_t  gtk[32];
	uint8_t  gtk_len;
	uint8_t  gtk_idx;
	uint32_t last_error;		/* MT7925_EAPOL_ERR_* of the last drop */
};

#define MT7925_EAPOL_ERR_NONE		0
#define MT7925_EAPOL_ERR_FORMAT		1
#define MT7925_EAPOL_ERR_VERSION	2
#define MT7925_EAPOL_ERR_REPLAY		3
#define MT7925_EAPOL_ERR_MIC		4
#define MT7925_EAPOL_ERR_NONCE		5
#define MT7925_EAPOL_ERR_KEYDATA	6
#define MT7925_EAPOL_ERR_NO_PTK		7

/*
 * Prepare for a handshake with authenticator aa. pass is a WPA passphrase
 * (8..63 characters) or 64 hex digits of PMK. rsn_ie is the element sent in
 * the association request. Returns 0, or -1 for an unusable passphrase.
 */
int mt7925_eapol_init(struct mt7925_eapol *e, const char *pass,
		      const uint8_t *ssid, uint8_t ssid_len,
		      const uint8_t aa[6], const uint8_t spa[6],
		      const uint8_t *rsn_ie, uint8_t rsn_ie_len);

/*
 * Process one received EAPOL frame. Returns a mask of MT7925_EAPOL_*; when
 * SEND is set, out holds *out_len bytes to transmit to aa with EtherType
 * 0x888e. A return of 0 means the frame was ignored (last_error says why).
 */
uint32_t mt7925_eapol_rx(struct mt7925_eapol *e, const uint8_t *pkt,
			 uint32_t len, uint8_t *out, uint32_t *out_len);

/* Temporal key (16 bytes) once INSTALL_PTK has been returned. */
const uint8_t *mt7925_eapol_tk(const struct mt7925_eapol *e);

/* Forget every key. */
void mt7925_eapol_clear(struct mt7925_eapol *e);

#endif /* ANX_MT7925_EAPOL_H */
