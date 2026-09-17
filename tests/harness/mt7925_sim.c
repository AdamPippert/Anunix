/*
 * mt7925_sim.c — Simulated MT7925 firmware and access point.
 *
 * Stands in for mt7925_fw.c (rings and firmware download) in the host
 * build. See mt7925_sim.h.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/crypto.h>
#include <anx/kprintf.h>
#include <anx/page.h>
#include <anx/pci.h>
#include <anx/mmio.h>
#include "../../kernel/drivers/net/wifi/mt7925_drv.h"
#include "mt7925_sim.h"

struct sim_state g_sim;

#define PKT_MAX		2048
#define QUEUE_LEN	128

struct pkt {
	uint16_t len;
	uint8_t  data[PKT_MAX];
};

struct queue {
	struct pkt items[QUEUE_LEN];
	uint32_t head;
	uint32_t tail;
};

static struct queue g_evt, g_rx;
static struct pkt g_out;
static uint8_t g_cmd_buf[16384];
static struct sim_cmd g_cmds[SIM_MAX_CMDS];
static struct sim_ap g_aps[4];
static uint32_t g_n_aps;
static uint8_t g_seq;
static uintptr_t g_heap_ref;
static uint8_t g_bar[0x200000];

/* The connected AP's authenticator state */
static int g_ap;			/* index into g_aps, -1 for none */
static uint8_t g_pmk[32], g_ptk[48], g_anonce[32];
static uint64_t g_replay;
static bool g_hdr_trans;

static const uint8_t g_sta_mac[6] = { 0x46, 0x3b, 0x72, 0x47, 0xb2, 0x5c };

/* ------------------------------------------------------------------ */
/* A WM image holding one CLC blob with a US rule                       */
/* ------------------------------------------------------------------ */

const uint8_t mt7925_ram_fw[122] = {
	/* CLC: len 46, idx 0 (power), ver 2, one country, encap 1, one seg */
	46, 0, 0, 0, 0, 2, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
	/* segment 1: offset 24, length 6 */
	1, 0, 0, 0, 24, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0,
	/* rule: US, type AB, segment 1 */
	'U', 'S', 'A', 'B', 1, 0, 0, 0,
	/* segment data */
	0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
	/* region: length 46, NON_DL, type CLC */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 46, 0, 0, 0, 0x40, 2, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	/* trailer: one region */
	0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0,
};
const uint32_t mt7925_ram_fw_size = sizeof(mt7925_ram_fw);

/* ------------------------------------------------------------------ */
/* Hardware stubs the driver links against                              */
/* ------------------------------------------------------------------ */

void *anx_mmio_map(uint64_t phys, uint64_t size)
{
	(void)phys;
	(void)size;
	return g_bar;
}

int anx_pci_power_on(struct anx_pci_device *dev)
{
	(void)dev;
	return 0;
}

void *sim_bar(void)
{
	return g_bar;
}

/* ------------------------------------------------------------------ */
/* Little helpers                                                       */
/* ------------------------------------------------------------------ */

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
	return get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static struct pkt *enqueue(struct queue *q)
{
	struct pkt *p;

	if ((q->head + 1) % QUEUE_LEN == q->tail) {
		kprintf("  sim: queue overflow\n");
		return NULL;
	}
	p = &q->items[q->head];
	q->head = (q->head + 1) % QUEUE_LEN;
	anx_memset(p->data, 0, sizeof(p->data));
	return p;
}

static const uint8_t *dequeue(struct queue *q, uint32_t *len)
{
	if (q->head == q->tail)
		return NULL;
	g_out = q->items[q->tail];
	q->tail = (q->tail + 1) % QUEUE_LEN;
	*len = g_out.len;
	return g_out.data;
}

/* The driver passes 32-bit DMA addresses; recover host pointers. */
static uint8_t *from_dma(uint32_t dma)
{
	uintptr_t c = (g_heap_ref & ~(uintptr_t)0xffffffffu) | dma;
	uintptr_t window = 64u << 20;

	if (c + window < g_heap_ref)
		c += (uintptr_t)1 << 32;
	else if (c > g_heap_ref + window)
		c -= (uintptr_t)1 << 32;
	return (uint8_t *)c;
}

/* ------------------------------------------------------------------ */
/* AES-128 encryption and RFC 3394 key wrap (authenticator side)        */
/* ------------------------------------------------------------------ */

static const uint8_t sbox[256] = {
	0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
	0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
	0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
	0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
	0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
	0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
	0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
	0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
	0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
	0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
	0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
	0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
	0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
	0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
	0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
	0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t xt(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static void aes128_encrypt(const uint8_t key[16], uint8_t s[16])
{
	uint8_t rk[176], t[4], tmp[16];
	uint8_t rcon = 1;
	int i, r, c;

	anx_memcpy(rk, key, 16);
	for (i = 16; i < 176; i += 4) {
		anx_memcpy(t, rk + i - 4, 4);
		if (i % 16 == 0) {
			uint8_t u = t[0];

			t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
			t[1] = sbox[t[2]];
			t[2] = sbox[t[3]];
			t[3] = sbox[u];
			rcon = xt(rcon);
		}
		for (c = 0; c < 4; c++)
			rk[i + c] = rk[i - 16 + c] ^ t[c];
	}

	for (i = 0; i < 16; i++)
		s[i] ^= rk[i];
	for (r = 1; r <= 10; r++) {
		for (i = 0; i < 16; i++)
			s[i] = sbox[s[i]];
		/* ShiftRows (column-major state) */
		for (i = 0; i < 16; i++)
			tmp[i] = s[(i + 4 * (i % 4)) % 16];
		anx_memcpy(s, tmp, 16);
		if (r != 10) {
			for (c = 0; c < 4; c++) {
				uint8_t *a = s + 4 * c;
				uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];

				a[0] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
				a[1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
				a[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
				a[3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
			}
		}
		for (i = 0; i < 16; i++)
			s[i] ^= rk[16 * r + i];
	}
}

/* RFC 3394 with the IEEE 802.11 padding; returns the wrapped length. */
static uint32_t key_wrap(const uint8_t kek[16], const uint8_t *in,
			 uint32_t len, uint8_t *out)
{
	uint8_t p[256], a[8], b[16];
	uint32_t n, i, j;

	anx_memcpy(p, in, len);
	if (len % 8 || len < 16) {
		p[len++] = 0xdd;
		while (len % 8 || len < 16)
			p[len++] = 0;
	}
	n = len / 8;
	anx_memset(a, 0xa6, 8);
	anx_memcpy(out + 8, p, len);
	for (j = 0; j <= 5; j++) {
		for (i = 1; i <= n; i++) {
			uint32_t t = n * j + i;

			anx_memcpy(b, a, 8);
			anx_memcpy(b + 8, out + 8 * i, 8);
			aes128_encrypt(kek, b);
			anx_memcpy(a, b, 8);
			a[7] ^= (uint8_t)t;
			a[6] ^= (uint8_t)(t >> 8);
			anx_memcpy(out + 8 * i, b + 8, 8);
		}
	}
	anx_memcpy(out, a, 8);
	return len + 8;
}

/* ------------------------------------------------------------------ */
/* Packet builders                                                      */
/* ------------------------------------------------------------------ */

static void push_event(uint8_t eid, uint8_t seq, uint8_t option,
		       const uint8_t *body, uint32_t len)
{
	struct pkt *p = enqueue(&g_evt);

	if (!p)
		return;
	put32(p->data, (7U << 27) | (44 + len));
	p->data[36] = eid;
	p->data[37] = seq;
	p->data[38] = option;
	anx_memcpy(p->data + 44, body, len);
	p->len = (uint16_t)(44 + len);
}

/* An 802.11 or Ethernet frame on the data ring. */
static void push_frame(const uint8_t *frame, uint32_t len, uint8_t channel,
		       bool unicast, bool hdr_trans, bool decrypted,
		       int8_t rssi)
{
	struct pkt *p = enqueue(&g_rx);
	uint32_t rxd2 = 0;

	if (!p)
		return;
	put32(p->data, (2U << 27) | (48 + len));
	put32(p->data + 4, 1U | (1U << 18));		/* wlan 1, group 3 */
	if (hdr_trans)
		rxd2 |= 1U << 7;
	if (decrypted)
		rxd2 |= 4U << 16;
	put32(p->data + 8, rxd2);
	put32(p->data + 12, ((uint32_t)channel << 8) |
			    ((unicast ? 1U : 2U) << 16));
	p->data[32 + 12] = (uint8_t)(2 * rssi + 220);
	anx_memcpy(p->data + 48, frame, len);
	p->len = (uint16_t)(48 + len);
}

static void push_tx_free(uint16_t token)
{
	struct pkt *p = enqueue(&g_rx);

	if (!p)
		return;
	put32(p->data, (6U << 27) | (1U << 16) | 12);
	put32(p->data + 4, 4U << 16);
	put32(p->data + 8, token | (0x7fffU << 15));
	p->len = 12;
}

static uint32_t hdr80211(uint8_t *f, uint16_t fc, const uint8_t *a1,
			 const uint8_t *a2, const uint8_t *a3)
{
	anx_memset(f, 0, 24);
	put16(f, fc);
	anx_memcpy(f + 4, a1, 6);
	anx_memcpy(f + 10, a2, 6);
	anx_memcpy(f + 16, a3, 6);
	return 24;
}

static void push_beacon(const struct sim_ap *ap)
{
	static const uint8_t rates2[] = {
		1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
		50, 4, 0x30, 0x48, 0x60, 0x6c,
	};
	static const uint8_t rates5[] = {
		1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
	};
	static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	uint8_t f[256];
	uint32_t pos, n = anx_strlen(ap->ssid);
	uint16_t cap = 0x0401;

	if (ap->passphrase)
		cap |= 0x0010;
	pos = hdr80211(f, 0x0080, bcast, ap->bssid, ap->bssid);
	anx_memset(f + pos, 0, 12);
	put16(f + pos + 8, 100);
	put16(f + pos + 10, cap);
	pos += 12;
	f[pos++] = 0;
	f[pos++] = (uint8_t)n;
	anx_memcpy(f + pos, ap->ssid, n);
	pos += n;
	if (ap->band == MT_BAND_2G) {
		anx_memcpy(f + pos, rates2, sizeof(rates2));
		pos += sizeof(rates2);
	} else {
		anx_memcpy(f + pos, rates5, sizeof(rates5));
		pos += sizeof(rates5);
	}
	f[pos++] = 3;
	f[pos++] = 1;
	f[pos++] = ap->channel;
	f[pos++] = 5;
	f[pos++] = 4;
	f[pos++] = 0;
	f[pos++] = 2;			/* DTIM period */
	f[pos++] = 0;
	f[pos++] = 0;
	if (ap->passphrase) {
		static const uint8_t rsn[] = {
			48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0,
			0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2,
			0x00, 0x00,
		};
		anx_memcpy(f + pos, rsn, sizeof(rsn));
		if (ap->pmf_required)
			f[pos + 20] = 0xc0;
		pos += sizeof(rsn);
	}
	push_frame(f, pos, ap->channel, false, false, false, ap->rssi);
}

/* An EAPOL frame from the connected AP, in the form the chip delivers. */
static void push_eapol(const uint8_t *eapol, uint32_t len)
{
	static const uint8_t llc[8] = { 0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e };
	const struct sim_ap *ap = &g_aps[g_ap];
	uint8_t f[600];
	uint32_t pos;

	if (g_hdr_trans) {
		anx_memcpy(f, g_sta_mac, 6);
		anx_memcpy(f + 6, ap->bssid, 6);
		f[12] = 0x88;
		f[13] = 0x8e;
		anx_memcpy(f + 14, eapol, len);
		push_frame(f, 14 + len, ap->channel, true, true, true,
			   ap->rssi);
		return;
	}
	pos = hdr80211(f, 0x0208, g_sta_mac, ap->bssid, ap->bssid);
	anx_memcpy(f + pos, llc, 8);
	anx_memcpy(f + pos + 8, eapol, len);
	push_frame(f, pos + 8 + len, ap->channel, true, false, false,
		   ap->rssi);
}

/* ------------------------------------------------------------------ */
/* Authenticator                                                        */
/* ------------------------------------------------------------------ */

#define E_INFO		5
#define E_KEY_LEN	7
#define E_REPLAY	9
#define E_NONCE		17
#define E_MIC		81
#define E_KD_LEN	97
#define E_KD		99

static uint32_t eapol_key(uint8_t *out, uint16_t info, const uint8_t *nonce,
			  const uint8_t *kd, uint16_t kd_len)
{
	uint32_t len = E_KD + kd_len;
	uint32_t i;

	anx_memset(out, 0, len);
	out[0] = 2;
	out[1] = 3;
	put_be16(out + 2, (uint16_t)(len - 4));
	out[4] = 2;
	put_be16(out + E_INFO, info);
	put_be16(out + E_KEY_LEN, 16);
	g_replay++;
	for (i = 0; i < 8; i++)
		out[E_REPLAY + i] = (uint8_t)(g_replay >> (56 - 8 * i));
	if (nonce)
		anx_memcpy(out + E_NONCE, nonce, 32);
	put_be16(out + E_KD_LEN, kd_len);
	anx_memcpy(out + E_KD, kd, kd_len);
	return len;
}

static void set_mic(uint8_t *frame, uint32_t len)
{
	uint8_t mic[20];

	anx_memset(frame + E_MIC, 0, 16);
	anx_hmac_sha1(g_ptk, 16, frame, len, mic);
	anx_memcpy(frame + E_MIC, mic, 16);
}

static bool mic_ok(const uint8_t *frame, uint32_t len)
{
	uint8_t copy[600], mic[20];

	if (len > sizeof(copy))
		return false;
	anx_memcpy(copy, frame, len);
	anx_memset(copy + E_MIC, 0, 16);
	anx_hmac_sha1(g_ptk, 16, copy, len, mic);
	return anx_memcmp(mic, frame + E_MIC, 16) == 0;
}

static uint32_t gtk_kde(uint8_t *out, uint8_t idx)
{
	uint32_t i;

	out[0] = 0xdd;
	out[1] = 22;
	out[2] = 0x00;
	out[3] = 0x0f;
	out[4] = 0xac;
	out[5] = 1;
	out[6] = idx;
	out[7] = 0;
	for (i = 0; i < 16; i++)
		g_sim.gtk[i] = (uint8_t)(0x40 + 17 * i + idx);
	g_sim.gtk_idx = idx;
	anx_memcpy(out + 8, g_sim.gtk, 16);
	return 24;
}

static void send_m1(void)
{
	uint8_t f[256];
	uint32_t i, len;

	for (i = 0; i < 32; i++)
		g_anonce[i] = (uint8_t)(0xa0 + i);
	len = eapol_key(f, 0x008a, g_anonce, NULL, 0);
	push_eapol(f, len);
}

static void derive(const uint8_t *snonce)
{
	static const char label[] = "Pairwise key expansion";
	const uint8_t *aa = g_aps[g_ap].bssid, *spa = g_sta_mac;
	bool aa_low = anx_memcmp(aa, spa, 6) < 0;
	bool an_low = anx_memcmp(g_anonce, snonce, 32) < 0;
	uint8_t data[76];

	anx_memcpy(data, aa_low ? aa : spa, 6);
	anx_memcpy(data + 6, aa_low ? spa : aa, 6);
	anx_memcpy(data + 12, an_low ? g_anonce : snonce, 32);
	anx_memcpy(data + 44, an_low ? snonce : g_anonce, 32);
	anx_prf_sha1(g_pmk, 32, label, sizeof(label) - 1, data, 76, g_ptk, 48);
}

static void eapol_from_sta(const uint8_t *e, uint32_t len)
{
	uint16_t info;

	if (len < E_KD || g_ap < 0)
		return;
	info = (uint16_t)((e[E_INFO] << 8) | e[E_INFO + 1]);

	if (info == 0x010a) {				/* message 2 */
		static const uint8_t rsn[] = {
			48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0,
			0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2,
			0x00, 0x00,
		};
		uint8_t kd[64], wrapped[80], f[256];
		uint32_t n, flen;

		derive(e + E_NONCE);
		g_sim.m2_mic_ok = mic_ok(e, len);
		if (!g_sim.m2_mic_ok || g_aps[g_ap].reject_m2) {
			sim_inject_deauth(15);
			return;
		}
		anx_memcpy(kd, rsn, sizeof(rsn));
		n = sizeof(rsn) + gtk_kde(kd + sizeof(rsn), 1);
		n = key_wrap(g_ptk + 16, kd, n, wrapped);
		flen = eapol_key(f, 0x13ca, g_anonce, wrapped, (uint16_t)n);
		set_mic(f, flen);
		push_eapol(f, flen);
	} else if (info == 0x030a) {			/* message 4 */
		if (mic_ok(e, len)) {
			g_sim.handshake_done = true;
			anx_memcpy(g_sim.tk, g_ptk + 32, 16);
		}
	} else if (info == 0x0302) {			/* group message 2 */
		g_sim.rekey_done = mic_ok(e, len);
	}
}

/* ------------------------------------------------------------------ */
/* Frames from the driver                                               */
/* ------------------------------------------------------------------ */

static int ap_by_bssid(const uint8_t *bssid)
{
	uint32_t i;

	for (i = 0; i < g_n_aps; i++)
		if (anx_memcmp(g_aps[i].bssid, bssid, 6) == 0)
			return (int)i;
	return -1;
}

static void mgmt_from_sta(const uint8_t *f, uint32_t len)
{
	uint16_t fc = get16(f);
	int ap = ap_by_bssid(f + 16);
	uint8_t r[64];
	uint32_t pos;

	g_sim.tx_mgmt++;
	if (ap < 0)
		return;

	switch (fc & 0xfc) {
	case 0xb0:					/* authentication */
		g_sim.auth_seen++;
		if (g_aps[ap].ignore_auth)
			return;
		pos = hdr80211(r, 0x00b0, g_sta_mac, g_aps[ap].bssid,
			       g_aps[ap].bssid);
		put16(r + pos, 0);
		put16(r + pos + 2, 2);
		put16(r + pos + 4, 0);
		push_frame(r, pos + 6, g_aps[ap].channel, true, false, false,
			   g_aps[ap].rssi);
		return;
	case 0x00:					/* association */
		g_sim.assoc_seen++;
		if (len <= sizeof(g_sim.assoc_req)) {
			anx_memcpy(g_sim.assoc_req, f, len);
			g_sim.assoc_req_len = len;
		}
		pos = hdr80211(r, 0x0010, g_sta_mac, g_aps[ap].bssid,
			       g_aps[ap].bssid);
		put16(r + pos, 0x0411);
		put16(r + pos + 2, g_aps[ap].assoc_status);
		put16(r + pos + 4, 0xc001);
		push_frame(r, pos + 6, g_aps[ap].channel, true, false, false,
			   g_aps[ap].rssi);
		if (g_aps[ap].assoc_status == 0) {
			g_ap = ap;
			g_replay = 0;
			g_hdr_trans = false;
			if (g_aps[ap].passphrase) {
				const char *pw = g_aps[ap].passphrase;
				const char *ss = g_aps[ap].ssid;

				anx_pbkdf2_hmac_sha1(pw, anx_strlen(pw), ss,
						     anx_strlen(ss), 4096,
						     g_pmk, 32);
				send_m1();
			}
		}
		return;
	case 0xc0:
		g_sim.deauth_seen++;
		g_ap = -1;
		return;
	default:
		return;
	}
}

int mt7925_data_tx_push(uint32_t txwi_phys, uint32_t len)
{
	uint8_t *txwi = from_dma(txwi_phys);
	uint32_t txd1 = get32(txwi + 4), txd3 = get32(txwi + 12);
	uint8_t *frame = from_dma(get32(txwi + 40));
	uint32_t flen = get16(txwi + 44) & 0x0fff;
	uint16_t token = get16(txwi + 32) & 0x7fff;
	uint8_t fmt = (uint8_t)((txd1 >> 14) & 3);

	if (len != 64)
		kprintf("  sim: TXWI length %u\n", len);
	anx_memcpy(g_sim.last_txwi, txwi, 64);

	if (fmt == 0) {					/* 802.3 */
		g_sim.tx_8023++;
		if (txd3 & 2)
			g_sim.tx_protected_8023++;
	} else if (fmt == 2) {
		uint16_t fc = get16(frame);

		if ((fc & 0x0c) == 0x00) {
			mgmt_from_sta(frame, flen);
		} else if ((fc & 0x0c) == 0x08 && flen > 32 &&
			   frame[30] == 0x88 && frame[31] == 0x8e) {
			g_sim.tx_eapol++;
			g_sim.last_eapol_protected = (fc & 0x4000) &&
						     (txd3 & 2);
			eapol_from_sta(frame + 32, flen - 32);
		}
	}

	if (g_sim.free_tokens)
		push_tx_free(token);
	return ANX_OK;
}

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

static void nic_capability(uint8_t seq)
{
	uint8_t b[64];

	anx_memset(b, 0, sizeof(b));
	put16(b, 3);
	put16(b + 4, 7);
	put16(b + 6, 10);
	anx_memcpy(b + 8, g_sta_mac, 6);
	put16(b + 14, 8);
	put16(b + 16, 17);
	b[18 + 4] = 2;					/* nss */
	b[18 + 10] = 3;					/* 2 GHz and 5 GHz */
	put16(b + 31, 0x20);
	put16(b + 33, 12);
	push_event(1, seq, 0, b, 31 + 12);
}

static void scan_request(void)
{
	uint8_t b[16];
	uint32_t i;

	for (i = 0; i < g_n_aps; i++)
		push_beacon(&g_aps[i]);
	anx_memset(b, 0, sizeof(b));
	put16(b + 4, 0);
	put16(b + 6, 8);
	push_event(0x0e, 0, 4, b, 12);
}

static void roc_request(const uint8_t *body)
{
	uint8_t b[24];

	if (get16(body + 4) != 0)			/* abort */
		return;
	anx_memset(b, 0, sizeof(b));
	put16(b + 4, 0);
	put16(b + 6, 20);
	b[8] = body[8];					/* bss_idx */
	b[9] = body[9];					/* token */
	b[11] = body[10];				/* primary channel */
	b[17] = body[19];				/* reqtype */
	b[18] = 0;					/* dbdcband */
	put32(b + 20, get32(body + 20));
	push_event(0x27, 0, 4, b, 24);
}

static void sta_record(const uint8_t *body, uint32_t len)
{
	uint32_t pos = 8;

	while (pos + 4 <= len) {
		uint16_t tag = get16(body + pos), l = get16(body + pos + 2);

		if (l < 4)
			return;
		if (tag == 0x2b && body[1] == 1 && pos + 7 <= len)
			g_hdr_trans = body[pos + 6] == 0;
		pos += l;
	}
}

uint8_t *mt7925_cmd_buf(uint32_t *cap)
{
	if (cap)
		*cap = sizeof(g_cmd_buf);
	return g_cmd_buf;
}

uint8_t mt7925_mcu_next_seq(void)
{
	g_seq = (uint8_t)((g_seq + 1) & 0xf);
	if (!g_seq)
		g_seq = 1;
	return g_seq;
}

int mt7925_wm_send(uint32_t len)
{
	const uint8_t *body = g_cmd_buf + 48;
	uint32_t blen = len - 48;
	uint16_t cid = get16(g_cmd_buf + 34);
	uint8_t seq = g_cmd_buf[39], option = g_cmd_buf[43];
	struct sim_cmd *c;
	uint8_t rsp[80];

	if (get32(g_cmd_buf) != (len | (2U << 23) | (0x20U << 25)) ||
	    get16(g_cmd_buf + 32) != len - 32 || g_cmd_buf[37] != 0xa0)
		kprintf("  sim: malformed descriptor for cid 0x%x\n", cid);

	if (g_sim.n_cmds < SIM_MAX_CMDS) {
		c = &g_cmds[g_sim.n_cmds++];
		c->cid = cid;
		c->option = option;
		c->len = blen;
		anx_memcpy(c->body, body,
			   blen < SIM_CMD_BODY ? blen : SIM_CMD_BODY);
	}

	/* The firmware answers a capability query even without the ack bit. */
	if (cid == MT_UNI_CMD_CHIP_CONFIG && get16(body + 4) == 3) {
		nic_capability(seq);
	} else if (option & 1) {
		anx_memset(rsp, 0, sizeof(rsp));
		if (cid == MT_UNI_CMD_EFUSE_CTRL && !(option & 4)) {
			rsp[MT_EFUSE_READ_EVT_DATA_OFFSET + 1] = 0x01;
			push_event(1, seq, 0, rsp, 64);
		} else {
			rsp[0] = (uint8_t)cid;
			push_event(1, seq, 0, rsp, 8);
		}
	}

	if (cid == MT_UNI_CMD_SCAN_REQ)
		scan_request();
	else if (cid == MT_UNI_CMD_ROC)
		roc_request(body);
	else if (cid == MT_UNI_CMD_STA_REC_UPDATE)
		sta_record(body, blen);
	return ANX_OK;
}

int mt7925_fw_download(struct mt7925_dev *dev)
{
	(void)dev;
	return ANX_OK;
}

const uint8_t *mt7925_evt_poll(uint32_t *out_len)
{
	return dequeue(&g_evt, out_len);
}

const uint8_t *mt7925_data_rx_poll(uint32_t *out_len)
{
	return dequeue(&g_rx, out_len);
}

/* ------------------------------------------------------------------ */
/* Test controls                                                        */
/* ------------------------------------------------------------------ */

void sim_reset(void)
{
	anx_memset(&g_sim, 0, sizeof(g_sim));
	g_sim.free_tokens = true;
	g_evt.head = g_evt.tail = 0;
	g_rx.head = g_rx.tail = 0;
	g_n_aps = 0;
	g_ap = -1;
	g_hdr_trans = false;
	if (!g_heap_ref)
		g_heap_ref = anx_page_alloc(0);
}

void sim_add_ap(const struct sim_ap *ap)
{
	if (g_n_aps < 4)
		g_aps[g_n_aps++] = *ap;
}

const struct sim_cmd *sim_cmd(uint32_t i)
{
	return i < g_sim.n_cmds ? &g_cmds[i] : NULL;
}

int sim_find_cmd(uint16_t cid, uint32_t start)
{
	uint32_t i;

	for (i = start; i < g_sim.n_cmds && i < SIM_MAX_CMDS; i++)
		if (g_cmds[i].cid == cid)
			return (int)i;
	return -1;
}

void sim_inject_eth(const uint8_t *frame, uint32_t len)
{
	const struct sim_ap *ap = g_ap >= 0 ? &g_aps[g_ap] : &g_aps[0];

	push_frame(frame, len, ap->channel, true, true, true, ap->rssi);
}

void sim_inject_deauth(uint16_t reason)
{
	const struct sim_ap *ap;
	uint8_t f[32];
	uint32_t pos;

	if (g_ap < 0)
		return;
	ap = &g_aps[g_ap];
	pos = hdr80211(f, 0x00c0, g_sta_mac, ap->bssid, ap->bssid);
	put16(f + pos, reason);
	push_frame(f, pos + 2, ap->channel, true, false, false, ap->rssi);
	g_ap = -1;
}

void sim_inject_beacon_loss(void)
{
	uint8_t b[12];

	anx_memset(b, 0, sizeof(b));
	put16(b + 4, 0);
	put16(b + 6, 8);
	b[8] = 1;
	push_event(0x0c, 0, 4, b, sizeof(b));
}

void sim_start_rekey(void)
{
	uint8_t kd[32], wrapped[48], f[200];
	uint32_t n, flen;

	if (g_ap < 0)
		return;
	n = gtk_kde(kd, 2);
	n = key_wrap(g_ptk + 16, kd, n, wrapped);
	flen = eapol_key(f, 0x1382, NULL, wrapped, (uint16_t)n);
	set_mic(f, flen);
	push_eapol(f, flen);
}

/* The shell's wifi tool is linked, so nothing else needs stubbing. */
