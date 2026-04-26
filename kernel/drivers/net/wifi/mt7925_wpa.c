/*
 * mt7925_wpa.c — WPA2-PSK 4-way handshake for MT7925.
 *
 * After 802.11 association (triggered by BSS_INFO_UPDATE), the AP sends
 * EAPOL-Key frames over the data ring.  This module:
 *   1. Derives PMK via PBKDF2-HMAC-SHA1
 *   2. Performs the 4-way handshake (M1→M2→M3→M4)
 *   3. Installs PTK (TK) and GTK into the MCU via SET_KEY
 *
 * All EAPOL frames are plain Ethernet frames (EtherType 0x888E) — the
 * MCU handles 802.11 framing.  No encryption is used on EAPOL frames
 * during the handshake; the MIC field provides integrity protection.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/crypto.h>
#include <anx/kprintf.h>
#include <anx/net.h>
#include "mt7925_drv.h"
#include "mt7925_reg.h"

/* ------------------------------------------------------------------ */
/* EAPOL-Key frame layout (offsets from start of Ethernet frame)      */
/* ------------------------------------------------------------------ */

/* Ethernet header: dst[6] src[6] etype[2] = 14 bytes */
#define ETH_DST       0
#define ETH_SRC       6
#define ETH_TYPE      12
#define ETH_HLEN      14

/* EAPOL header at ETH_HLEN */
#define EAPOL_VER     14  /* 1 byte: 1 or 2 */
#define EAPOL_TYPE    15  /* 1 byte: 3 = Key */
#define EAPOL_PLEN    16  /* 2 bytes BE: payload length */

/* EAPOL-Key body at ETH_HLEN + 4 */
#define KEY_DESC      18  /* 1 byte: 2 = RSN */
#define KEY_INFO      19  /* 2 bytes BE */
#define KEY_LEN       21  /* 2 bytes BE: cipher key length */
#define KEY_RCTR      23  /* 8 bytes: replay counter */
#define KEY_NONCE     31  /* 32 bytes: ANonce / SNonce */
#define KEY_IV        63  /* 16 bytes: IV (zeros in WPA2) */
#define KEY_RSC       79  /* 8 bytes: receive sequence counter */
#define KEY_RSVD      87  /* 8 bytes: reserved */
#define KEY_MIC       95  /* 16 bytes: message integrity code */
#define KEY_DLEN     111  /* 2 bytes BE: key data length */
#define KEY_DATA     113  /* variable: key data */

/* Minimum EAPOL-Key frame size (no key data) */
#define EAPOL_MIN    113

/* Key Info bits (field is big-endian) */
#define KI_VER_MASK  0x0007   /* Key Descriptor Version */
#define KI_TYPE      0x0008   /* 1 = pairwise */
#define KI_INSTALL   0x0040   /* Install */
#define KI_ACK       0x0080   /* Key Ack */
#define KI_MIC       0x0100   /* Key MIC */
#define KI_SECURE    0x0200   /* Secure */
#define KI_ENCDATA   0x1000   /* Encrypted Key Data */

/* EtherType for EAPOL */
#define ETH_TYPE_EAPOL  0x888E

/* ------------------------------------------------------------------ */
/* RSN IE to include in Message 2 (WPA2-CCMP-PSK, no capabilities)   */
/* ------------------------------------------------------------------ */

static const uint8_t rsnie[] = {
	0x30,                         /* Element ID: RSN */
	0x14,                         /* Length: 20 */
	0x01, 0x00,                   /* Version: 1 */
	0x00, 0x0f, 0xac, 0x04,       /* Group cipher: CCMP */
	0x01, 0x00,                   /* Pairwise count: 1 */
	0x00, 0x0f, 0xac, 0x04,       /* Pairwise: CCMP */
	0x01, 0x00,                   /* AKM count: 1 */
	0x00, 0x0f, 0xac, 0x02,       /* AKM: PSK */
	0x00, 0x00,                   /* RSN Capabilities */
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* Compare two 32-byte nonces lexicographically */
static int nonce_cmp(const uint8_t *a, const uint8_t *b)
{
	uint32_t i;

	for (i = 0; i < 32; i++) {
		if (a[i] < b[i]) return -1;
		if (a[i] > b[i]) return  1;
	}
	return 0;
}

/* Compare two 6-byte MACs lexicographically */
static int mac_cmp(const uint8_t *a, const uint8_t *b)
{
	uint32_t i;

	for (i = 0; i < 6; i++) {
		if (a[i] < b[i]) return -1;
		if (a[i] > b[i]) return  1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Key derivation                                                      */
/* ------------------------------------------------------------------ */

/* PMK = PBKDF2(HMAC-SHA1, PSK, SSID, 4096, 32) */
static void derive_pmk(const char *psk, const char *ssid, uint8_t pmk[32])
{
	anx_pbkdf2_hmac_sha1(psk, (uint32_t)anx_strlen(psk),
			     ssid, (uint32_t)anx_strlen(ssid),
			     4096, pmk, 32);
}

/*
 * PTK = PRF-512(PMK, "Pairwise key expansion",
 *               min(AP,STA) || max(AP,STA) || min(AN,SN) || max(AN,SN))
 * Layout: KCK[0:16] | KEK[16:32] | TK[32:48] | TK2[48:64]
 */
static void derive_ptk(const uint8_t pmk[32],
		       const uint8_t ap_mac[6], const uint8_t sta_mac[6],
		       const uint8_t anonce[32], const uint8_t snonce[32],
		       uint8_t ptk[64])
{
	static const char label[] = "Pairwise key expansion";
	uint8_t data[76];  /* 6+6+32+32 */
	const uint8_t *mac_lo, *mac_hi, *non_lo, *non_hi;

	mac_lo = (mac_cmp(ap_mac, sta_mac) < 0) ? ap_mac : sta_mac;
	mac_hi = (mac_cmp(ap_mac, sta_mac) < 0) ? sta_mac : ap_mac;
	non_lo = (nonce_cmp(anonce, snonce) < 0) ? anonce : snonce;
	non_hi = (nonce_cmp(anonce, snonce) < 0) ? snonce : anonce;

	anx_memcpy(data,      mac_lo, 6);
	anx_memcpy(data + 6,  mac_hi, 6);
	anx_memcpy(data + 12, non_lo, 32);
	anx_memcpy(data + 44, non_hi, 32);

	anx_prf_sha1(pmk, 32, label, (uint32_t)anx_strlen(label),
		     data, 76, ptk, 64);
}

/* ------------------------------------------------------------------ */
/* EAPOL frame builder                                                 */
/* ------------------------------------------------------------------ */

/*
 * Build an EAPOL-Key frame into buf[].
 * Returns total frame length.
 * MIC is zeroed; caller must fill it after computing over the frame.
 */
static uint32_t build_eapol_key(uint8_t *buf, uint32_t buf_max,
				const uint8_t dst[6], const uint8_t src[6],
				uint16_t key_info,
				const uint8_t replay_ctr[8],
				const uint8_t nonce[32],
				const uint8_t *key_data, uint16_t key_data_len)
{
	uint16_t eapol_plen = (uint16_t)(1 + 2 + 2 + 8 + 32 + 16 + 8 + 8 + 16 + 2 + key_data_len);
	uint32_t total = ETH_HLEN + 4 + eapol_plen;

	if (total > buf_max) return 0;

	anx_memset(buf, 0, total);

	/* Ethernet */
	anx_memcpy(buf + ETH_DST, dst, 6);
	anx_memcpy(buf + ETH_SRC, src, 6);
	buf[12] = 0x88; buf[13] = 0x8E;

	/* EAPOL header */
	buf[EAPOL_VER]  = 0x02;          /* EAPOL v2 */
	buf[EAPOL_TYPE] = 0x03;          /* Key */
	put_be16(buf + EAPOL_PLEN, eapol_plen);

	/* EAPOL-Key body */
	buf[KEY_DESC] = 0x02;             /* RSN key descriptor */
	put_be16(buf + KEY_INFO, key_info);
	/* key_len: 0 for pairwise messages 2/4, 16 for CCM */
	put_be16(buf + KEY_LEN, 0);
	anx_memcpy(buf + KEY_RCTR, replay_ctr, 8);
	if (nonce)
		anx_memcpy(buf + KEY_NONCE, nonce, 32);
	/* IV, RSC, reserved: already zeroed */
	/* MIC: zeroed */
	put_be16(buf + KEY_DLEN, key_data_len);
	if (key_data && key_data_len)
		anx_memcpy(buf + KEY_DATA, key_data, key_data_len);

	return total;
}

/* Compute and fill MIC field: HMAC-SHA1(KCK, frame)[0:16] */
static void fill_mic(uint8_t *frame, uint32_t frame_len, const uint8_t kck[16])
{
	uint8_t mac[20];

	/* MIC field must be zeroed before computing */
	anx_memset(frame + KEY_MIC, 0, 16);
	anx_hmac_sha1(kck, 16, frame + ETH_HLEN, frame_len - ETH_HLEN, mac);
	anx_memcpy(frame + KEY_MIC, mac, 16);
	anx_memset(mac, 0, sizeof(mac));
}

/* Verify MIC in a received EAPOL-Key frame */
static bool verify_mic(const uint8_t *frame, uint32_t frame_len,
		       const uint8_t kck[16])
{
	uint8_t saved_mic[16], mac[20];
	uint8_t *mutable_frame = (uint8_t *)frame; /* const cast for zeroing */

	anx_memcpy(saved_mic, frame + KEY_MIC, 16);
	anx_memset(mutable_frame + KEY_MIC, 0, 16);
	anx_hmac_sha1(kck, 16, frame + ETH_HLEN, frame_len - ETH_HLEN, mac);
	anx_memcpy(mutable_frame + KEY_MIC, saved_mic, 16);

	return (anx_memcmp(saved_mic, mac, 16) == 0);
}

/* ------------------------------------------------------------------ */
/* GTK extraction from Key Data KDE                                    */
/* ------------------------------------------------------------------ */

/*
 * Parse decrypted key data for GTK KDE:
 *   0xdd | len | 00:0f:ac:01 | key_id_flags | reserved | gtk[gtk_len]
 * Returns 0 on success, fills gtk[] and gtk_len.
 */
static int parse_gtk_kde(const uint8_t *kd, uint16_t kd_len,
			 uint8_t *gtk, uint8_t *gtk_len, uint8_t *gtk_idx)
{
	uint16_t pos = 0;

	while (pos + 2 <= kd_len) {
		uint8_t type = kd[pos];
		uint8_t len  = kd[pos + 1];

		if (pos + 2 + len > kd_len) break;

		/* GTK KDE: type=0xdd, OUI+type = 00:0f:ac:01 */
		if (type == 0xdd && len >= 6 &&
		    kd[pos+2] == 0x00 && kd[pos+3] == 0x0f &&
		    kd[pos+4] == 0xac && kd[pos+5] == 0x01) {
			/* byte 6: key_id in bits [1:0] */
			*gtk_idx = kd[pos + 6] & 0x03;
			/* byte 7: reserved; gtk starts at byte 8 */
			uint8_t glen = len - 6;  /* skip OUI+type+keyid+rsv */
			if (glen > 32) glen = 32;
			*gtk_len = glen;
			anx_memcpy(gtk, kd + pos + 8, glen);
			return 0;
		}

		pos += 2 + len;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* MCU key installation (MCU_EXT_CMD_SET_KEY = 0x30)                  */
/* ------------------------------------------------------------------ */

#define MCU_EXT_CMD_SET_KEY  0x30
#define MT_CIPHER_CCMP       4
#define MT_KEY_PAIRWISE      1
#define MT_KEY_GROUP         0

struct mt7925_key_req {
	uint8_t  bss_idx;
	uint8_t  wlan_idx;
	uint8_t  key_idx;   /* 0=PTK, GTK index for group */
	uint8_t  key_type;  /* MT_KEY_PAIRWISE or MT_KEY_GROUP */
	uint8_t  cipher;    /* MT_CIPHER_CCMP */
	uint8_t  mode;      /* 0=add */
	uint8_t  len;       /* 16 for CCMP */
	uint8_t  rsv;
	uint8_t  key[32];
	uint8_t  rx_seq[6];
	uint8_t  tx_seq[6];
	uint8_t  peer_addr[6];
	uint8_t  rsv2[2];
} __attribute__((packed));

/* Install one key via MCU command; returns MCU seq# or -1 */
static int install_key(struct mt7925_dev *dev,
		       uint8_t wlan_idx, uint8_t key_idx, uint8_t key_type,
		       const uint8_t *key, uint8_t key_len,
		       const uint8_t peer_addr[6])
{
	struct mt7925_key_req req;

	anx_memset(&req, 0, sizeof(req));
	req.bss_idx   = 0;
	req.wlan_idx  = wlan_idx;
	req.key_idx   = key_idx;
	req.key_type  = key_type;
	req.cipher    = MT_CIPHER_CCMP;
	req.mode      = 0;
	req.len       = key_len;
	if (key_len > 32) key_len = 32;
	anx_memcpy(req.key, key, key_len);
	if (peer_addr)
		anx_memcpy(req.peer_addr, peer_addr, 6);

	return mt7925_mcu_send_cmd(dev, MCU_EXT_CMD_SET_KEY,
				   &req, sizeof(req));
}

/* ------------------------------------------------------------------ */
/* Main WPA2 connect: called after BSS_INFO_UPDATE is sent            */
/* ------------------------------------------------------------------ */

/*
 * Poll the data RX ring for one EAPOL frame.  Passes non-EAPOL frames
 * to anx_eth_recv() so they aren't lost.  Returns pointer to the static
 * RX buffer on success, NULL if no EAPOL frame available yet.
 */
static const uint8_t *poll_eapol(struct mt7925_dev *dev, uint32_t *out_len)
{
	const uint8_t *frame;
	uint32_t len;

	for (;;) {
		frame = mt7925_data_rx_one(dev, &len);
		if (!frame) return NULL;

		if (len >= ETH_HLEN + 4 &&
		    frame[12] == 0x88 && frame[13] == 0x8E)
			break;

		/* Not EAPOL — pass to network stack */
		anx_eth_recv(frame, len);
	}

	*out_len = len;
	return frame;
}

/*
 * Spin-wait for an EAPOL frame with a timeout expressed as a loop count.
 * On x86 "pause" hints the CPU that this is a spin-wait (~100 ns/iter).
 * On ARM64 "wfe" enters a low-power wait state until an event wakes the core.
 */
static const uint8_t *wait_eapol(struct mt7925_dev *dev, uint32_t *out_len,
				  uint32_t max_iters)
{
	uint32_t i;

	for (i = 0; i < max_iters; i++) {
		const uint8_t *f = poll_eapol(dev, out_len);

		if (f) return f;
#if defined(__x86_64__)
		__asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
		__asm__ volatile("wfe" ::: "memory");
#else
		__asm__ volatile("" ::: "memory");
#endif
	}
	return NULL;
}

int mt7925_wpa_connect(struct mt7925_dev *dev,
		       const char *ssid, const char *psk)
{
	uint8_t pmk[32], ptk[64];
	uint8_t snonce[32];
	uint8_t ap_mac[6];
	uint8_t gtk[32];
	uint8_t gtk_len = 0, gtk_idx = 0;
	const uint8_t *frame;
	uint32_t flen;
	uint16_t key_info;
	uint8_t txbuf[256];
	uint32_t txlen;
	int ret = -1;

	/* ---- 1. Derive PMK ----------------------------------------- */
	kprintf("mt7925: deriving PMK (4096 PBKDF2 rounds)...\n");
	derive_pmk(psk, ssid, pmk);

	/* ---- 2. Generate SNonce ------------------------------------ */
	anx_random_bytes(snonce, 32);

	/* ---- 3. Wait for EAPOL Message 1 from AP (~5s)  ----------- */
	kprintf("mt7925: waiting for EAPOL M1...\n");
	/* 50 million pause iterations ≈ 5 seconds */
	frame = wait_eapol(dev, &flen, 50000000);
	if (!frame) {
		kprintf("mt7925: EAPOL M1 timeout\n");
		goto out;
	}

	/* Validate: type=3 (Key), key_type=pairwise, no MIC, Ack=1 */
	if (flen < (uint32_t)EAPOL_MIN) goto out;
	key_info = be16(frame + KEY_INFO);
	if (!(key_info & KI_ACK) || (key_info & KI_MIC)) {
		kprintf("mt7925: not EAPOL M1 (key_info=0x%04x)\n", key_info);
		goto out;
	}

	anx_memcpy(ap_mac, frame + ETH_SRC, 6);
	kprintf("mt7925: M1 from %02x:%02x:%02x:%02x:%02x:%02x\n",
		ap_mac[0], ap_mac[1], ap_mac[2],
		ap_mac[3], ap_mac[4], ap_mac[5]);

	/* ---- 4. Derive PTK ----------------------------------------- */
	derive_ptk(pmk, ap_mac, dev->mac,
		   frame + KEY_NONCE, snonce, ptk);

	/* ---- 5. Build and send EAPOL Message 2 -------------------- */
	txlen = build_eapol_key(txbuf, sizeof(txbuf),
				ap_mac, dev->mac,
				0x010A, /* Version=2, KeyType=1(0x08), MIC=1(0x100) */
				frame + KEY_RCTR,
				snonce,
				rsnie, (uint16_t)sizeof(rsnie));
	if (!txlen) goto out;
	fill_mic(txbuf, txlen, ptk);       /* KCK = ptk[0:16] */
	mt7925_tx_frame(dev, txbuf, (uint16_t)txlen);
	kprintf("mt7925: sent EAPOL M2\n");

	/* ---- 6. Wait for EAPOL Message 3 (~5s) -------------------- */
	frame = wait_eapol(dev, &flen, 50000000);
	if (!frame) {
		kprintf("mt7925: EAPOL M3 timeout\n");
		goto out;
	}

	key_info = be16(frame + KEY_INFO);
	if (!(key_info & KI_MIC) || !(key_info & KI_ACK) ||
	    !(key_info & KI_INSTALL)) {
		kprintf("mt7925: not EAPOL M3 (key_info=0x%04x)\n", key_info);
		goto out;
	}

	if (!verify_mic(frame, flen, ptk)) {
		kprintf("mt7925: M3 MIC invalid\n");
		goto out;
	}

	/* ---- 7. Decrypt GTK from key data  ------------------------ */
	{
		uint16_t kd_len = be16(frame + KEY_DLEN);
		uint8_t plain[128];

		if (kd_len == 0 || kd_len > 128 ||
		    flen < (uint32_t)(KEY_DATA + kd_len)) {
			kprintf("mt7925: M3 key data length error\n");
			goto out;
		}

		/* Unwrap with KEK = ptk[16:32] */
		if (anx_aes128_unwrap(ptk + 16, frame + KEY_DATA, kd_len,
				      plain, kd_len - 8) != 0) {
			kprintf("mt7925: GTK unwrap failed\n");
			goto out;
		}

		if (parse_gtk_kde(plain, kd_len - 8,
				  gtk, &gtk_len, &gtk_idx) != 0) {
			kprintf("mt7925: GTK KDE not found\n");
			goto out;
		}

		kprintf("mt7925: GTK idx=%u len=%u\n", gtk_idx, gtk_len);
	}

	/* ---- 8. Build and send EAPOL Message 4 -------------------- */
	txlen = build_eapol_key(txbuf, sizeof(txbuf),
				ap_mac, dev->mac,
				0x030A, /* Version=2, KeyType=1(0x08), MIC=1(0x100), Secure=1(0x200) */
				frame + KEY_RCTR,
				NULL, NULL, 0);
	if (!txlen) goto out;
	fill_mic(txbuf, txlen, ptk);
	mt7925_tx_frame(dev, txbuf, (uint16_t)txlen);
	kprintf("mt7925: sent EAPOL M4\n");

	/* ---- 9. Install PTK (TK = ptk[32:48]) -------------------- */
	if (install_key(dev, 0, 0, MT_KEY_PAIRWISE,
			ptk + 32, 16, ap_mac) < 0) {
		kprintf("mt7925: PTK install failed\n");
		goto out;
	}

	/* ---- 10. Install GTK ------------------------------------- */
	if (gtk_len > 0) {
		if (install_key(dev, 0, gtk_idx, MT_KEY_GROUP,
				gtk, gtk_len, ap_mac) < 0) {
			kprintf("mt7925: GTK install failed\n");
			goto out;
		}
	}

	dev->state = MT7925_STATE_CONNECTED;
	kprintf("mt7925: WPA2 connected — keys installed\n");
	ret = 0;

out:
	anx_memset(pmk, 0, sizeof(pmk));
	anx_memset(ptk, 0, sizeof(ptk));
	anx_memset(gtk, 0, sizeof(gtk));
	return ret;
}
