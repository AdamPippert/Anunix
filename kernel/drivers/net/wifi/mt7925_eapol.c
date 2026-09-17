/*
 * mt7925_eapol.c — WPA2-PSK 4-way and group key handshakes.
 *
 * Offsets below are from the start of the EAPOL header. Behaviour follows
 * wpa_supplicant's src/rsn_supp/wpa.c where the standard leaves a choice:
 * EAPOL version 1 on transmit, key length 0 in messages 2 and 4, and the
 * replay counter only advancing on frames whose MIC verified.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/crypto.h>
#include "mt7925_eapol.h"

#define EAPOL_HLEN		4
#define EAPOL_TYPE_KEY		3
#define KEY_DESC_RSN		2

#define OFF_TYPE		1
#define OFF_BODY_LEN		2
#define OFF_DESC		4
#define OFF_INFO		5
#define OFF_KEY_LEN		7
#define OFF_REPLAY		9
#define OFF_NONCE		17
#define OFF_MIC			81
#define OFF_KD_LEN		97
#define OFF_KD			99

#define KI_VERSION_MASK		0x0007
#define KI_VERSION_AES_SHA1	2
#define KI_PAIRWISE		0x0008
#define KI_INSTALL		0x0040
#define KI_ACK			0x0080
#define KI_MIC			0x0100
#define KI_SECURE		0x0200
#define KI_ENC_KEY_DATA		0x1000

#define KDE_GTK_TYPE		1

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int bytes_cmp(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return a[i] < b[i] ? -1 : 1;
	return 0;
}

/* Constant-time comparison for MICs. */
static bool ct_equal(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	uint8_t d = 0;
	uint32_t i;

	for (i = 0; i < n; i++)
		d |= a[i] ^ b[i];
	return d == 0;
}

int mt7925_eapol_init(struct mt7925_eapol *e, const char *pass,
		      const uint8_t *ssid, uint8_t ssid_len,
		      const uint8_t aa[6], const uint8_t spa[6],
		      const uint8_t *rsn_ie, uint8_t rsn_ie_len)
{
	size_t plen = anx_strlen(pass);
	uint32_t i;

	anx_memset(e, 0, sizeof(*e));
	anx_memcpy(e->aa, aa, 6);
	anx_memcpy(e->spa, spa, 6);
	if (rsn_ie_len > sizeof(e->rsn_ie))
		return -1;
	anx_memcpy(e->rsn_ie, rsn_ie, rsn_ie_len);
	e->rsn_ie_len = rsn_ie_len;

	if (plen == 64) {
		for (i = 0; i < 32; i++) {
			int hi = hexval(pass[2 * i]), lo = hexval(pass[2 * i + 1]);

			if (hi < 0 || lo < 0)
				return -1;
			e->pmk[i] = (uint8_t)((hi << 4) | lo);
		}
		return 0;
	}
	if (plen < 8 || plen > 63)
		return -1;
	anx_pbkdf2_hmac_sha1(pass, (uint32_t)plen, ssid, ssid_len, 4096,
			     e->pmk, sizeof(e->pmk));
	return 0;
}

void mt7925_eapol_clear(struct mt7925_eapol *e)
{
	anx_memset(e->pmk, 0, sizeof(e->pmk));
	anx_memset(e->ptk, 0, sizeof(e->ptk));
	anx_memset(e->gtk, 0, sizeof(e->gtk));
	e->ptk_valid = false;
	e->gtk_len = 0;
}

const uint8_t *mt7925_eapol_tk(const struct mt7925_eapol *e)
{
	return e->ptk + 32;
}

/* PTK = PRF-384(PMK, "Pairwise key expansion", Min/Max(AA,SPA) || Min/Max(ANonce,SNonce)) */
static void derive_ptk(struct mt7925_eapol *e)
{
	static const char label[] = "Pairwise key expansion";
	uint8_t data[76];
	bool aa_low = bytes_cmp(e->aa, e->spa, 6) < 0;
	bool an_low = bytes_cmp(e->anonce, e->snonce, 32) < 0;

	anx_memcpy(data, aa_low ? e->aa : e->spa, 6);
	anx_memcpy(data + 6, aa_low ? e->spa : e->aa, 6);
	anx_memcpy(data + 12, an_low ? e->anonce : e->snonce, 32);
	anx_memcpy(data + 44, an_low ? e->snonce : e->anonce, 32);
	anx_prf_sha1(e->pmk, sizeof(e->pmk), label, sizeof(label) - 1,
		     data, sizeof(data), e->ptk, sizeof(e->ptk));
	e->ptk_valid = true;
}

static void compute_mic(const uint8_t *kck, const uint8_t *frame,
			uint32_t len, uint8_t mic[16])
{
	uint8_t full[20];

	anx_hmac_sha1(kck, 16, frame, len, full);
	anx_memcpy(mic, full, 16);
	anx_memset(full, 0, sizeof(full));
}

static bool verify_mic(const struct mt7925_eapol *e, const uint8_t *pkt,
		       uint32_t len)
{
	uint8_t copy[MT7925_EAPOL_MAX], mic[16];

	if (len > sizeof(copy))
		return false;
	anx_memcpy(copy, pkt, len);
	anx_memset(copy + OFF_MIC, 0, 16);
	compute_mic(e->ptk, copy, len, mic);
	return ct_equal(mic, pkt + OFF_MIC, 16);
}

/* Build an EAPOL-Key reply; the MIC is computed over the finished frame. */
static uint32_t build_reply(const struct mt7925_eapol *e, uint16_t info,
			    const uint8_t *replay, const uint8_t *nonce,
			    const uint8_t *kd, uint16_t kd_len, uint8_t *out)
{
	uint32_t len = OFF_KD + kd_len;
	uint8_t mic[16];

	anx_memset(out, 0, len);
	out[0] = 1;				/* EAPOL version */
	out[OFF_TYPE] = EAPOL_TYPE_KEY;
	put_be16(out + OFF_BODY_LEN, (uint16_t)(len - EAPOL_HLEN));
	out[OFF_DESC] = KEY_DESC_RSN;
	put_be16(out + OFF_INFO, info);
	put_be16(out + OFF_KEY_LEN, 0);
	anx_memcpy(out + OFF_REPLAY, replay, 8);
	if (nonce)
		anx_memcpy(out + OFF_NONCE, nonce, 32);
	put_be16(out + OFF_KD_LEN, kd_len);
	if (kd_len)
		anx_memcpy(out + OFF_KD, kd, kd_len);

	compute_mic(e->ptk, out, len, mic);
	anx_memcpy(out + OFF_MIC, mic, 16);
	return len;
}

/* Find the GTK KDE (dd len 00-0f-ac:1 keyid rsv gtk) in key data. */
static int parse_gtk(struct mt7925_eapol *e, const uint8_t *kd, uint32_t len)
{
	uint32_t pos = 0;

	while (pos + 2 <= len) {
		uint8_t id = kd[pos], elen = kd[pos + 1];
		const uint8_t *p = kd + pos + 2;

		if (id == 0xdd && elen == 0)
			break;			/* padding */
		if (pos + 2 + elen > len)
			break;
		if (id == 0xdd && elen >= 6 && p[0] == 0x00 && p[1] == 0x0f &&
		    p[2] == 0xac && p[3] == KDE_GTK_TYPE) {
			uint8_t glen = (uint8_t)(elen - 6);

			if (glen > sizeof(e->gtk))
				return -1;
			e->gtk_idx = p[4] & 0x03;
			e->gtk_len = glen;
			anx_memcpy(e->gtk, p + 6, glen);
			return 0;
		}
		pos += 2 + (uint32_t)elen;
	}
	return -1;
}

static int unwrap_key_data(struct mt7925_eapol *e, const uint8_t *pkt,
			   uint16_t kd_len)
{
	uint8_t plain[MT7925_EAPOL_MAX];
	int ret;

	if (kd_len < 16 || kd_len % 8 || kd_len - 8u > sizeof(plain))
		return -1;
	if (anx_aes128_unwrap(e->ptk + 16, pkt + OFF_KD, kd_len,
			      plain, kd_len - 8u) != 0)
		return -1;
	ret = parse_gtk(e, plain, kd_len - 8u);
	anx_memset(plain, 0, sizeof(plain));
	return ret;
}

static bool replay_ok(const struct mt7925_eapol *e, const uint8_t *pkt)
{
	return !e->replay_valid ||
	       bytes_cmp(pkt + OFF_REPLAY, e->replay, 8) > 0;
}

static uint32_t drop(struct mt7925_eapol *e, uint32_t why)
{
	e->last_error = why;
	return 0;
}

uint32_t mt7925_eapol_rx(struct mt7925_eapol *e, const uint8_t *pkt,
			 uint32_t len, uint8_t *out, uint32_t *out_len)
{
	uint16_t info, kd_len;
	uint32_t flen;

	*out_len = 0;
	e->last_error = MT7925_EAPOL_ERR_NONE;

	if (len < OFF_KD || pkt[OFF_TYPE] != EAPOL_TYPE_KEY ||
	    pkt[OFF_DESC] != KEY_DESC_RSN)
		return drop(e, MT7925_EAPOL_ERR_FORMAT);
	flen = EAPOL_HLEN + be16(pkt + OFF_BODY_LEN);
	kd_len = be16(pkt + OFF_KD_LEN);
	if (flen > len || flen > MT7925_EAPOL_MAX || OFF_KD + kd_len > flen)
		return drop(e, MT7925_EAPOL_ERR_FORMAT);

	info = be16(pkt + OFF_INFO);
	if ((info & KI_VERSION_MASK) != KI_VERSION_AES_SHA1)
		return drop(e, MT7925_EAPOL_ERR_VERSION);
	if (!(info & KI_ACK))
		return drop(e, MT7925_EAPOL_ERR_FORMAT);
	if (!replay_ok(e, pkt))
		return drop(e, MT7925_EAPOL_ERR_REPLAY);

	/* Message 1 of 4 */
	if ((info & KI_PAIRWISE) && !(info & KI_MIC)) {
		anx_memcpy(e->anonce, pkt + OFF_NONCE, 32);
		if (!e->fixed_snonce)
			anx_random_bytes(e->snonce, sizeof(e->snonce));
		derive_ptk(e);
		*out_len = build_reply(e, KI_VERSION_AES_SHA1 | KI_PAIRWISE |
				       KI_MIC, pkt + OFF_REPLAY, e->snonce,
				       e->rsn_ie, e->rsn_ie_len, out);
		return MT7925_EAPOL_SEND;
	}

	if (!e->ptk_valid)
		return drop(e, MT7925_EAPOL_ERR_NO_PTK);
	if (!(info & KI_MIC) || !verify_mic(e, pkt, flen))
		return drop(e, MT7925_EAPOL_ERR_MIC);

	/* Message 3 of 4 */
	if (info & KI_PAIRWISE) {
		if (!(info & KI_INSTALL))
			return drop(e, MT7925_EAPOL_ERR_FORMAT);
		if (bytes_cmp(e->anonce, pkt + OFF_NONCE, 32) != 0)
			return drop(e, MT7925_EAPOL_ERR_NONCE);
		anx_memcpy(e->replay, pkt + OFF_REPLAY, 8);
		e->replay_valid = true;

		e->gtk_len = 0;
		if ((info & KI_ENC_KEY_DATA) &&
		    unwrap_key_data(e, pkt, kd_len) != 0)
			return drop(e, MT7925_EAPOL_ERR_KEYDATA);

		*out_len = build_reply(e, KI_VERSION_AES_SHA1 | KI_PAIRWISE |
				       KI_MIC | KI_SECURE, pkt + OFF_REPLAY,
				       NULL, NULL, 0, out);
		return MT7925_EAPOL_SEND | MT7925_EAPOL_INSTALL_PTK |
		       (e->gtk_len ? MT7925_EAPOL_INSTALL_GTK : 0);
	}

	/* Group message 1 of 2 */
	if (!(info & KI_ENC_KEY_DATA))
		return drop(e, MT7925_EAPOL_ERR_FORMAT);
	anx_memcpy(e->replay, pkt + OFF_REPLAY, 8);
	e->replay_valid = true;
	if (unwrap_key_data(e, pkt, kd_len) != 0)
		return drop(e, MT7925_EAPOL_ERR_KEYDATA);

	*out_len = build_reply(e, KI_VERSION_AES_SHA1 | KI_MIC | KI_SECURE,
			       pkt + OFF_REPLAY, NULL, NULL, 0, out);
	return MT7925_EAPOL_SEND | MT7925_EAPOL_INSTALL_GTK;
}
