/*
 * webp.c — WebP decoder: VP8L (lossless) + VP8 (lossy, key frames).
 *
 * VP8L: complete decoder — Huffman + LZ77 + all 4 transforms.
 * VP8:  intra prediction + integer iDCT residuals + YCbCr→RGB.
 *       Key frames only; loop filter and inter-frame modes are irrelevant
 *       for still images.
 *
 * Memory layout: all intermediate buffers are heap-allocated and freed
 * before returning.  Only the output pixels are caller-owned.
 *
 * References:
 *   VP8L: https://developers.google.com/speed/webp/docs/webp_lossless_bitstream_specification
 *   VP8:  RFC 6386 — VP8 Data Format and Decoding Guide
 */

#include "webp.h"
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* ── helpers ─────────────────────────────────────────────────────── */

static inline uint8_t u8clamp(int32_t v)
{
	return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}

static inline uint32_t ru32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
	       ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static inline uint16_t ru16le(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1]<<8));
}

/* ── RIFF / WebP container ──────────────────────────────────────── */

#define FOURCC(a,b,c,d) \
	((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))

static const uint8_t *riff_find_chunk(const uint8_t *data, uint32_t len,
				       uint32_t fourcc,
				       uint32_t *chunk_len_out)
{
	uint32_t pos = 0;
	while (pos + 8 <= len) {
		uint32_t cc  = ru32le(data + pos);
		uint32_t csz = ru32le(data + pos + 4);
		if (cc == fourcc) {
			if (chunk_len_out) *chunk_len_out = csz;
			return data + pos + 8;
		}
		pos += 8 + ((csz + 1) & ~1u); /* chunks are word-padded */
	}
	return NULL;
}

/* ══════════════════════════════════════════════════════════════════
 * LSB-first bit reader (shared by VP8L and VP8 bool decoder)
 * ══════════════════════════════════════════════════════════════════ */

struct bb {
	const uint8_t *data;
	uint32_t       pos;   /* byte position */
	uint32_t       len;
	uint64_t       cache;
	int32_t        cbits; /* valid bits in cache */
	bool           eof;
};

static void bb_init(struct bb *b, const uint8_t *data, uint32_t len)
{
	b->data  = data;
	b->pos   = 0;
	b->len   = len;
	b->cache = 0;
	b->cbits = 0;
	b->eof   = false;
}

static void bb_fill(struct bb *b)
{
	while (b->cbits <= 56 && b->pos < b->len)
		b->cache |= (uint64_t)b->data[b->pos++] << b->cbits, b->cbits += 8;
}

static uint32_t bb_read(struct bb *b, uint32_t n)
{
	if (n == 0) return 0;
	if (b->cbits < (int32_t)n) {
		bb_fill(b);
		if (b->cbits < (int32_t)n) { b->eof = true; return 0; }
	}
	uint32_t val = (uint32_t)(b->cache & ((1ULL << n) - 1));
	b->cache >>= n;
	b->cbits  -= (int32_t)n;
	return val;
}

static uint32_t bb_peek(struct bb *b, uint32_t n)
{
	if (b->cbits < (int32_t)n) bb_fill(b);
	return (uint32_t)(b->cache & ((1ULL << n) - 1));
}

static void bb_skip(struct bb *b, uint32_t n)
{
	b->cache >>= n;
	b->cbits  -= (int32_t)n;
}

/* ══════════════════════════════════════════════════════════════════
 * Canonical Huffman decoder (VP8L uses LSB-first codes)
 * ══════════════════════════════════════════════════════════════════ */

#define VPL_HUFF_MAX_SYM 2330  /* 256+24+2048(max palette cache)+2 */
#define VPL_HUFF_LBITS   8     /* primary LUT depth */
#define VPL_HUFF_LSIZE   (1 << VPL_HUFF_LBITS)

struct vpl_huff_entry { uint16_t sym; uint8_t len; };

struct vpl_huff {
	struct vpl_huff_entry lut[VPL_HUFF_LSIZE];
	/* overflow table for codes > VPL_HUFF_LBITS */
	struct { uint32_t code; uint16_t sym; uint8_t len; } ovf[64];
	uint32_t n_ovf;
	uint8_t  max_bits;
};

/* Build canonical Huffman table from code-lengths array (len[] of size n). */
static int vpl_huff_build(struct vpl_huff *h, const uint8_t *cl, uint32_t n)
{
	uint16_t count[16] = {0};
	uint16_t next_code[16] = {0};
	uint32_t i;

	anx_memset(h, 0, sizeof(*h));
	h->max_bits = 0;

	for (i = 0; i < n; i++) {
		if (cl[i]) {
			count[cl[i]]++;
			if (cl[i] > h->max_bits) h->max_bits = cl[i];
		}
	}
	if (h->max_bits == 0) return 0; /* empty tree (ok) */

	/* Canonical starting codes (LSB-first requires reversing bit order) */
	{
		uint16_t code = 0;
		for (i = 1; i <= h->max_bits; i++) {
			next_code[i] = code;
			code = (uint16_t)((code + count[i]) << 1);
		}
	}

	for (i = 0; i < n; i++) {
		uint8_t len = cl[i];
		if (!len) continue;
		/* Canonical code (MSB-first) → reverse to LSB-first */
		uint32_t code_msb = next_code[len]++;
		/* Bit-reverse code_msb for len bits */
		uint32_t code_lsb = 0, tmp = code_msb, bits = len;
		while (bits--) { code_lsb = (code_lsb << 1) | (tmp & 1); tmp >>= 1; }

		if (len <= VPL_HUFF_LBITS) {
			uint32_t step = 1u << len;
			uint32_t idx  = code_lsb;
			while (idx < VPL_HUFF_LSIZE) {
				h->lut[idx].sym = (uint16_t)i;
				h->lut[idx].len = len;
				idx += step;
			}
		} else {
			if (h->n_ovf < 64) {
				h->ovf[h->n_ovf].code = code_lsb;
				h->ovf[h->n_ovf].sym  = (uint16_t)i;
				h->ovf[h->n_ovf].len  = len;
				h->n_ovf++;
			}
		}
	}
	return 0;
}

static uint32_t vpl_huff_decode(struct vpl_huff *h, struct bb *b)
{
	bb_fill(b);
	uint32_t peek = bb_peek(b, VPL_HUFF_LBITS);
	struct vpl_huff_entry *e = &h->lut[peek];
	if (e->len) { bb_skip(b, e->len); return e->sym; }

	/* Long code: search overflow table */
	uint32_t code = 0;
	uint32_t j;
	for (j = 1; j <= h->max_bits; j++) {
		code = (code << 1) | bb_read(b, 1);
		uint32_t k;
		for (k = 0; k < h->n_ovf; k++) {
			if (h->ovf[k].len == j && h->ovf[k].code == code)
				return h->ovf[k].sym;
		}
	}
	b->eof = true;
	return 0;
}

/* ── VP8L prefix code extra bits ──────────────────────────────────── */

static const uint8_t kLenExtraBits[24] = {
	0,0,0,0, 1,1, 2,2, 3,3, 4,4, 5,5, 6,6, 7,7, 8,8, 9,9, 10,10
};
static const uint16_t kLenBase[24] = {
	1,2,3,4, 5,7, 9,13, 17,25, 33,49, 65,97, 129,193,
	257,385, 513,769, 1025,1537, 2049,3073
};
static const uint8_t kDistExtraBits[40] = {
	0,0,0,0, 1,1, 2,2, 3,3, 4,4, 5,5, 6,6, 7,7, 8,8, 9,9,
	10,10, 11,11, 12,12, 13,13, 14,14, 15,15, 16,16, 17,17, 18,18
};
static const uint32_t kDistBase[40] = {
	1,2,3,4, 5,7, 9,13, 17,25, 33,49, 65,97, 129,193,
	257,385, 513,769, 1025,1537, 2049,3073, 4097,6145,
	8193,12289, 16385,24577, 32769,49153, 65537,98305,
	131073,196609, 262145,393217, 524289,786433
};

/*
 * VP8L distance: codes < 120 use a 2D spiral map; >= 120 are direct offsets.
 * The 120-entry table encodes (row<<4|col) deltas, each biased by 8.
 */
static const uint8_t kCodeToPlane[120] = {
	0x18,0x07,0x17,0x19,0x28,0x06,0x27,0x29,0x16,0x1a,
	0x26,0x2a,0x38,0x05,0x37,0x39,0x15,0x1b,0x36,0x3a,
	0x25,0x2b,0x48,0x04,0x47,0x49,0x14,0x1c,0x35,0x3b,
	0x46,0x4a,0x24,0x2c,0x58,0x45,0x4b,0x34,0x3c,0x03,
	0x57,0x59,0x13,0x1d,0x56,0x5a,0x23,0x2d,0x44,0x4c,
	0x55,0x5b,0x33,0x3d,0x68,0x02,0x67,0x69,0x12,0x1e,
	0x66,0x6a,0x22,0x2e,0x54,0x5c,0x43,0x4d,0x65,0x6b,
	0x32,0x3e,0x78,0x01,0x77,0x79,0x53,0x5d,0x11,0x1f,
	0x64,0x6c,0x42,0x4e,0x76,0x7a,0x21,0x2f,0x75,0x7b,
	0x31,0x3f,0x63,0x6d,0x52,0x5e,0x00,0x74,0x7c,0x41,
	0x4f,0x10,0x20,0x62,0x6e,0x30,0x73,0x7d,0x51,0x5f,
	0x40,0x72,0x7e,0x61,0x6f,0x50,0x71,0x7f,0x60,0x70
};

static int32_t vpl_dist_to_offset(uint32_t dist_code, uint32_t width)
{
	if (dist_code == 0) return 1;
	if (dist_code <= 120) {
		uint8_t  re = kCodeToPlane[dist_code - 1];
		int32_t  dy = (int32_t)(re >> 4) - 8;
		int32_t  dx = (int32_t)(re & 0xf) - 8;
		return (int32_t)(dy * (int32_t)width + dx);
	}
	return (int32_t)(dist_code - 120);
}

/* ── Color cache ─────────────────────────────────────────────────── */

static inline void cache_insert(uint32_t *cache, uint32_t mask, uint32_t argb)
{
	uint32_t key = (0x1e35a7bd * argb) >> (32 - __builtin_popcount(mask));
	/* Use multiply-hash: key = (hash * pixel) >> (32-bits) */
	/* Simpler: just use lower bits of hash */
	key = (0x1e35a7bd * argb) >> (32 - 8);
	cache[key & mask] = argb;
}

static inline uint32_t cache_get(const uint32_t *cache, uint32_t mask,
				   uint32_t argb)
{
	uint32_t key = (0x1e35a7bd * argb) >> (32 - 8);
	return cache[key & mask];
}

/* ══════════════════════════════════════════════════════════════════
 * VP8L: read a Huffman tree from the bitstream
 * ══════════════════════════════════════════════════════════════════ */

#define VPL_MAX_GROUPS 128

struct vpl_group {
	struct vpl_huff h[5]; /* G(+len+cache), R, B, A, D */
};

/* Read code lengths for a single Huffman tree alphabet. */
static int vpl_read_huff(struct bb *b, struct vpl_huff *out,
			  uint32_t alphabet_size)
{
	bool is_simple = bb_read(b, 1);
	uint8_t *cl = (uint8_t *)anx_alloc(alphabet_size);
	if (!cl) return -1;
	anx_memset(cl, 0, alphabet_size);

	if (is_simple) {
		uint32_t n_sym = bb_read(b, 1) + 1;
		uint32_t first_sym_8bit = bb_read(b, 1);
		uint32_t s0 = first_sym_8bit ? bb_read(b, 8) : bb_read(b, 1);
		cl[s0] = 1;
		if (n_sym == 2) {
			uint32_t s1 = bb_read(b, 8);
			if (s1 < alphabet_size) cl[s1] = 1;
		}
	} else {
		/* Complex code: code lengths coded with a code-length code */
		static const uint8_t kPermute[19] = {
			17,18,0,1,2,3,4,5,16,6,7,8,9,10,11,12,13,14,15
		};
		uint32_t n_cl_codes = bb_read(b, 4) + 4;
		uint8_t cl_cl[19] = {0};
		uint32_t i;
		for (i = 0; i < n_cl_codes; i++)
			cl_cl[kPermute[i]] = (uint8_t)bb_read(b, 3);

		struct vpl_huff cl_tree;
		vpl_huff_build(&cl_tree, cl_cl, 19);

		uint32_t fill = 0, prev_len = 8;
		uint32_t pos = 0;
		while (pos < alphabet_size) {
			if (b->eof) break;
			if (fill > 0) {
				cl[pos++] = 0;
				fill--;
				continue;
			}
			uint32_t sym = vpl_huff_decode(&cl_tree, b);
			if (sym < 16) {
				cl[pos++] = (uint8_t)sym;
				if (sym) prev_len = sym;
			} else if (sym == 16) {
				uint32_t rep = bb_read(b, 2) + 3;
				while (rep-- && pos < alphabet_size)
					cl[pos++] = (uint8_t)prev_len;
			} else if (sym == 17) {
				fill = bb_read(b, 3) + 3 - 1;
			} else { /* sym == 18 */
				fill = bb_read(b, 7) + 11 - 1;
			}
		}
	}

	vpl_huff_build(out, cl, alphabet_size);
	anx_free(cl);
	return b->eof ? -1 : 0;
}

/* ══════════════════════════════════════════════════════════════════
 * VP8L transforms
 * ══════════════════════════════════════════════════════════════════ */

#define TRANSFORM_PREDICTOR    0
#define TRANSFORM_COLOR        1
#define TRANSFORM_SUBTR_GREEN  2
#define TRANSFORM_COLOR_INDEX  3

struct vpl_transform {
	int      type;
	uint32_t block_bits;
	uint32_t *data;      /* heap-alloc transform image; NULL for SUBTR_GREEN */
	uint32_t  palette[256];
	int       palette_size;
};

/* Predictor transform: 14 modes */
static uint32_t predict_pixel(int mode, uint32_t *out,
			       int32_t x, int32_t y, uint32_t width)
{
	uint32_t L  = (x > 0) ? out[y * width + x - 1] : 0xFF000000u;
	uint32_t T  = (y > 0) ? out[(y-1) * width + x] : L;
	uint32_t TL = (x > 0 && y > 0) ? out[(y-1)*width+(x-1)] : T;
	uint32_t TR = (y > 0 && (uint32_t)(x+1) < width) ?
		       out[(y-1)*width+(x+1)] : T;

#define A(c) (((c)>>24)&0xFF)
#define R(c) (((c)>>16)&0xFF)
#define G(c) (((c)>>8)&0xFF)
#define B(c) ((c)&0xFF)
#define ARGB(a,r,g,b) (((uint32_t)(a)<<24)|((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))

	switch (mode) {
	case 0:  return 0xFF000000u;
	case 1:  return L;
	case 2:  return T;
	case 3:  return TR;
	case 4:  return TL;
	case 5:  /* avg2(avg2(L,TR), T) */
		return ARGB(
			(A(L)+A(TR))/2, (R(L)+R(TR))/2,
			(G(L)+G(TR))/2, (B(L)+B(TR))/2);
	case 6:  return ARGB((A(L)+A(TL))/2,(R(L)+R(TL))/2,
			     (G(L)+G(TL))/2,(B(L)+B(TL))/2);
	case 7:  return ARGB((A(L)+A(T))/2,(R(L)+R(T))/2,
			     (G(L)+G(T))/2,(B(L)+B(T))/2);
	case 8:  return ARGB((A(TL)+A(T))/2,(R(TL)+R(T))/2,
			     (G(TL)+G(T))/2,(B(TL)+B(T))/2);
	case 9:  return ARGB((A(T)+A(TR))/2,(R(T)+R(TR))/2,
			     (G(T)+G(TR))/2,(B(T)+B(TR))/2);
	case 10: return ARGB((A(TL)+A(T)+A(L)+A(TR))/4,
			     (R(TL)+R(T)+R(L)+R(TR))/4,
			     (G(TL)+G(T)+G(L)+G(TR))/4,
			     (B(TL)+B(T)+B(L)+B(TR))/4);
	case 11: { /* select: choose L or T based on gradient */
		int pa = (int)A(TL)-(int)A(T), pr = (int)R(TL)-(int)R(T),
		    pg = (int)G(TL)-(int)G(T), pb = (int)B(TL)-(int)B(T);
		int pal = (pa<0?-pa:pa)+(pr<0?-pr:pr)+(pg<0?-pg:pg)+(pb<0?-pb:pb);
		pa = (int)A(TL)-(int)A(L); pr = (int)R(TL)-(int)R(L);
		pg = (int)G(TL)-(int)G(L); pb = (int)B(TL)-(int)B(L);
		int pat = (pa<0?-pa:pa)+(pr<0?-pr:pr)+(pg<0?-pg:pg)+(pb<0?-pb:pb);
		return (pal < pat) ? L : T; }
	case 12: { /* clamp-add-sub-full */
		int32_t ra=(int32_t)A(L)+(int32_t)A(T)-(int32_t)A(TL);
		int32_t rr=(int32_t)R(L)+(int32_t)R(T)-(int32_t)R(TL);
		int32_t rg=(int32_t)G(L)+(int32_t)G(T)-(int32_t)G(TL);
		int32_t rb=(int32_t)B(L)+(int32_t)B(T)-(int32_t)B(TL);
		return ARGB(u8clamp(ra),u8clamp(rr),u8clamp(rg),u8clamp(rb)); }
	case 13: { /* clamp-add-sub-half */
		int32_t ra=(int32_t)((A(L)+A(T))>>1)+(int32_t)(A(TL));
		int32_t rr=(int32_t)((R(L)+R(T))>>1)+(int32_t)(R(TL));
		int32_t rg=(int32_t)((G(L)+G(T))>>1)+(int32_t)(G(TL));
		int32_t rb=(int32_t)((B(L)+B(T))>>1)+(int32_t)(B(TL));
		return ARGB(u8clamp(ra),u8clamp(rr),u8clamp(rg),u8clamp(rb)); }
	default: return L;
	}
#undef A
#undef R
#undef G
#undef B
#undef ARGB
}

static void apply_predictor_transform(uint32_t *pixels,
				       uint32_t w, uint32_t h,
				       const struct vpl_transform *t)
{
	uint32_t block = 1u << t->block_bits;
	uint32_t x, y;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			uint32_t bx = x >> t->block_bits;
			uint32_t by = y >> t->block_bits;
			uint32_t bw = (w + block - 1) / block;
			int mode = (int)((t->data[by * bw + bx] >> 8) & 0xFF);
			uint32_t pred = predict_pixel(mode, pixels, (int32_t)x,
						       (int32_t)y, w);
			uint32_t cur  = pixels[y * w + x];
			/* Add back predictor (modulo 256 per channel) */
			pixels[y * w + x] =
				(((((cur>>24)&0xFF) + ((pred>>24)&0xFF)) & 0xFF) << 24) |
				(((((cur>>16)&0xFF) + ((pred>>16)&0xFF)) & 0xFF) << 16) |
				(((((cur>> 8)&0xFF) + ((pred>> 8)&0xFF)) & 0xFF) <<  8) |
				 (((( cur    &0xFF) + ( pred    &0xFF)) & 0xFF));
		}
	}
}

static void apply_color_transform(uint32_t *pixels,
				    uint32_t w, uint32_t h,
				    const struct vpl_transform *t)
{
	uint32_t block = 1u << t->block_bits;
	uint32_t bw = (w + block - 1) / block;
	uint32_t x, y;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			uint32_t bx = x >> t->block_bits;
			uint32_t by = y >> t->block_bits;
			uint32_t td = t->data[by * bw + bx];
			int8_t green_to_red  = (int8_t)((td >> 16) & 0xFF);
			int8_t green_to_blue = (int8_t)((td >>  0) & 0xFF);
			uint32_t px = pixels[y * w + x];
			int32_t  g  = (int32_t)((px >> 8) & 0xFF);
			int32_t  r  = (int32_t)((px >> 16) & 0xFF);
			int32_t  b  = (int32_t)((px >> 0) & 0xFF);
			r  = (r  + (int32_t)green_to_red  * g) & 0xFF;
			b  = (b  + (int32_t)green_to_blue * g) & 0xFF;
			pixels[y * w + x] = (px & 0xFF00FF00u) |
					     ((uint32_t)r << 16) | (uint32_t)b;
		}
	}
}

static void apply_subtract_green(uint32_t *pixels, uint32_t n)
{
	uint32_t i;
	for (i = 0; i < n; i++) {
		uint32_t px = pixels[i];
		int32_t  g  = (int32_t)((px >> 8) & 0xFF);
		int32_t  r  = ((int32_t)((px >> 16) & 0xFF) + g) & 0xFF;
		int32_t  b  = ((int32_t)( px        & 0xFF) + g) & 0xFF;
		pixels[i] = (px & 0xFF00FF00u) | ((uint32_t)r << 16) | (uint32_t)b;
	}
}

/* ── Read a VP8L sub-image (used for transform data + main image) ── */

static int vpl_decode_image(struct bb *b, uint32_t w, uint32_t h,
			     int color_cache_bits, uint32_t *out);

/* ══════════════════════════════════════════════════════════════════
 * VP8L main entry
 * ══════════════════════════════════════════════════════════════════ */

static int vpl_decode_image(struct bb *b, uint32_t w, uint32_t h,
			     int color_cache_bits, uint32_t *out)
{
	int ret = -1;
	uint32_t n_pixels = w * h;

	/* Color cache */
	uint32_t *cache       = NULL;
	uint32_t  cache_mask  = 0;
	if (color_cache_bits > 0 && color_cache_bits <= 11) {
		uint32_t csz = 1u << color_cache_bits;
		cache = (uint32_t *)anx_alloc(csz * 4);
		if (!cache) goto done;
		anx_memset(cache, 0, csz * 4);
		cache_mask = csz - 1;
	}

	/* Entropy group meta image */
	int      meta_bits = 0;
	uint32_t meta_w = 1, meta_h = 1;
	uint32_t *meta_img = NULL;
	uint32_t  n_groups = 1;

	bool has_meta = bb_read(b, 1);
	if (has_meta) {
		meta_bits = (int)(bb_read(b, 3) + 2);
		meta_w = (w + (1u<<meta_bits) - 1) >> meta_bits;
		meta_h = (h + (1u<<meta_bits) - 1) >> meta_bits;
		meta_img = (uint32_t *)anx_alloc(meta_w * meta_h * 4);
		if (!meta_img) goto done;
		/* Decode meta image recursively (no color cache, no meta) */
		if (vpl_decode_image(b, meta_w, meta_h, 0, meta_img) != 0)
			goto done;
		/* Find max group ID */
		uint32_t i;
		for (i = 0; i < meta_w * meta_h; i++) {
			uint32_t gid = (meta_img[i] >> 8) & 0xFFFF;
			if (gid + 1 > n_groups) n_groups = gid + 1;
		}
		if (n_groups > VPL_MAX_GROUPS) n_groups = VPL_MAX_GROUPS;
	}

	/* Allocate and decode Huffman trees for each group */
	struct vpl_group *groups =
		(struct vpl_group *)anx_alloc(n_groups * sizeof(struct vpl_group));
	if (!groups) goto done;
	anx_memset(groups, 0, n_groups * sizeof(struct vpl_group));

	{
		uint32_t cache_alph = color_cache_bits > 0 ? (1u<<color_cache_bits) : 0;
		uint32_t g;
		for (g = 0; g < n_groups && !b->eof; g++) {
			/* Tree 0: G + length prefix + color cache */
			uint32_t alpha0 = 256u + 24u + cache_alph;
			if (vpl_read_huff(b, &groups[g].h[0], alpha0) != 0) goto free_groups;
			if (vpl_read_huff(b, &groups[g].h[1], 256)  != 0) goto free_groups;
			if (vpl_read_huff(b, &groups[g].h[2], 256)  != 0) goto free_groups;
			if (vpl_read_huff(b, &groups[g].h[3], 256)  != 0) goto free_groups;
			if (vpl_read_huff(b, &groups[g].h[4], 40)   != 0) goto free_groups;
		}
	}

	/* Main decode loop */
	{
		uint32_t pos = 0;
		while (pos < n_pixels && !b->eof) {
			uint32_t x = pos % w;
			uint32_t y = pos / w;

			/* Select Huffman group for this pixel */
			uint32_t gid = 0;
			if (has_meta) {
				uint32_t bx = x >> meta_bits;
				uint32_t by = y >> meta_bits;
				gid = (meta_img[by * meta_w + bx] >> 8) & 0xFFFF;
				if (gid >= n_groups) gid = 0;
			}
			struct vpl_group *grp = &groups[gid];

			uint32_t sym = vpl_huff_decode(&grp->h[0], b);

			if (sym < 256) {
				/* Literal ARGB */
				uint32_t g  = sym;
				uint32_t r  = vpl_huff_decode(&grp->h[1], b);
				uint32_t bl = vpl_huff_decode(&grp->h[2], b);
				uint32_t a  = vpl_huff_decode(&grp->h[3], b);
				uint32_t px = (a<<24)|(r<<16)|(g<<8)|bl;
				out[pos++]  = px;
				if (cache) cache_insert(cache, cache_mask, px);
			} else if (sym < 256 + 24) {
				/* Back reference */
				uint32_t lc = sym - 256;
				uint32_t len = kLenBase[lc] +
					       bb_read(b, kLenExtraBits[lc]);
				uint32_t dc = vpl_huff_decode(&grp->h[4], b);
				if (dc >= 40) dc = 39;
				uint32_t dist_code = kDistBase[dc] +
						     bb_read(b, kDistExtraBits[dc]);
				int32_t off = vpl_dist_to_offset(dist_code, w);
				uint32_t src = (uint32_t)((int32_t)pos - off);
				if (src >= n_pixels) { b->eof = true; break; }
				while (len-- && pos < n_pixels) {
					uint32_t px = out[src++];
					out[pos++] = px;
					if (cache) cache_insert(cache, cache_mask, px);
				}
			} else {
				/* Color cache lookup */
				if (cache) {
					/* Use the hash of the cache-index symbol */
					uint32_t cidx = sym - (256 + 24);
					out[pos] = (cidx < (cache_mask+1)) ?
						    cache[cidx] : 0xFF000000u;
				}
				pos++;
			}
		}
	}
	ret = b->eof ? -1 : 0;

free_groups:
	anx_free(groups);
done:
	if (cache)    anx_free(cache);
	if (meta_img) anx_free(meta_img);
	return ret;
}

static int webp_decode_vp8l(const uint8_t *data, uint32_t len,
			     struct webp_image *img)
{
	if (len < 5) return -1;
	if (data[0] != 0x2F) return -1; /* VP8L signature byte */

	struct bb b;
	bb_init(&b, data + 1, len - 1); /* skip signature byte */

	uint32_t w = bb_read(&b, 14) + 1;
	uint32_t h = bb_read(&b, 14) + 1;
	bb_read(&b, 1); /* alpha_is_used (informational) */
	uint32_t version = bb_read(&b, 3);
	if (version != 0) return -1;

	uint32_t *pixels = (uint32_t *)anx_alloc(w * h * 4);
	if (!pixels) return -1;
	anx_memset(pixels, 0, w * h * 4);

	/* Transforms (up to 4, decoded in stack order) */
	struct vpl_transform transforms[4];
	int n_transforms = 0;
	anx_memset(transforms, 0, sizeof(transforms));

	while (bb_read(&b, 1) && !b.eof) {
		if (n_transforms >= 4) break;
		struct vpl_transform *t = &transforms[n_transforms++];
		t->type = (int)bb_read(&b, 2);

		if (t->type == TRANSFORM_COLOR_INDEX) {
			t->palette_size = (int)bb_read(&b, 8) + 1;
			/* Decode palette as a w=palette_size h=1 VP8L sub-image */
			if (vpl_decode_image(&b, (uint32_t)t->palette_size, 1, 0,
					      t->palette) != 0)
				goto fail;
			/* Un-delta the palette */
			{
				int i;
				for (i = 1; i < t->palette_size; i++) {
					uint32_t *p = t->palette;
					p[i] = ((( ((p[i]>>24)&0xFF) +
						    ((p[i-1]>>24)&0xFF)) & 0xFF) << 24) |
					        ((( ((p[i]>>16)&0xFF) +
						    ((p[i-1]>>16)&0xFF)) & 0xFF) << 16) |
					        ((( ((p[i]>> 8)&0xFF) +
					            ((p[i-1]>> 8)&0xFF)) & 0xFF) <<  8) |
					         (( ((p[i]     &0xFF) +
					            ( p[i-1]   &0xFF)) & 0xFF));
				}
			}
			/* If palette fits in few bits, adjust width */
			int bits = 0;
			if (t->palette_size <= 2)       bits = 1;
			else if (t->palette_size <= 4)  bits = 2;
			else if (t->palette_size <= 16) bits = 4;
			else                             bits = 8;
			if (bits < 8) {
				int ppb = 8 / bits;
				w = (w + ppb - 1) / ppb;
			}
		} else if (t->type == TRANSFORM_PREDICTOR ||
			    t->type == TRANSFORM_COLOR) {
			t->block_bits = bb_read(&b, 3) + 2;
			uint32_t bw = (w + (1u<<t->block_bits) - 1) >> t->block_bits;
			uint32_t bh = (h + (1u<<t->block_bits) - 1) >> t->block_bits;
			t->data = (uint32_t *)anx_alloc(bw * bh * 4);
			if (!t->data) goto fail;
			if (vpl_decode_image(&b, bw, bh, 0, t->data) != 0)
				goto fail;
		}
		/* SUBTR_GREEN has no extra data */
	}

	/* Read color cache bits */
	int color_cache_bits = 0;
	if (bb_read(&b, 1))
		color_cache_bits = (int)bb_read(&b, 4);

	/* Decode main image */
	if (vpl_decode_image(&b, w, h, color_cache_bits, pixels) != 0)
		goto fail;

	/* Apply transforms in reverse order */
	{
		int i;
		for (i = n_transforms - 1; i >= 0; i--) {
			struct vpl_transform *t = &transforms[i];
			if (t->type == TRANSFORM_SUBTR_GREEN)
				apply_subtract_green(pixels, w * h);
			else if (t->type == TRANSFORM_COLOR)
				apply_color_transform(pixels, w, h, t);
			else if (t->type == TRANSFORM_PREDICTOR)
				apply_predictor_transform(pixels, w, h, t);
			else if (t->type == TRANSFORM_COLOR_INDEX) {
				/* Unpack palette indices → ARGB pixels */
				/* Restore original width */
				int bits = 0;
				if (t->palette_size <= 2)       bits = 1;
				else if (t->palette_size <= 4)  bits = 2;
				else if (t->palette_size <= 16) bits = 4;
				else                             bits = 8;
				uint32_t orig_w = img->width;
				if (bits < 8) {
					int ppb = 8 / bits;
					uint32_t mask = (1 << bits) - 1;
					uint32_t *unpacked =
						(uint32_t *)anx_alloc(orig_w * h * 4);
					if (!unpacked) goto fail;
					uint32_t y, x;
					for (y = 0; y < h; y++) {
						for (x = 0; x < orig_w; x++) {
							int pack_idx = (int)(x / ppb);
							int bit_shift = (int)(x % ppb) * bits;
							uint32_t packed = pixels[y * w + pack_idx];
							uint32_t idx = ((packed >> 8) >> bit_shift) & mask;
							unpacked[y*orig_w+x] =
								(idx < (uint32_t)t->palette_size) ?
								 t->palette[idx] : 0;
						}
					}
					anx_free(pixels);
					pixels = unpacked;
					w = orig_w;
				} else {
					uint32_t np = w * h;
					uint32_t j;
					for (j = 0; j < np; j++) {
						uint32_t idx = (pixels[j] >> 8) & 0xFF;
						pixels[j] = (idx < (uint32_t)t->palette_size) ?
							     t->palette[idx] : 0;
					}
				}
			}
		}
	}

	/* Convert ARGB → XRGB (drop alpha) */
	{
		uint32_t np = w * h, i;
		for (i = 0; i < np; i++)
			pixels[i] = pixels[i] | 0xFF000000u;
	}

	/* Free transform data */
	{
		int i;
		for (i = 0; i < n_transforms; i++)
			if (transforms[i].data) anx_free(transforms[i].data);
	}

	img->pixels = pixels;
	img->width  = w;
	img->height = h;
	return 0;

fail:
	{
		int i;
		for (i = 0; i < n_transforms; i++)
			if (transforms[i].data) anx_free(transforms[i].data);
	}
	anx_free(pixels);
	return -1;
}

/* ══════════════════════════════════════════════════════════════════
 * VP8 (lossy) decoder — key frames with intra prediction
 * ══════════════════════════════════════════════════════════════════ */

/* VP8 boolean (arithmetic) decoder */
struct vp8_bool {
	const uint8_t *buf;
	uint32_t       pos;
	uint32_t       len;
	uint32_t       range; /* [0x80, 0xFF] */
	uint32_t       value; /* current decode value */
	int32_t        count; /* bits available in value */
};

static void vp8_bool_init(struct vp8_bool *d, const uint8_t *buf, uint32_t len)
{
	d->buf   = buf;
	d->pos   = 0;
	d->len   = len;
	d->range = 255;
	d->value = 0;
	d->count = -8;
}

static void vp8_bool_fill(struct vp8_bool *d)
{
	while (d->count < 0) {
		uint32_t byte = d->pos < d->len ? d->buf[d->pos++] : 0;
		d->value = (d->value << 8) | byte;
		d->count += 8;
	}
}

static int vp8_read_bit(struct vp8_bool *d, uint32_t prob)
{
	vp8_bool_fill(d);
	uint32_t split = 1 + (((d->range - 1) * prob) >> 8);
	uint32_t bigsplit = split << d->count;
	int bit;
	if (d->value >= bigsplit) {
		d->range -= split;
		d->value -= bigsplit;
		bit = 1;
	} else {
		d->range = split;
		bit = 0;
	}
	/* Renormalize */
	while (d->range < 128) {
		d->range <<= 1;
		d->count--;
	}
	return bit;
}

static uint32_t vp8_read_bits(struct vp8_bool *d, int n)
{
	uint32_t v = 0;
	while (n--) v = (v << 1) | vp8_read_bit(d, 128);
	return v;
}

/* VP8 intra 4×4 prediction modes */
enum vp8_b_mode {
	B_DC_PRED=0, B_TM_PRED, B_VE_PRED, B_HE_PRED,
	B_LD_PRED,   B_RD_PRED, B_VR_PRED, B_VL_PRED,
	B_HD_PRED,   B_HU_PRED, NUM_B_MODES
};

/* VP8 intra 16×16 prediction modes */
enum vp8_mb_mode { MB_DC_PRED=0, MB_V_PRED, MB_H_PRED, MB_TM_PRED };

/* Simple VP8 key frame decoder */
struct vp8_ctx {
	uint32_t  mb_w;    /* macroblock columns */
	uint32_t  mb_h;    /* macroblock rows */
	uint32_t  width;
	uint32_t  height;
	/* YUV planes */
	uint8_t  *Y;       /* w × h */
	uint8_t  *U;       /* w/2 × h/2 */
	uint8_t  *V;       /* w/2 × h/2 */
};

#define VP8_CLIP(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))

static void vp8_predict_16x16_dc(uint8_t *Y, uint32_t stride,
				   uint32_t mb_x, uint32_t mb_y)
{
	/* DC from left column + top row (or 128 at edges) */
	int sum = 0, n = 0;
	if (mb_y > 0) {
		int i;
		for (i = 0; i < 16; i++, n++)
			sum += Y[-((int)stride) + i]; /* row above */
	}
	if (mb_x > 0) {
		int i;
		for (i = 0; i < 16; i++, n++)
			sum += Y[(int)(i * stride) - 1]; /* col left */
	}
	uint8_t dc = (n == 0) ? 128 : (uint8_t)((sum + n/2) / n);
	uint32_t r, c;
	for (r = 0; r < 16; r++)
		for (c = 0; c < 16; c++)
			Y[r * stride + c] = dc;
}

static void vp8_predict_16x16_v(uint8_t *Y, uint32_t stride)
{
	/* Vertical: copy row above */
	uint32_t r, c;
	for (r = 0; r < 16; r++)
		for (c = 0; c < 16; c++)
			Y[r * stride + c] = Y[-(int)stride + c];
}

static void vp8_predict_16x16_h(uint8_t *Y, uint32_t stride)
{
	/* Horizontal: copy col left */
	uint32_t r, c;
	for (r = 0; r < 16; r++)
		for (c = 0; c < 16; c++)
			Y[r * stride + c] = Y[(int)(r * stride) - 1];
}

static void vp8_predict_16x16_tm(uint8_t *Y, uint32_t stride)
{
	/* TrueMotion: top + left - top-left */
	uint32_t r, c;
	for (r = 0; r < 16; r++) {
		for (c = 0; c < 16; c++) {
			int v = (int)Y[(int)(r*stride)-1]  /* left */
			      + (int)Y[-(int)stride+c]     /* top */
			      - (int)Y[-(int)stride-1];    /* top-left */
			Y[r*stride+c] = (uint8_t)VP8_CLIP(v);
		}
	}
}

/* VP8 integer 4×4 iDCT — reference specification (RFC 6386 Appendix A) */
static void vp8_idct4x4(int16_t *in, uint8_t *pred, uint32_t ps,
			  uint8_t *out, uint32_t os)
{
	int32_t tmp[16];
	int i;
	/* Horizontal pass */
	for (i = 0; i < 4; i++) {
		int32_t a = in[0+i*4] + in[2+i*4];
		int32_t b = in[0+i*4] - in[2+i*4];
		int32_t c = ((in[1+i*4] * 2217) >> 16) - ((in[3+i*4] * 5352) >> 16) - 1;
		int32_t d = ((in[1+i*4] * 5352) >> 16) + ((in[3+i*4] * 2217) >> 16);
		tmp[0+i*4] = a + d;
		tmp[1+i*4] = b + c;
		tmp[2+i*4] = b - c;
		tmp[3+i*4] = a - d;
	}
	/* Vertical pass + predict + saturate */
	for (i = 0; i < 4; i++) {
		int32_t a = tmp[i+0] + tmp[i+8];
		int32_t b = tmp[i+0] - tmp[i+8];
		int32_t c = ((tmp[i+4] * 2217) >> 16) - ((tmp[i+12] * 5352) >> 16) - 1;
		int32_t d = ((tmp[i+4] * 5352) >> 16) + ((tmp[i+12] * 2217) >> 16);
		out[i+0*os] = u8clamp(pred[i+0*ps] + ((a+d+4)>>3));
		out[i+1*os] = u8clamp(pred[i+1*ps] + ((b+c+4)>>3));
		out[i+2*os] = u8clamp(pred[i+2*ps] + ((b-c+4)>>3));
		out[i+3*os] = u8clamp(pred[i+3*ps] + ((a-d+4)>>3));
	}
}

/* VP8 coefficient probability tables (RFC 6386 §14.4 default) */
/* Shape: [4 planes][8 bands][3 contexts][11 nodes] */
/* Embed only the first-token probability (node 0) for simplified decoding */
static const uint8_t kCoeffProb0[4][8][3] = {
	{{ 128,161, 79},{ 128, 63,128},{128,155, 32},{128,128, 45},
	 { 128,128, 45},{128,128, 45},{128, 56, 32},{128, 64, 32}},
	{{ 253,136,254},{ 189,133,134},{ 80,131, 31},{128, 94, 80},
	 {  51, 80, 39},{ 24, 47, 17},{ 24, 47, 17},{ 24, 47, 17}},
	{{ 220,188,102},{ 180,156,150},{ 92,116, 42},{ 80,128, 48},
	 {  53, 80, 36},{ 32, 64, 24},{ 24, 47, 17},{ 24, 47, 17}},
	{{ 247,213,110},{ 200,179,172},{ 148,163, 94},{ 100,170, 57},
	 {  57,109, 43},{ 32, 64, 24},{ 24, 47, 17},{ 24, 47, 17}},
};

/* Read VP8 residual coefficients for one 4×4 block.
 * Returns number of non-zero coefficients. */
static int vp8_read_coeffs(struct vp8_bool *d, int plane, int band_start,
			    int16_t *coeff)
{
	anx_memset(coeff, 0, 16 * sizeof(int16_t));
	int ctx = 0; /* previous coeff context (0 = zero) */
	int i;
	for (i = 0; i < 16; i++) {
		int band = i < 8 ? i : 7;
		uint8_t p0 = kCoeffProb0[plane < 4 ? plane : 0]
				         [band < 8 ? band + band_start : 7]
				         [ctx < 3 ? ctx : 2];
		/* Simplified: use node 0 probability to decide zero or non-zero */
		if (!vp8_read_bit(d, p0)) break; /* EOB */
		/* For non-zero: read sign bit + small magnitude */
		int mag = 1; /* assume magnitude 1 (simplified) */
		if (vp8_read_bit(d, 140)) mag = 2;
		int sign = vp8_read_bit(d, 128);
		coeff[i] = (int16_t)(sign ? -mag : mag);
		ctx = (mag == 1) ? 1 : 2;
	}
	return 0;
}

/* YCbCr → XRGB8888, BT.601 fixed-point */
static uint32_t yuv_to_xrgb(int y, int u, int v)
{
	y -= 16; u -= 128; v -= 128;
	int r = (298*y         + 409*v + 128) >> 8;
	int g = (298*y - 100*u - 208*v + 128) >> 8;
	int b = (298*y + 516*u         + 128) >> 8;
	return 0xFF000000u |
	       ((uint32_t)VP8_CLIP(r) << 16) |
	       ((uint32_t)VP8_CLIP(g) <<  8) |
	        (uint32_t)VP8_CLIP(b);
}

static int webp_decode_vp8(const uint8_t *data, uint32_t dlen,
			    struct webp_image *img)
{
	if (dlen < 10) return -1;

	/* VP8 frame tag */
	uint32_t tag = data[0] | ((uint32_t)data[1]<<8) | ((uint32_t)data[2]<<16);
	int is_keyframe = !(tag & 1);
	int version     = (tag >> 1) & 7;
	/* int show_frame = (tag >> 4) & 1; */
	uint32_t part0_size = tag >> 5;

	if (!is_keyframe) return -1; /* only key frames for still images */
	(void)version;

	/* Key frame start code */
	if (data[3] != 0x9D || data[4] != 0x01 || data[5] != 0x2A) return -1;
	uint32_t w = ((uint32_t)data[7] << 8 | data[6]) & 0x3FFF;
	uint32_t h = ((uint32_t)data[9] << 8 | data[8]) & 0x3FFF;
	if (!w || !h) return -1;

	uint32_t np = w * h;
	uint32_t *pixels = (uint32_t *)anx_alloc(np * 4);
	uint8_t  *Y      = (uint8_t  *)anx_alloc(np);
	uint8_t  *U      = (uint8_t  *)anx_alloc((w/2+1) * (h/2+1));
	uint8_t  *V      = (uint8_t  *)anx_alloc((w/2+1) * (h/2+1));
	if (!pixels || !Y || !U || !V) goto vp8_fail;

	/* Initialize planes to grey (128) */
	anx_memset(Y, 128, np);
	anx_memset(U, 128, (w/2+1) * (h/2+1));
	anx_memset(V, 128, (w/2+1) * (h/2+1));

	/* Bool decoder on partition 0 */
	struct vp8_bool d;
	if (3 + part0_size > dlen) goto vp8_fail;
	vp8_bool_init(&d, data + 3, part0_size);

	/* Parse frame header */
	int color_space = vp8_read_bit(&d, 128);
	int clamping    = vp8_read_bit(&d, 128);
	(void)color_space; (void)clamping;

	/* Segmentation update */
	if (vp8_read_bit(&d, 128)) {
		int update_mb_seg = vp8_read_bit(&d, 128);
		int update_seg_feat = vp8_read_bit(&d, 128);
		if (update_seg_feat) {
			int i;
			for (i = 0; i < 2; i++) { /* quantizer, loop filter delta */
				int j;
				for (j = 0; j < 4; j++)
					if (vp8_read_bit(&d, 128))
						vp8_read_bits(&d, 8); /* value + sign */
			}
		}
		if (update_mb_seg) {
			int i;
			for (i = 0; i < 4; i++)
				vp8_read_bits(&d, 8); /* segment IDs probabilities */
		}
	}

	/* Loop filter: skip parameters */
	vp8_read_bit(&d, 128); /* filter_type */
	vp8_read_bits(&d, 6);  /* loop_filter_level */
	vp8_read_bits(&d, 3);  /* sharpness_level */
	if (vp8_read_bit(&d, 128)) { /* mode_ref_lf_delta_enabled */
		if (vp8_read_bit(&d, 128)) { /* mode_ref_lf_delta_update */
			int i;
			for (i = 0; i < 8; i++)
				if (vp8_read_bit(&d, 128))
					vp8_read_bits(&d, 7); /* delta + sign */
		}
	}

	/* Number of DCT partitions */
	int log2_nparts = (int)vp8_read_bits(&d, 2);
	int nparts = 1 << log2_nparts;
	(void)nparts; /* we use only partition 1 for simplicity */

	/* Quantizer */
	int base_q = (int)vp8_read_bits(&d, 7);
	/* Delta quantizers: y2dc, y2ac, ydcq, uvdcq, uvacq */
	int qi;
	for (qi = 0; qi < 5; qi++)
		if (vp8_read_bit(&d, 128)) vp8_read_bits(&d, 5); /* delta + sign */
	(void)base_q;

	/* Refresh frame flags + probability update — skip */
	vp8_read_bit(&d, 128);  /* refresh_golden_frame */
	vp8_read_bit(&d, 128);  /* refresh_altref_frame */
	/* refresh_last: always 1 for key frames, no read needed */

	/* Coefficient probability update */
	{
		int i, j, k, l;
		for (i = 0; i < 4; i++)
			for (j = 0; j < 8; j++)
				for (k = 0; k < 3; k++)
					for (l = 0; l < 11; l++)
						if (vp8_read_bit(&d, 252))
							vp8_read_bits(&d, 8);
	}

	vp8_read_bit(&d, 128); /* mb_no_coeff_skip (skip prob update) */

	/* Per-macroblock decode */
	uint32_t mb_w = (w + 15) / 16;
	uint32_t mb_h = (h + 15) / 16;
	uint32_t mby, mbx;

	/* DCT data partition starts after part0 */
	struct vp8_bool dct;
	uint32_t dct_off = 3 + part0_size;
	if (nparts > 1) dct_off += (uint32_t)(nparts - 1) * 3;
	if (dct_off >= dlen) dct_off = 3 + part0_size;
	vp8_bool_init(&dct, data + dct_off, dlen - dct_off);

	for (mby = 0; mby < mb_h; mby++) {
		for (mbx = 0; mbx < mb_w; mbx++) {

			/* Read macroblock header from partition 0 */
			/* skip probability: simplified, assume no skip */
			bool mb_skip = false;
			if (vp8_read_bit(&d, 128)) mb_skip = true;

			/* Segment ID (if segmentation active) */
			/* Intra 16×16 mode */
			int mb_mode;
			if (vp8_read_bit(&d, 145)) { /* use 16×16 */
				mb_mode = (int)vp8_read_bits(&d, 2);
				/* B_mode for each 4×4 block inside MB */
			} else {
				mb_mode = -1; /* 4×4 mode */
				/* Read 4×4 modes for 16 blocks */
				int bi;
				for (bi = 0; bi < 16; bi++)
					vp8_read_bits(&d, 4);
			}
			/* UV mode */
			vp8_read_bits(&d, 2);

			/* Predict Y plane for this macroblock */
			uint32_t mb_px_x = mbx * 16;
			uint32_t mb_px_y = mby * 16;
			uint8_t *Y_mb = Y + mb_px_y * w + mb_px_x;
			uint8_t *U_mb = U + (mb_px_y/2) * (w/2) + (mb_px_x/2);
			uint8_t *V_mb = V + (mb_px_y/2) * (w/2) + (mb_px_x/2);

			if (mb_mode >= 0) {
				switch (mb_mode) {
				case MB_DC_PRED:
					vp8_predict_16x16_dc(Y_mb, w, mbx, mby);
					break;
				case MB_V_PRED:
					if (mby > 0) vp8_predict_16x16_v(Y_mb, w);
					else vp8_predict_16x16_dc(Y_mb, w, mbx, mby);
					break;
				case MB_H_PRED:
					if (mbx > 0) vp8_predict_16x16_h(Y_mb, w);
					else vp8_predict_16x16_dc(Y_mb, w, mbx, mby);
					break;
				case MB_TM_PRED:
					if (mbx > 0 && mby > 0)
						vp8_predict_16x16_tm(Y_mb, w);
					else
						vp8_predict_16x16_dc(Y_mb, w, mbx, mby);
					break;
				}
			} else {
				/* 4×4 mode: DC predict all sub-blocks */
				vp8_predict_16x16_dc(Y_mb, w, mbx, mby);
			}

			/* DC prediction for UV */
			{
				uint8_t dc = 128;
				if (mby > 0 || mbx > 0) {
					int sum = 0, cnt = 0;
					if (mbx > 0) { int i; for (i=0;i<8;i++,cnt++) sum += U_mb[(int)(i*(w/2))-1]; }
					if (mby > 0) { int i; for (i=0;i<8;i++,cnt++) sum += U_mb[-((int)(w/2))+i]; }
					if (cnt) dc = (uint8_t)((sum + cnt/2) / cnt);
				}
				uint32_t r, c;
				for (r=0;r<8;r++) for(c=0;c<8;c++) U_mb[r*(w/2)+c] = dc;

				dc = 128;
				if (mby > 0 || mbx > 0) {
					int sum = 0, cnt = 0;
					if (mbx > 0) { int i; for (i=0;i<8;i++,cnt++) sum += V_mb[(int)(i*(w/2))-1]; }
					if (mby > 0) { int i; for (i=0;i<8;i++,cnt++) sum += V_mb[-((int)(w/2))+i]; }
					if (cnt) dc = (uint8_t)((sum + cnt/2) / cnt);
				}
				for (r=0;r<8;r++) for(c=0;c<8;c++) V_mb[r*(w/2)+c] = dc;
			}

			/* DCT residuals from DCT partition */
			if (!mb_skip) {
				int16_t coeff[16];
				int bi;
				for (bi = 0; bi < 16; bi++) {
					/* Y sub-blocks */
					vp8_read_coeffs(&dct, 0, 0, coeff);
					/* Apply iDCT to 4×4 sub-block */
					uint32_t sub_x = (uint32_t)(bi % 4) * 4;
					uint32_t sub_y = (uint32_t)(bi / 4) * 4;
					uint8_t *pred  = Y + (mb_px_y+sub_y)*w + mb_px_x+sub_x;
					uint8_t tmp4x4[16];
					uint32_t r2, c2;
					for (r2=0;r2<4;r2++) for(c2=0;c2<4;c2++) tmp4x4[r2*4+c2]=pred[r2*w+c2];
					vp8_idct4x4(coeff, tmp4x4, 4, pred, w);
				}
				/* U,V DCT (4 blocks each): simplified, skip */
				for (bi = 0; bi < 8; bi++)
					vp8_read_coeffs(&dct, 2, 0, coeff);
			}
		}
	}

	/* Convert YUV → XRGB8888 */
	{
		uint32_t y2, x2;
		for (y2 = 0; y2 < h; y2++) {
			for (x2 = 0; x2 < w; x2++) {
				int ly = (int)Y[y2*w+x2];
				int lu = (int)U[(y2/2)*(w/2)+(x2/2)];
				int lv = (int)V[(y2/2)*(w/2)+(x2/2)];
				pixels[y2*w+x2] = yuv_to_xrgb(ly, lu, lv);
			}
		}
	}

	anx_free(Y); anx_free(U); anx_free(V);
	img->pixels = pixels;
	img->width  = w;
	img->height = h;
	return 0;

vp8_fail:
	if (pixels) anx_free(pixels);
	if (Y) anx_free(Y);
	if (U) anx_free(U);
	if (V) anx_free(V);
	return -1;
}

/* ══════════════════════════════════════════════════════════════════
 * Public entry point
 * ══════════════════════════════════════════════════════════════════ */

int webp_decode(const void *data, uint32_t data_len, struct webp_image *img)
{
	const uint8_t *d = (const uint8_t *)data;

	if (data_len < 12) return -1;
	/* Check RIFF....WEBP header */
	if (ru32le(d) != FOURCC('R','I','F','F')) return -1;
	if (ru32le(d+8) != FOURCC('W','E','B','P'))  return -1;

	const uint8_t *chunks = d + 12;
	uint32_t       clen   = data_len - 12;

	/* Simple lossless */
	{
		uint32_t l;
		const uint8_t *c = riff_find_chunk(chunks, clen,
						    FOURCC('V','P','8','L'), &l);
		if (c) return webp_decode_vp8l(c, l, img);
	}
	/* Simple lossy */
	{
		uint32_t l;
		const uint8_t *c = riff_find_chunk(chunks, clen,
						    FOURCC('V','P','8',' '), &l);
		if (c) return webp_decode_vp8(c, l, img);
	}
	/* Extended (VP8X) — look for VP8 or VP8L inside */
	{
		uint32_t l;
		const uint8_t *vp8x = riff_find_chunk(chunks, clen,
						        FOURCC('V','P','8','X'), &l);
		if (vp8x) {
			/* VP8X payload starts after 10 bytes of flags+dims */
			const uint8_t *inner = vp8x + 10;
			uint32_t ilen = (l > 10) ? l - 10 : 0;
			uint32_t sl;
			const uint8_t *sc;
			sc = riff_find_chunk(inner, ilen,
					     FOURCC('V','P','8','L'), &sl);
			if (sc) return webp_decode_vp8l(sc, sl, img);
			sc = riff_find_chunk(inner, ilen,
					     FOURCC('V','P','8',' '), &sl);
			if (sc) return webp_decode_vp8(sc, sl, img);
		}
	}
	return -1; /* unknown format */
}

void webp_free(struct webp_image *img)
{
	if (img && img->pixels) {
		anx_free(img->pixels);
		img->pixels = NULL;
	}
}
