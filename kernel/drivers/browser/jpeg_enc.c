/*
 * jpeg_enc.c — Minimal baseline JPEG encoder (JFIF, quality ~75).
 *
 * Pipeline per MCU (16×16 block):
 *   RGB→YCbCr → level-shift → 2D integer DCT → quantize →
 *   zig-zag → DC differential + AC RLE → Huffman → bitstream
 *
 * Uses standard JPEG Huffman and quantization tables.
 * Chroma subsampling: 4:2:0.
 */

#include "jpeg_enc.h"
#include <anx/alloc.h>
#include <anx/string.h>

/* ── Quantization tables (standard baseline, quality ≈ 75) ─────── */

static const uint8_t QT_LUMA[64] = {
	16, 11, 10, 16, 24, 40, 51, 61,
	12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56,
	14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68,109,103, 77,
	24, 35, 55, 64, 81,104,113, 92,
	49, 64, 78, 87,103,121,120,101,
	72, 92, 95, 98,112,100,103, 99,
};
static const uint8_t QT_CHROMA[64] = {
	17, 18, 24, 47, 99, 99, 99, 99,
	18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99,
	47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
};

/* ── Zig-zag scan order ─────────────────────────────────────────── */

static const uint8_t ZIGZAG[64] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63,
};

/* ── Standard Huffman tables ────────────────────────────────────── */

static const uint8_t DC_LY_BITS[16]  = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t DC_LY_VALS[]    = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t DC_CX_BITS[16]  = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t DC_CX_VALS[]    = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t AC_LY_BITS[16]  = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125};
static const uint8_t AC_LY_VALS[] = {
	0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,
	0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,0x23,0x42,
	0xB1,0xC1,0x15,0x52,0xD1,0xF0,0x24,0x33,0x62,0x72,0x82,0x09,0x0A,
	0x16,0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,0x29,0x2A,0x34,0x35,
	0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,
	0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,0x67,
	0x68,0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x83,0x84,
	0x85,0x86,0x87,0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,
	0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,
	0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,
	0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE1,
	0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF1,0xF2,0xF3,0xF4,
	0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,
};

static const uint8_t AC_CX_BITS[16]  = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,119};
static const uint8_t AC_CX_VALS[] = {
	0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,
	0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xA1,0xB1,
	0xC1,0x09,0x23,0x33,0x52,0xF0,0x15,0x62,0x72,0xD1,0x0A,0x16,0x24,
	0x34,0xE1,0x25,0xF1,0x17,0x18,0x19,0x1A,0x26,0x27,0x28,0x29,0x2A,
	0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
	0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x63,0x64,0x65,0x66,
	0x67,0x68,0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x82,
	0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,
	0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,
	0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,
	0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,
	0xDA,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF2,0xF3,0xF4,
	0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,
};

/* ── Huffman code table ─────────────────────────────────────────── */

#define HUFF_MAX 256

struct htab {
	uint16_t code[HUFF_MAX];
	uint8_t  bits[HUFF_MAX];  /* code lengths */
	uint8_t  sym[HUFF_MAX];
	uint32_t n;
};

static void htab_build(struct htab *t, const uint8_t *lens,
		        const uint8_t *syms, uint32_t nsyms)
{
	uint16_t c = 0;
	uint32_t si = 0, li;

	t->n = nsyms;
	for (li = 1; li <= 16 && si < nsyms; li++) {
		uint32_t k;
		for (k = 0; k < (uint32_t)lens[li - 1] && si < nsyms; k++, si++) {
			t->sym[si]  = syms[si];
			t->code[si] = c++;
			t->bits[si] = (uint8_t)li;
		}
		c <<= 1;
	}
}

static void htab_lookup(const struct htab *t, uint8_t sym,
			  uint16_t *code, uint8_t *nbits)
{
	uint32_t i;
	for (i = 0; i < t->n; i++) {
		if (t->sym[i] == sym) {
			*code  = t->code[i];
			*nbits = t->bits[i];
			return;
		}
	}
	*code = 0; *nbits = 0;
}

/* ── Bit-stream writer ───────────────────────────────────────────── */

struct bs {
	uint8_t *buf;
	size_t   cap;
	size_t   pos;
	uint32_t acc;
	uint32_t nbits;
};

static void bs_put(struct bs *b, uint32_t val, uint32_t n)
{
	b->acc    = (b->acc << n) | (val & ((1u << n) - 1u));
	b->nbits += n;
	while (b->nbits >= 8) {
		b->nbits -= 8;
		if (b->pos < b->cap) {
			uint8_t byte = (uint8_t)(b->acc >> b->nbits);
			b->buf[b->pos++] = byte;
			if (byte == 0xFF && b->pos < b->cap)
				b->buf[b->pos++] = 0x00;
		}
	}
}

static void bs_done(struct bs *b)
{
	if (b->nbits > 0) {
		uint8_t byte = (uint8_t)(b->acc << (8 - b->nbits));
		if (b->pos < b->cap)
			b->buf[b->pos++] = byte;
	}
}

/* ── Simple output buffer ────────────────────────────────────────── */

struct jout {
	uint8_t *buf;
	size_t   cap;
	size_t   pos;
};

static void jo_u8(struct jout *j, uint8_t v)
{
	if (j->pos < j->cap) j->buf[j->pos++] = v;
}
static void jo_u16(struct jout *j, uint16_t v)
{
	jo_u8(j, (uint8_t)(v >> 8));
	jo_u8(j, (uint8_t)(v));
}
static void jo_raw(struct jout *j, const uint8_t *src, size_t n)
{
	if (j->pos + n <= j->cap) {
		anx_memcpy(j->buf + j->pos, src, n);
		j->pos += n;
	}
}
static void jo_dht(struct jout *j, uint8_t tc_th,
		    const uint8_t *lens, const uint8_t *syms,
		    uint32_t nsyms __attribute__((unused)))
{
	uint32_t k, total = 0;
	for (k = 0; k < 16; k++) total += lens[k];
	jo_u8(j, 0xFF); jo_u8(j, 0xC4);
	jo_u16(j, (uint16_t)(2 + 1 + 16 + total));
	jo_u8(j, tc_th);
	jo_raw(j, lens, 16);
	jo_raw(j, syms, total);
}

static void jo_headers(struct jout *j, uint32_t w, uint32_t h)
{
	/* SOI */
	jo_u8(j, 0xFF); jo_u8(j, 0xD8);
	/* APP0 JFIF */
	jo_u8(j, 0xFF); jo_u8(j, 0xE0);
	jo_u16(j, 16);
	jo_raw(j, (const uint8_t *)"JFIF\0", 5);
	jo_u16(j, 0x0101); jo_u8(j, 0);
	jo_u16(j, 1); jo_u16(j, 1);
	jo_u8(j, 0); jo_u8(j, 0);
	/* DQT luma */
	jo_u8(j, 0xFF); jo_u8(j, 0xDB);
	jo_u16(j, 67); jo_u8(j, 0x00);
	jo_raw(j, QT_LUMA, 64);
	/* DQT chroma */
	jo_u8(j, 0xFF); jo_u8(j, 0xDB);
	jo_u16(j, 67); jo_u8(j, 0x01);
	jo_raw(j, QT_CHROMA, 64);
	/* SOF0: 3-component 4:2:0 */
	jo_u8(j, 0xFF); jo_u8(j, 0xC0);
	jo_u16(j, 17); jo_u8(j, 8);
	jo_u16(j, (uint16_t)h); jo_u16(j, (uint16_t)w);
	jo_u8(j, 3);
	jo_u8(j, 1); jo_u8(j, 0x22); jo_u8(j, 0);  /* Y  2h2v QT0 */
	jo_u8(j, 2); jo_u8(j, 0x11); jo_u8(j, 1);  /* Cb 1h1v QT1 */
	jo_u8(j, 3); jo_u8(j, 0x11); jo_u8(j, 1);  /* Cr 1h1v QT1 */
	/* DHTs */
	jo_dht(j, 0x00, DC_LY_BITS, DC_LY_VALS, sizeof(DC_LY_VALS));
	jo_dht(j, 0x10, AC_LY_BITS, AC_LY_VALS, sizeof(AC_LY_VALS));
	jo_dht(j, 0x01, DC_CX_BITS, DC_CX_VALS, sizeof(DC_CX_VALS));
	jo_dht(j, 0x11, AC_CX_BITS, AC_CX_VALS, sizeof(AC_CX_VALS));
	/* SOS */
	jo_u8(j, 0xFF); jo_u8(j, 0xDA);
	jo_u16(j, 12); jo_u8(j, 3);
	jo_u8(j, 1); jo_u8(j, 0x00);
	jo_u8(j, 2); jo_u8(j, 0x11);
	jo_u8(j, 3); jo_u8(j, 0x11);
	jo_u8(j, 0); jo_u8(j, 63); jo_u8(j, 0);
}

/* ── Integer 2D DCT ─────────────────────────────────────────────── */

/* Fixed-point cosine table: cos_tab[u][x] ≈ 4096 * cos((2x+1)*u*pi/16)
 * Row u=0 is pre-scaled by 1/sqrt(2). */
static const int16_t COS_TAB[8][8] = {
	{ 2896, 2896, 2896, 2896, 2896, 2896, 2896, 2896 },
	{ 4017, 3406, 2275,  799, -799,-2275,-3406,-4017 },
	{ 3784, 1567,-1567,-3784,-3784,-1567, 1567, 3784 },
	{ 3406, -799,-4017,-2275, 2275, 4017,  799,-3406 },
	{ 2896,-2896,-2896, 2896, 2896,-2896,-2896, 2896 },
	{ 2275,-4017,  799, 3406,-3406, -799, 4017,-2275 },
	{ 1567,-3784, 3784,-1567,-1567, 3784,-3784, 1567 },
	{  799,-2275, 3406,-4017, 4017,-3406, 2275, -799 },
};

static void dct8(const int16_t *in, int16_t *out)
{
	int32_t tmp[64];
	int u, v, x, y;

	for (y = 0; y < 8; y++)
		for (u = 0; u < 8; u++) {
			int32_t s = 0;
			for (x = 0; x < 8; x++)
				s += (int32_t)COS_TAB[u][x] * in[y * 8 + x];
			tmp[y * 8 + u] = s >> 12;
		}
	for (v = 0; v < 8; v++)
		for (u = 0; u < 8; u++) {
			int32_t s = 0;
			for (y = 0; y < 8; y++)
				s += (int32_t)COS_TAB[v][y] * tmp[y * 8 + u];
			out[v * 8 + u] = (int16_t)(s >> 12);
		}
}

/* ── Encode one 8×8 block ──────────────────────────────────────── */

static uint8_t vli_cat(int16_t v) {
	int16_t a = (v < 0) ? -v : v;
	uint8_t c = 0;
	while (a) { a >>= 1; c++; }
	return c;
}

static uint16_t vli_bits(int16_t v) {
	return (uint16_t)((v < 0) ? v - 1 : v);
}

static void encode_block(struct bs *b,
			   const int16_t *coeff, const uint8_t *qt,
			   const struct htab *dc_t, const struct htab *ac_t,
			   int16_t *prev_dc)
{
	int16_t zz[64];
	int     i, k, run;

	/* Quantize in zig-zag order */
	for (i = 0; i < 64; i++) {
		int16_t c = coeff[ZIGZAG[i]];
		int16_t q = (int16_t)qt[i];
		zz[i] = (c < 0) ? -(int16_t)((-c + q/2) / q)
			         :  (int16_t)(( c + q/2) / q);
	}

	/* DC: differential */
	{
		int16_t  diff = zz[0] - *prev_dc;
		uint8_t  cat  = vli_cat(diff);
		uint16_t hc; uint8_t hs;
		*prev_dc = zz[0];
		htab_lookup(dc_t, cat, &hc, &hs);
		bs_put(b, hc, hs);
		if (cat) bs_put(b, vli_bits(diff), cat);
	}

	/* AC: RLE + Huffman */
	run = 0;
	for (k = 1; k < 64; k++) {
		if (zz[k] == 0) {
			if (k == 63) {
				uint16_t ec; uint8_t es;
				htab_lookup(ac_t, 0x00, &ec, &es);
				bs_put(b, ec, es);
			} else if (++run == 16) {
				uint16_t zc; uint8_t zs;
				htab_lookup(ac_t, 0xF0, &zc, &zs);
				bs_put(b, zc, zs);
				run = 0;
			}
		} else {
			uint8_t  cat = vli_cat(zz[k]);
			uint8_t  sym = (uint8_t)((run << 4) | cat);
			uint16_t hc; uint8_t hs;
			htab_lookup(ac_t, sym, &hc, &hs);
			bs_put(b, hc, hs);
			bs_put(b, vli_bits(zz[k]), cat);
			run = 0;
		}
	}
}

/* ── Public encoder ──────────────────────────────────────────────── */

size_t anx_jpeg_encode(const uint32_t *xrgb_pixels,
		        uint32_t width, uint32_t height, uint32_t stride,
		        uint8_t *out_buf, size_t out_cap)
{
	struct jout  jo = { out_buf, out_cap, 0 };
	struct htab  ht_dc_y, ht_ac_y, ht_dc_c, ht_ac_c;
	struct bs    b;
	int16_t      prev_y = 0, prev_cb = 0, prev_cr = 0;
	uint32_t     mcus_x = (width  + 15) / 16;
	uint32_t     mcus_y = (height + 15) / 16;
	uint32_t     mx, my, bx, by;
	uint32_t     stride32 = stride / 4;

	if (out_cap < 2048 || !xrgb_pixels)
		return 0;

	htab_build(&ht_dc_y, DC_LY_BITS, DC_LY_VALS, sizeof(DC_LY_VALS));
	htab_build(&ht_ac_y, AC_LY_BITS, AC_LY_VALS, sizeof(AC_LY_VALS));
	htab_build(&ht_dc_c, DC_CX_BITS, DC_CX_VALS, sizeof(DC_CX_VALS));
	htab_build(&ht_ac_c, AC_CX_BITS, AC_CX_VALS, sizeof(AC_CX_VALS));

	jo_headers(&jo, width, height);

	b.buf = out_buf + jo.pos;
	b.cap = out_cap - jo.pos;
	b.pos = 0; b.acc = 0; b.nbits = 0;

	for (my = 0; my < mcus_y; my++) {
		for (mx = 0; mx < mcus_x; mx++) {
			int16_t Y0[64], Y1[64], Y2[64], Y3[64];
			int16_t Cb[64], Cr[64];
			int16_t dY0[64], dY1[64], dY2[64], dY3[64];
			int16_t dCb[64], dCr[64];

			anx_memset(Y0,0,128); anx_memset(Y1,0,128);
			anx_memset(Y2,0,128); anx_memset(Y3,0,128);
			anx_memset(Cb,0,128); anx_memset(Cr,0,128);

			for (by = 0; by < 16; by++) {
				for (bx = 0; bx < 16; bx++) {
					uint32_t px = mx*16 + bx;
					uint32_t py = my*16 + by;
					if (px >= width)  px = width  - 1;
					if (py >= height) py = height - 1;

					uint32_t pix = xrgb_pixels[py * stride32 + px];
					int32_t R = (pix >> 16) & 0xFF;
					int32_t G = (pix >>  8) & 0xFF;
					int32_t Bv = pix & 0xFF;

					int32_t Yv  = (( 66*R + 129*G +  25*Bv + 128) >> 8) + 16  - 128;
					int32_t Cbv = ((-38*R -  74*G + 112*Bv + 128) >> 8) + 128 - 128;
					int32_t Crv = ((112*R -  94*G -  18*Bv + 128) >> 8) + 128 - 128;

					uint32_t ci = (by & 7)*8 + (bx & 7);
					if      (by < 8 && bx < 8)  Y0[ci] = (int16_t)Yv;
					else if (by < 8)             Y1[ci] = (int16_t)Yv;
					else if (bx < 8)             Y2[ci] = (int16_t)Yv;
					else                         Y3[ci] = (int16_t)Yv;

					/* 4:2:0: sample Cb/Cr at even positions */
					if (!(bx & 1) && !(by & 1)) {
						uint32_t csi = (by/2)*8 + (bx/2);
						Cb[csi] = (int16_t)Cbv;
						Cr[csi] = (int16_t)Crv;
					}
				}
			}

			dct8(Y0,dY0); dct8(Y1,dY1); dct8(Y2,dY2); dct8(Y3,dY3);
			dct8(Cb,dCb); dct8(Cr,dCr);

			encode_block(&b, dY0, QT_LUMA,   &ht_dc_y, &ht_ac_y, &prev_y);
			encode_block(&b, dY1, QT_LUMA,   &ht_dc_y, &ht_ac_y, &prev_y);
			encode_block(&b, dY2, QT_LUMA,   &ht_dc_y, &ht_ac_y, &prev_y);
			encode_block(&b, dY3, QT_LUMA,   &ht_dc_y, &ht_ac_y, &prev_y);
			encode_block(&b, dCb, QT_CHROMA, &ht_dc_c, &ht_ac_c, &prev_cb);
			encode_block(&b, dCr, QT_CHROMA, &ht_dc_c, &ht_ac_c, &prev_cr);
		}
	}

	bs_done(&b);

	size_t total = jo.pos + b.pos;
	if (total + 2 <= out_cap) {
		out_buf[total++] = 0xFF;
		out_buf[total++] = 0xD9;  /* EOI */
	}
	return total;
}
