/*
 * png.c — PNG decoder.
 *
 * Supports:
 *   Color types 0 (grayscale), 2 (RGB), 3 (indexed), 4 (gray+alpha), 6 (RGBA)
 *   Bit depth 8 and 16 (16-bit: upper byte used)
 *   Filters: None, Sub, Up, Average, Paeth
 *   DEFLATE block types: stored (00), fixed Huffman (01), dynamic Huffman (10)
 *   Non-interlaced images only
 *
 * No libc, no float — integer-only arithmetic throughout.
 * Heap via anx_alloc/anx_free; string ops via <anx/string.h>.
 */

#include "png.h"
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* ── byte helpers ──────────────────────────────────────────────── */

static inline uint32_t ru32be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static inline int32_t abs32(int32_t v)
{
	return (v < 0) ? -v : v;
}

/* ── PNG signature ─────────────────────────────────────────────── */

static const uint8_t png_sig[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};

/* ── Huffman tree ──────────────────────────────────────────────── */

/*
 * Canonical Huffman tree stored as a sorted (len, symbol) array.
 * For codes up to MAX_BITS bits, we also build a 9-bit LUT for O(1)
 * decode of short codes, falling back to linear scan for longer ones.
 *
 * LUT entry: bits [8:0] = symbol (0x1FF = invalid), bits [15:9] = code length.
 */

#define HT_LUT_BITS	9
#define HT_LUT_SIZE	(1 << HT_LUT_BITS)
#define HT_MAX_CODES	320		/* litlen: 288 + dist: 32 */
#define HT_INVALID	0x1FFu

struct huff_entry {
	uint16_t code;
	uint8_t  len;
	uint16_t sym;
};

struct htree {
	struct huff_entry entries[HT_MAX_CODES];
	int               count;
	/* LUT: indexed by next HT_LUT_BITS bits (MSB-first) */
	uint16_t          lut[HT_LUT_SIZE];	/* sym | (len << 9) — packed */
};

/* Build canonical codes from array of code lengths, one per symbol. */
static int htree_build(struct htree *ht,
		const uint8_t *lens, int nsym)
{
	/* Count codes per length */
	uint16_t bl_count[16];
	anx_memset(bl_count, 0, sizeof(bl_count));
	int max_bits = 0;
	for (int i = 0; i < nsym; i++) {
		if (lens[i] > 15) return -1;
		if (lens[i] > 0) bl_count[lens[i]]++;
		if (lens[i] > max_bits) max_bits = lens[i];
	}

	/* Assign smallest codes */
	uint16_t next_code[16];
	anx_memset(next_code, 0, sizeof(next_code));
	uint16_t code = 0;
	for (int bits = 1; bits <= max_bits; bits++) {
		code = (uint16_t)((code + bl_count[bits - 1]) << 1);
		next_code[bits] = code;
	}

	/* Fill entries */
	ht->count = 0;
	for (int sym = 0; sym < nsym; sym++) {
		int l = lens[sym];
		if (l == 0) continue;
		if (ht->count >= HT_MAX_CODES) return -1;
		ht->entries[ht->count].code = next_code[l];
		ht->entries[ht->count].len  = (uint8_t)l;
		ht->entries[ht->count].sym  = (uint16_t)sym;
		ht->count++;
		next_code[l]++;
	}

	/* Build 9-bit LUT */
	for (int i = 0; i < HT_LUT_SIZE; i++)
		ht->lut[i] = (uint16_t)(HT_INVALID | (15u << 9));

	for (int i = 0; i < ht->count; i++) {
		int l = ht->entries[i].len;
		if (l > HT_LUT_BITS) continue;
		uint16_t base = (uint16_t)(ht->entries[i].code << (HT_LUT_BITS - l));
		int fill = 1 << (HT_LUT_BITS - l);
		uint16_t packed = (uint16_t)(ht->entries[i].sym |
			((uint16_t)l << 9));
		for (int j = 0; j < fill; j++)
			ht->lut[base + j] = packed;
	}

	return 0;
}

/* ── LSB-first bit reader (DEFLATE uses LSB-first) ─────────────── */

struct br {
	const uint8_t *data;
	uint32_t       pos;
	uint32_t       len;
	uint32_t       cache;	/* LSB-aligned */
	int            cbits;
	int            err;
};

static void br_init(struct br *b, const uint8_t *data, uint32_t len)
{
	b->data  = data;
	b->pos   = 0;
	b->len   = len;
	b->cache = 0;
	b->cbits = 0;
	b->err   = 0;
}

static void br_refill(struct br *b)
{
	while (b->cbits <= 24 && b->pos < b->len) {
		b->cache |= (uint32_t)b->data[b->pos++] << b->cbits;
		b->cbits += 8;
	}
}

static uint32_t br_bits(struct br *b, int n)
{
	if (b->cbits < n) {
		br_refill(b);
		if (b->cbits < n) { b->err = 1; return 0; }
	}
	uint32_t v = b->cache & (((uint32_t)1 << n) - 1);
	b->cache >>= n;
	b->cbits  -= n;
	return v;
}

static void br_skip(struct br *b, int n)
{
	b->cache >>= n;
	b->cbits  -= n;
}

/* Align to next byte boundary (discard partial byte). */
static void br_byte_align(struct br *b)
{
	int rem = b->cbits & 7;
	if (rem) {
		b->cache >>= rem;
		b->cbits  -= rem;
	}
}

/* Read a Huffman code — try LUT first, fall back to linear scan. */
static int huff_decode(struct br *b, const struct htree *ht)
{
	/* We need at least HT_LUT_BITS bits; refill if needed */
	if (b->cbits < HT_LUT_BITS) br_refill(b);

	if (b->cbits >= HT_LUT_BITS) {
		uint32_t idx = b->cache & (HT_LUT_SIZE - 1);
		uint16_t e   = ht->lut[idx];
		int sym = e & HT_INVALID;
		int len = e >> 9;
		if (sym != (int)HT_INVALID && len <= b->cbits) {
			br_skip(b, len);
			return sym;
		}
	}

	/* Linear scan for longer codes: rebuild from scratch */
	uint32_t code = 0;
	int cur_len   = 0;

	for (int i = 0; i < ht->count; i++) {
		int need = ht->entries[i].len;
		while (cur_len < need) {
			if (b->cbits == 0) br_refill(b);
			if (b->cbits == 0) { b->err = 1; return -1; }
			code = (code << 1) | (b->cache & 1);
			b->cache >>= 1;
			b->cbits--;
			cur_len++;
		}
		if (code == ht->entries[i].code)
			return ht->entries[i].sym;
	}
	b->err = 1;
	return -1;
}

/* ── DEFLATE tables ────────────────────────────────────────────── */

static const uint8_t len_extra[29] = {
	0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t len_base[29] = {
	3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
	35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t dist_extra[30] = {
	0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,
	10,10,11,11,12,12,13,13
};
static const uint16_t dist_base[30] = {
	1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
	257,385,513,769,1025,1537,2049,3073,4097,6145,
	8193,12289,16385,24577
};
static const uint8_t clperm[19] = {
	16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

/* ── Output buffer (growable) ──────────────────────────────────── */

struct outbuf {
	uint8_t  *data;
	uint32_t  len;
	uint32_t  cap;
};

static int outbuf_push(struct outbuf *ob, uint8_t byte)
{
	if (ob->len >= ob->cap) {
		uint32_t newcap = ob->cap ? ob->cap * 2 : 4096;
		uint8_t *nb = (uint8_t *)anx_alloc(newcap);
		if (!nb) return -1;
		if (ob->data) {
			anx_memcpy(nb, ob->data, ob->len);
			anx_free(ob->data);
		}
		ob->data = nb;
		ob->cap  = newcap;
	}
	ob->data[ob->len++] = byte;
	return 0;
}

static int outbuf_copy(struct outbuf *ob, uint32_t dist, uint32_t length)
{
	if (dist == 0 || dist > ob->len) return -1;
	for (uint32_t i = 0; i < length; i++) {
		uint8_t byte = ob->data[ob->len - dist];
		if (outbuf_push(ob, byte) < 0) return -1;
	}
	return 0;
}

/* ── Fixed Huffman tree (pre-built at first use) ───────────────── */

static struct htree g_fixed_litlen;
static struct htree g_fixed_dist;
static int          g_fixed_built;

static void build_fixed_trees(void)
{
	if (g_fixed_built) return;

	uint8_t lens[288];
	int i;
	for (i =   0; i <= 143; i++) lens[i] = 8;
	for (i = 144; i <= 255; i++) lens[i] = 9;
	for (i = 256; i <= 279; i++) lens[i] = 7;
	for (i = 280; i <= 287; i++) lens[i] = 8;
	htree_build(&g_fixed_litlen, lens, 288);

	uint8_t dlens[32];
	for (i = 0; i < 32; i++) dlens[i] = 5;
	htree_build(&g_fixed_dist, dlens, 32);

	g_fixed_built = 1;
}

/* ── Dynamic Huffman tree decode ───────────────────────────────── */

static int decode_dynamic_trees(struct br *b,
				struct htree *litlen,
				struct htree *dist)
{
	int hlit  = (int)br_bits(b, 5) + 257;
	int hdist = (int)br_bits(b, 5) + 1;
	int hclen = (int)br_bits(b, 4) + 4;
	if (b->err) return -1;
	if (hlit > 286 || hdist > 32) return -1;

	/* Code-length alphabet */
	uint8_t cl_lens[19];
	anx_memset(cl_lens, 0, sizeof(cl_lens));
	for (int i = 0; i < hclen; i++)
		cl_lens[clperm[i]] = (uint8_t)br_bits(b, 3);
	if (b->err) return -1;

	struct htree cl_tree;
	if (htree_build(&cl_tree, cl_lens, 19) < 0) return -1;

	/* Decode code lengths for litlen + dist alphabets */
	int total = hlit + hdist;
	uint8_t all_lens[320];
	anx_memset(all_lens, 0, sizeof(all_lens));

	int i = 0;
	while (i < total) {
		int sym = huff_decode(b, &cl_tree);
		if (b->err || sym < 0) return -1;
		if (sym < 16) {
			all_lens[i++] = (uint8_t)sym;
		} else if (sym == 16) {
			if (i == 0) return -1;
			int rep = (int)br_bits(b, 2) + 3;
			uint8_t prev = all_lens[i - 1];
			while (rep-- > 0 && i < total)
				all_lens[i++] = prev;
		} else if (sym == 17) {
			int rep = (int)br_bits(b, 3) + 3;
			while (rep-- > 0 && i < total)
				all_lens[i++] = 0;
		} else if (sym == 18) {
			int rep = (int)br_bits(b, 7) + 11;
			while (rep-- > 0 && i < total)
				all_lens[i++] = 0;
		} else {
			return -1;
		}
		if (b->err) return -1;
	}

	if (htree_build(litlen, all_lens, hlit) < 0) return -1;
	if (htree_build(dist, all_lens + hlit, hdist) < 0) return -1;
	return 0;
}

/* ── DEFLATE block decoder ─────────────────────────────────────── */

static int deflate_block(struct br *b,
			 const struct htree *litlen,
			 const struct htree *dist,
			 struct outbuf *ob)
{
	for (;;) {
		int sym = huff_decode(b, litlen);
		if (b->err || sym < 0) return -1;

		if (sym < 256) {
			if (outbuf_push(ob, (uint8_t)sym) < 0) return -1;
			continue;
		}
		if (sym == 256) break;	/* end of block */

		/* Length/distance back-reference */
		int lidx = sym - 257;
		if (lidx < 0 || lidx >= 29) return -1;
		uint32_t length = len_base[lidx] +
			br_bits(b, len_extra[lidx]);
		if (b->err) return -1;

		int dsym = huff_decode(b, dist);
		if (b->err || dsym < 0 || dsym >= 30) return -1;
		uint32_t distance = dist_base[dsym] +
			br_bits(b, dist_extra[dsym]);
		if (b->err) return -1;

		if (outbuf_copy(ob, distance, length) < 0) return -1;
	}
	return 0;
}

/* ── DEFLATE (RFC 1951) decompressor ───────────────────────────── */

static int deflate_decompress(const uint8_t *src, uint32_t src_len,
			      struct outbuf *ob)
{
	struct br b;
	br_init(&b, src, src_len);

	build_fixed_trees();

	for (;;) {
		int bfinal = (int)br_bits(&b, 1);
		int btype  = (int)br_bits(&b, 2);
		if (b.err) return -1;

		if (btype == 0) {
			/* Stored block */
			br_byte_align(&b);
			if (b.pos + 4 > b.len) return -1;
			uint16_t nlen = (uint16_t)(b.data[b.pos] |
					((uint16_t)b.data[b.pos+1] << 8));
			uint16_t nclen = (uint16_t)(b.data[b.pos+2] |
					((uint16_t)b.data[b.pos+3] << 8));
			b.pos += 4;
			if ((uint16_t)(nlen ^ nclen) != 0xFFFF) return -1;
			if (b.pos + nlen > b.len) return -1;
			for (uint16_t i = 0; i < nlen; i++) {
				if (outbuf_push(ob, b.data[b.pos++]) < 0)
					return -1;
			}

		} else if (btype == 1) {
			/* Fixed Huffman */
			if (deflate_block(&b, &g_fixed_litlen,
					  &g_fixed_dist, ob) < 0)
				return -1;

		} else if (btype == 2) {
			/* Dynamic Huffman */
			struct htree litlen, dist;
			if (decode_dynamic_trees(&b, &litlen, &dist) < 0)
				return -1;
			if (deflate_block(&b, &litlen, &dist, ob) < 0)
				return -1;

		} else {
			return -1;	/* reserved block type */
		}

		if (bfinal) break;
	}
	return 0;
}

/* ── zlib wrapper (PNG uses zlib-wrapped DEFLATE) ──────────────── */

static int zlib_decompress(const uint8_t *src, uint32_t src_len,
			   struct outbuf *ob)
{
	if (src_len < 2) return -1;
	uint8_t cmf = src[0];
	uint8_t flg = src[1];
	(void)flg;

	/* CM must be 8 (DEFLATE), CINFO must be <= 7 */
	if ((cmf & 0x0F) != 8) return -1;
	if (((cmf & 0xF0) >> 4) > 7) return -1;

	/* Skip 2-byte header; skip 4-byte adler32 at end (best-effort) */
	return deflate_decompress(src + 2, src_len - 2, ob);
}

/* ── PNG filter reconstruction ─────────────────────────────────── */

static inline uint8_t paeth(int32_t a, int32_t b, int32_t c)
{
	int32_t p  = a + b - c;
	int32_t pa = abs32(p - a);
	int32_t pb = abs32(p - b);
	int32_t pc = abs32(p - c);
	if (pa <= pb && pa <= pc) return (uint8_t)a;
	if (pb <= pc) return (uint8_t)b;
	return (uint8_t)c;
}

/*
 * Reconstruct one row in-place.
 * row:    points to the filter byte followed by stride bytes of sample data
 * prev:   previous reconstructed row (NULL for first row — treated as all-zero)
 * bpp:    bytes per pixel
 * stride: bytes per row (not counting filter byte)
 */
static int png_filter_row(uint8_t *row, const uint8_t *prev,
			  int bpp, uint32_t stride)
{
	uint8_t ftype = row[0];
	uint8_t *p = row + 1;

	switch (ftype) {
	case 0:	/* None */
		break;
	case 1:	/* Sub */
		for (uint32_t i = (uint32_t)bpp; i < stride; i++)
			p[i] += p[i - (uint32_t)bpp];
		break;
	case 2:	/* Up */
		if (prev) {
			for (uint32_t i = 0; i < stride; i++)
				p[i] += prev[i];
		}
		break;
	case 3:	/* Average */
		for (uint32_t i = 0; i < stride; i++) {
			uint8_t left = (i >= (uint32_t)bpp) ? p[i - (uint32_t)bpp] : 0;
			uint8_t up   = prev ? prev[i] : 0;
			p[i] += (uint8_t)(((uint32_t)left + up) >> 1);
		}
		break;
	case 4:	/* Paeth */
		for (uint32_t i = 0; i < stride; i++) {
			int32_t a = (i >= (uint32_t)bpp) ? p[i - (uint32_t)bpp] : 0;
			int32_t b = prev ? (int32_t)prev[i] : 0;
			int32_t c = (prev && i >= (uint32_t)bpp)
				? (int32_t)prev[i - (uint32_t)bpp] : 0;
			p[i] += paeth(a, b, c);
		}
		break;
	default:
		return -1;
	}
	return 0;
}

/* ── IHDR color type constants ─────────────────────────────────── */

#define CT_GRAY		0
#define CT_RGB		2
#define CT_INDEXED	3
#define CT_GRAY_ALPHA	4
#define CT_RGBA		6

/* ── Main PNG decoder ──────────────────────────────────────────── */

int png_decode(const void *data, uint32_t data_len, struct png_image *img)
{
	const uint8_t *src = (const uint8_t *)data;

	img->pixels = NULL;
	img->width  = 0;
	img->height = 0;

	/* Signature check */
	if (data_len < 8) return -1;
	if (anx_memcmp(src, png_sig, 8) != 0) return -1;

	uint32_t pos = 8;

	/* IHDR fields */
	uint32_t width = 0, height = 0;
	uint8_t  bit_depth = 0, color_type = 0, interlace = 0;

	/* Palette */
	uint32_t palette[256];
	int      pal_len = 0;

	/* tRNS alpha for indexed mode — stored but alpha channel discarded in XRGB output */
	uint8_t  trns[256];
	int      trns_len = 0;
	anx_memset(trns, 255, sizeof(trns));
	(void)trns;

	/* Concatenated IDAT compressed data */
	uint8_t  *idat_buf = NULL;
	uint32_t  idat_len = 0;
	uint32_t  idat_cap = 0;

	int got_ihdr = 0, got_iend = 0;

	while (pos + 8 <= data_len && !got_iend) {
		uint32_t chunk_len  = ru32be(src + pos);
		uint32_t chunk_type = ru32be(src + pos + 4);
		pos += 8;

		if (pos + chunk_len + 4 > data_len) goto err_idat;

		const uint8_t *cdata = src + pos;

		/* IHDR */
		if (chunk_type == 0x49484452u) {	/* 'IHDR' */
			if (chunk_len < 13 || got_ihdr) goto err_idat;
			width      = ru32be(cdata);
			height     = ru32be(cdata + 4);
			bit_depth  = cdata[8];
			color_type = cdata[9];
			interlace  = cdata[12];
			if (interlace != 0) goto err_idat;	/* no interlace */
			if (bit_depth != 8 && bit_depth != 16) goto err_idat;
			if (color_type != CT_GRAY   && color_type != CT_RGB &&
			    color_type != CT_INDEXED && color_type != CT_GRAY_ALPHA &&
			    color_type != CT_RGBA)
				goto err_idat;
			if (color_type == CT_INDEXED && bit_depth == 16) goto err_idat;
			got_ihdr = 1;

		/* PLTE */
		} else if (chunk_type == 0x504C5445u) {	/* 'PLTE' */
			if (!got_ihdr || chunk_len % 3 != 0) goto err_idat;
			pal_len = (int)(chunk_len / 3);
			if (pal_len > 256) goto err_idat;
			for (int i = 0; i < pal_len; i++) {
				uint8_t r = cdata[i*3 + 0];
				uint8_t g = cdata[i*3 + 1];
				uint8_t b = cdata[i*3 + 2];
				palette[i] = (uint32_t)((r << 16) | (g << 8) | b);
			}

		/* tRNS */
		} else if (chunk_type == 0x74524E53u) {	/* 'tRNS' */
			if (color_type == CT_INDEXED) {
				trns_len = (int)chunk_len;
				if (trns_len > 256) goto err_idat;
				anx_memcpy(trns, cdata, (uint32_t)trns_len);
			}
			/* For other color types, ignore (simple transparency not needed) */

		/* IDAT */
		} else if (chunk_type == 0x49444154u) {	/* 'IDAT' */
			if (!got_ihdr) goto err_idat;
			/* Append to idat_buf */
			if (idat_len + chunk_len > idat_cap) {
				uint32_t newcap = idat_cap ? idat_cap * 2 : 65536;
				while (newcap < idat_len + chunk_len)
					newcap *= 2;
				uint8_t *nb = (uint8_t *)anx_alloc(newcap);
				if (!nb) goto err_idat;
				if (idat_buf) {
					anx_memcpy(nb, idat_buf, idat_len);
					anx_free(idat_buf);
				}
				idat_buf = nb;
				idat_cap = newcap;
			}
			anx_memcpy(idat_buf + idat_len, cdata, chunk_len);
			idat_len += chunk_len;

		/* IEND */
		} else if (chunk_type == 0x49454E44u) {	/* 'IEND' */
			got_iend = 1;
		}
		/* Unknown chunks: skip */

		pos += chunk_len + 4;	/* +4 for CRC */
	}

	if (!got_ihdr || idat_len == 0) goto err_idat;
	if (width == 0 || height == 0) goto err_idat;
	if (color_type == CT_INDEXED && pal_len == 0) goto err_idat;

	/* Decompress IDAT */
	struct outbuf raw;
	raw.data = NULL;
	raw.len  = 0;
	raw.cap  = 0;

	if (zlib_decompress(idat_buf, idat_len, &raw) < 0) {
		if (raw.data) anx_free(raw.data);
		goto err_idat;
	}
	anx_free(idat_buf);
	idat_buf = NULL;

	/* Bytes-per-pixel and bytes-per-sample */
	int samples_per_pixel;
	switch (color_type) {
	case CT_GRAY:       samples_per_pixel = 1; break;
	case CT_RGB:        samples_per_pixel = 3; break;
	case CT_INDEXED:    samples_per_pixel = 1; break;
	case CT_GRAY_ALPHA: samples_per_pixel = 2; break;
	case CT_RGBA:       samples_per_pixel = 4; break;
	default:            goto err_raw;
	}

	int bytes_per_sample = (bit_depth == 16) ? 2 : 1;
	int bpp = samples_per_pixel * bytes_per_sample;
	uint32_t stride = width * (uint32_t)bpp;
	uint32_t row_bytes = stride + 1;	/* +1 for filter byte */

	if (raw.len < row_bytes * height) goto err_raw;

	/* Allocate output */
	uint32_t npix   = width * height;
	uint32_t *pixels = (uint32_t *)anx_alloc(npix * 4);
	if (!pixels) goto err_raw;
	anx_memset(pixels, 0, npix * 4);

	/* Filter and convert each row */
	const uint8_t *prev_row = NULL;

	for (uint32_t y = 0; y < height; y++) {
		uint8_t *row = raw.data + y * row_bytes;
		const uint8_t *prev = prev_row;

		if (png_filter_row(row, prev, bpp, stride) < 0) {
			anx_free(pixels);
			goto err_raw;
		}

		const uint8_t *p = row + 1;	/* skip filter byte */

		for (uint32_t x = 0; x < width; x++) {
			uint32_t pix = 0;

			switch (color_type) {
			case CT_GRAY: {
				uint8_t luma = (bit_depth == 16) ? p[x*2] : p[x];
				pix = (uint32_t)((luma << 16) | (luma << 8) | luma);
				break;
			}
			case CT_RGB: {
				uint32_t ri, gi, bi;
				if (bit_depth == 16) {
					ri = p[x*6];
					gi = p[x*6+2];
					bi = p[x*6+4];
				} else {
					ri = p[x*3];
					gi = p[x*3+1];
					bi = p[x*3+2];
				}
				pix = (ri << 16) | (gi << 8) | bi;
				break;
			}
			case CT_INDEXED: {
				uint8_t idx = p[x];
				if (idx >= (uint8_t)pal_len) {
					anx_free(pixels);
					goto err_raw;
				}
				pix = palette[idx];
				break;
			}
			case CT_GRAY_ALPHA: {
				uint8_t luma = (bit_depth == 16) ? p[x*4] : p[x*2];
				pix = (uint32_t)((luma << 16) | (luma << 8) | luma);
				break;
			}
			case CT_RGBA: {
				uint32_t ri, gi, bi;
				if (bit_depth == 16) {
					ri = p[x*8];
					gi = p[x*8+2];
					bi = p[x*8+4];
				} else {
					ri = p[x*4];
					gi = p[x*4+1];
					bi = p[x*4+2];
				}
				pix = (ri << 16) | (gi << 8) | bi;
				break;
			}
			}

			pixels[y * width + x] = pix;
		}

		prev_row = row + 1;	/* point at reconstructed samples, not filter byte */
	}

	anx_free(raw.data);

	img->width  = width;
	img->height = height;
	img->pixels = pixels;
	return 0;

err_raw:
	anx_free(raw.data);
	return -1;

err_idat:
	if (idat_buf) anx_free(idat_buf);
	return -1;
}

void png_free(struct png_image *img)
{
	if (img && img->pixels) {
		anx_free(img->pixels);
		img->pixels = NULL;
	}
}
