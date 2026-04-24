/*
 * jpeg.c — Baseline JPEG decoder (SOF0).
 *
 * Supports:
 *   SOF0   — baseline DCT (sequential)
 *   1 or 3 components (grayscale / YCbCr)
 *   4:4:4 and 4:2:0 (H2V2) chroma subsampling
 *   Up to 4 quantization tables, 4 DC + 4 AC Huffman tables
 *
 * No libc, no float — integer-only arithmetic throughout.
 * Heap via anx_alloc/anx_free; string ops via <anx/string.h>.
 */

#include "jpeg.h"
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* ── byte helpers ──────────────────────────────────────────────── */

static inline uint16_t ru16be(const uint8_t *p)
{
	return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}

static inline int32_t clamp8(int32_t v)
{
	return (v < 0) ? 0 : (v > 255) ? 255 : v;
}

/* ── JPEG marker codes ─────────────────────────────────────────── */

#define M_SOI	0xFFD8
#define M_EOI	0xFFD9
#define M_SOF0	0xFFC0
#define M_DHT	0xFFC4
#define M_DQT	0xFFDB
#define M_SOS	0xFFDA
#define M_APP0	0xFFE0	/* APPn: E0..EF */
#define M_APP15	0xFFEF
#define M_COM	0xFFFE
#define M_DRI	0xFFDD
#define M_RST0	0xFFD0

/* ── Huffman table ─────────────────────────────────────────────── */

#define HUFF_MAX_CODES	256

struct huff_table {
	uint8_t  size[HUFF_MAX_CODES];	/* code lengths */
	uint16_t code[HUFF_MAX_CODES];	/* canonical codes */
	uint8_t  val[HUFF_MAX_CODES];	/* symbol values */
	int      count;			/* total number of codes */
};

/* Build canonical Huffman table from JPEG BITS[16] + HUFFVAL[]. */
static void huff_build(struct huff_table *ht,
		const uint8_t bits[16], const uint8_t *hval, int nval)
{
	int i, j, k = 0;
	uint16_t code = 0;

	ht->count = nval;
	for (i = 0; i < 16; i++) {
		for (j = 0; j < bits[i]; j++) {
			ht->size[k] = (uint8_t)(i + 1);
			ht->code[k] = code;
			ht->val[k]  = hval[k];
			k++;
			code++;
		}
		code <<= 1;
	}
}

/* ── MSB-first bit reader ──────────────────────────────────────── */

struct br {
	const uint8_t *data;
	uint32_t       pos;
	uint32_t       len;
	uint32_t       cache;	/* MSB-aligned bit cache */
	int            cbits;	/* valid bits in cache */
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

/* Refill at least 16 bits into cache (JPEG byte-stuffing: skip 0x00 after 0xFF). */
static void br_refill(struct br *b)
{
	while (b->cbits <= 16) {
		if (b->pos >= b->len) {
			b->cache |= (uint32_t)0xFF << (24 - b->cbits);
			b->cbits += 8;
			continue;
		}
		uint8_t byte = b->data[b->pos++];
		if (byte == 0xFF) {
			uint8_t next = (b->pos < b->len) ? b->data[b->pos] : 0;
			if (next == 0x00) {
				b->pos++;	/* byte stuffing: 0xFF 0x00 → 0xFF */
			} else if (next >= 0xD0 && next <= 0xD7) {
				/* RST marker — consume and treat as 0xFF */
				b->pos++;
			} else if (next == 0x00) {
				b->pos++;
			}
			/* else: just use 0xFF as-is (e.g. end marker peeked) */
		}
		b->cache |= (uint32_t)byte << (24 - b->cbits);
		b->cbits += 8;
	}
}

static int br_bits(struct br *b, int n)
{
	if (b->cbits < n)
		br_refill(b);
	int val = (int)(b->cache >> (32 - n));
	b->cache <<= n;
	b->cbits  -= n;
	return val;
}

static int br_bit(struct br *b)
{
	return br_bits(b, 1);
}

/* Decode one symbol from a Huffman table — linear scan (small tables). */
static int huff_decode(struct br *b, const struct huff_table *ht)
{
	int code = 0, i;
	int cur_len = 0;

	for (i = 0; i < ht->count; i++) {
		int sl = ht->size[i];
		/* Shift code register to match symbol's code length. */
		while (cur_len < sl) {
			code = (code << 1) | br_bit(b);
			cur_len++;
		}
		if (code == ht->code[i])
			return ht->val[i];
	}
	b->err = 1;
	return 0;
}

/* Extend sign of an n-bit integer (JPEG spec Appendix F). */
static inline int32_t extend_sign(int32_t v, int n)
{
	if (n == 0) return 0;
	int32_t vt = 1 << (n - 1);
	return (v < vt) ? v - (vt + vt - 1) : v;
}

/* ── Zigzag scan order ─────────────────────────────────────────── */

static const uint8_t zigzag[64] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
};

/* ── Integer iDCT (LLM factored, CONST_BITS=13) ────────────────── */

#define CONST_BITS	13
#define PASS1_BITS	2
#define DESCALE(x, n)	(((x) + (1 << ((n)-1))) >> (n))

#define FIX_0_541196100	4433
#define FIX_0_765366865	6270
#define FIX_1_847759065	15137
#define FIX_0_899976223	7373
#define FIX_1_175875602	9633
#define FIX_0_298631336	2446
#define FIX_2_053119869	16819
#define FIX_3_072711026	25172
#define FIX_1_501321110	12299
#define FIX_2_562915447	20995
#define FIX_1_961570560	16069
#define FIX_0_390180644	3196

/*
 * One 1-D iDCT pass over 8 values.
 * in[]:  input coefficients (stride = instride ints)
 * out[]: output (stride = outstride ints)
 * shift: total right-shift for output
 */
static void idct_1d(const int32_t *in, int instride,
		    int32_t *out, int outstride, int shift)
{
	int32_t tmp0, tmp1, tmp2, tmp3;
	int32_t tmp10, tmp11, tmp12, tmp13;
	int32_t z1, z2, z3, z4, z5;

	tmp0 = in[0 * instride];
	tmp1 = in[4 * instride];
	tmp2 = in[2 * instride];
	tmp3 = in[6 * instride];

	/* Even part */
	z1   = (tmp2 + tmp3) * FIX_0_541196100;
	tmp11 = z1 - tmp3 * FIX_1_847759065;
	tmp13 = z1 + tmp2 * FIX_0_765366865;

	tmp10 = (tmp0 + tmp1) << CONST_BITS;
	tmp12 = (tmp0 - tmp1) << CONST_BITS;

	int32_t ev0 = tmp10 + tmp13;
	int32_t ev1 = tmp12 + tmp11;
	int32_t ev2 = tmp12 - tmp11;
	int32_t ev3 = tmp10 - tmp13;

	/* Odd part */
	tmp0 = in[1 * instride];
	tmp1 = in[3 * instride];
	tmp2 = in[5 * instride];
	tmp3 = in[7 * instride];

	z1 = tmp0 + tmp3;
	z2 = tmp1 + tmp2;
	z3 = tmp0 + tmp2;
	z4 = tmp1 + tmp3;
	z5 = (z3 + z4) * FIX_1_175875602;

	tmp0 = tmp0 * FIX_0_298631336;
	tmp1 = tmp1 * FIX_2_053119869;
	tmp2 = tmp2 * FIX_3_072711026;
	tmp3 = tmp3 * FIX_1_501321110;
	z1   = z1 * (-FIX_0_899976223);
	z2   = z2 * (-FIX_2_562915447);
	z3   = z3 * (-FIX_1_961570560);
	z4   = z4 * (-FIX_0_390180644);

	z3 += z5;
	z4 += z5;

	tmp0 += z1 + z3;
	tmp1 += z2 + z4;
	tmp2 += z2 + z3;
	tmp3 += z1 + z4;

	out[0 * outstride] = DESCALE(ev0 + tmp3, shift);
	out[7 * outstride] = DESCALE(ev0 - tmp3, shift);
	out[1 * outstride] = DESCALE(ev1 + tmp2, shift);
	out[6 * outstride] = DESCALE(ev1 - tmp2, shift);
	out[2 * outstride] = DESCALE(ev2 + tmp1, shift);
	out[5 * outstride] = DESCALE(ev2 - tmp1, shift);
	out[3 * outstride] = DESCALE(ev3 + tmp0, shift);
	out[4 * outstride] = DESCALE(ev3 - tmp0, shift);
}

/*
 * Full 2-D iDCT on an 8x8 block.
 * coeff[64]: dequantized coefficients in natural (row-major) order.
 * out[64]:   level-shifted output samples [0,255].
 */
static void idct_block(const int32_t coeff[64], uint8_t out[64])
{
	int32_t ws[64];
	int i;

	/* Row pass: CONST_BITS + PASS1_BITS precision. */
	for (i = 0; i < 8; i++)
		idct_1d(coeff + i * 8, 1,
			ws + i * 8, 1,
			CONST_BITS - PASS1_BITS);

	/* Column pass: shift down to 8-bit range, add level shift (128). */
	for (i = 0; i < 8; i++) {
		int32_t col[8];
		idct_1d(ws + i, 8, col, 1, CONST_BITS + PASS1_BITS + 3);
		for (int j = 0; j < 8; j++)
			out[j * 8 + i] = (uint8_t)clamp8(col[j] + 128);
	}
}

/* ── Decoder context ───────────────────────────────────────────── */

#define MAX_COMPONENTS	3
#define MAX_QTABLES	4
#define MAX_HTABLES	4

struct component {
	uint8_t id;
	uint8_t h_samp;		/* horizontal sampling factor */
	uint8_t v_samp;		/* vertical sampling factor */
	uint8_t qtable_id;
	uint8_t dc_table_id;
	uint8_t ac_table_id;
	int32_t dc_pred;	/* DC predictor (differential coding) */
};

struct jpeg_ctx {
	uint32_t width;
	uint32_t height;
	uint8_t  precision;	/* sample precision (8) */
	int      n_comp;

	struct component comp[MAX_COMPONENTS];

	int32_t  qtable[MAX_QTABLES][64];	/* dequant tables */
	int      qtable_valid[MAX_QTABLES];

	struct huff_table dc_table[MAX_HTABLES];
	struct huff_table ac_table[MAX_HTABLES];
	int      dc_valid[MAX_HTABLES];
	int      ac_valid[MAX_HTABLES];

	/* Restart interval (0 = disabled). */
	uint16_t restart_interval;
};

/* ── Decode one 8x8 MCU component block ───────────────────────── */

static int decode_block(struct br *b,
			struct huff_table *dc_ht,
			struct huff_table *ac_ht,
			const int32_t qtab[64],
			int32_t *dc_pred,
			uint8_t out[64])
{
	int32_t coeff[64];
	anx_memset(coeff, 0, sizeof(coeff));

	/* DC coefficient */
	int ssss = huff_decode(b, dc_ht);
	if (b->err) return -1;
	int32_t diff = (ssss > 0) ? extend_sign(br_bits(b, ssss), ssss) : 0;
	*dc_pred += diff;
	coeff[0] = *dc_pred * qtab[0];

	/* AC coefficients */
	int k = 1;
	while (k < 64) {
		int rs = huff_decode(b, ac_ht);
		if (b->err) return -1;
		int r = (rs >> 4) & 0x0F;
		int s = rs & 0x0F;
		if (s == 0) {
			if (r == 15)
				k += 16;	/* ZRL: skip 16 zeros */
			else
				break;		/* EOB */
		} else {
			k += r;
			if (k >= 64) return -1;
			int32_t v = extend_sign(br_bits(b, s), s);
			coeff[zigzag[k]] = v * qtab[zigzag[k]];
			k++;
		}
	}

	idct_block(coeff, out);
	return 0;
}

/* ── YCbCr → XRGB8888 ──────────────────────────────────────────── */

static inline uint32_t ycbcr_to_xrgb(int32_t Y, int32_t Cb, int32_t Cr)
{
	/* BT.601 fixed-point; Y is already level-shifted to [0,255]. */
	int32_t yy = Y - 128;
	int32_t r = clamp8((298 * yy + 409 * Cr + 128) >> 8);
	int32_t g = clamp8((298 * yy - 100 * Cb - 208 * Cr + 128) >> 8);
	int32_t b = clamp8((298 * yy + 516 * Cb + 128) >> 8);
	return (uint32_t)((r << 16) | (g << 8) | b);
}

/* ── Marker parsing ────────────────────────────────────────────── */

static int parse_dqt(struct jpeg_ctx *ctx, const uint8_t *p, uint32_t len)
{
	uint32_t off = 0;
	while (off + 65 <= len) {
		uint8_t qt_info = p[off++];
		uint8_t precision = (qt_info >> 4) & 0x0F;
		uint8_t id        = qt_info & 0x0F;
		if (id >= MAX_QTABLES) return -1;
		if (precision == 0) {
			/* 8-bit values */
			for (int i = 0; i < 64; i++)
				ctx->qtable[id][i] = p[off + i];
			off += 64;
		} else if (precision == 1) {
			/* 16-bit values big-endian */
			if (off + 128 > len) return -1;
			for (int i = 0; i < 64; i++)
				ctx->qtable[id][i] = (int32_t)ru16be(p + off + i * 2);
			off += 128;
		} else {
			return -1;
		}
		ctx->qtable_valid[id] = 1;
	}
	return 0;
}

static int parse_dht(struct jpeg_ctx *ctx, const uint8_t *p, uint32_t len)
{
	uint32_t off = 0;
	while (off < len) {
		if (off + 17 > len) return -1;
		uint8_t tc = (p[off] >> 4) & 0x0F;	/* 0=DC, 1=AC */
		uint8_t th = p[off] & 0x0F;		/* table id */
		off++;
		if (tc > 1 || th >= MAX_HTABLES) return -1;

		uint8_t bits[16];
		int nval = 0;
		for (int i = 0; i < 16; i++) {
			bits[i] = p[off++];
			nval += bits[i];
		}
		if (nval > HUFF_MAX_CODES) return -1;
		if (off + (uint32_t)nval > len) return -1;

		if (tc == 0) {
			huff_build(&ctx->dc_table[th], bits, p + off, nval);
			ctx->dc_valid[th] = 1;
		} else {
			huff_build(&ctx->ac_table[th], bits, p + off, nval);
			ctx->ac_valid[th] = 1;
		}
		off += (uint32_t)nval;
	}
	return 0;
}

static int parse_sof0(struct jpeg_ctx *ctx, const uint8_t *p, uint32_t len)
{
	if (len < 11) return -1;
	ctx->precision = p[0];
	ctx->height    = ru16be(p + 1);
	ctx->width     = ru16be(p + 3);
	ctx->n_comp    = p[5];

	if (ctx->precision != 8) return -1;
	if (ctx->n_comp != 1 && ctx->n_comp != 3) return -1;
	if ((uint32_t)(6 + ctx->n_comp * 3) > len) return -1;

	for (int i = 0; i < ctx->n_comp; i++) {
		ctx->comp[i].id       = p[6 + i * 3];
		uint8_t sf            = p[7 + i * 3];
		ctx->comp[i].h_samp   = (sf >> 4) & 0x0F;
		ctx->comp[i].v_samp   = sf & 0x0F;
		ctx->comp[i].qtable_id = p[8 + i * 3];
		ctx->comp[i].dc_pred  = 0;
	}
	return 0;
}

static int parse_sos_header(struct jpeg_ctx *ctx, const uint8_t *p, uint32_t len)
{
	if (len < 3) return -1;
	int ns = p[0];
	if (ns != ctx->n_comp) return -1;
	if ((uint32_t)(1 + ns * 2 + 3) > len) return -1;

	for (int i = 0; i < ns; i++) {
		uint8_t cid = p[1 + i * 2];
		uint8_t tab = p[2 + i * 2];
		/* Find component by id */
		int ci = -1;
		for (int j = 0; j < ctx->n_comp; j++) {
			if (ctx->comp[j].id == cid) { ci = j; break; }
		}
		if (ci < 0) return -1;
		ctx->comp[ci].dc_table_id = (tab >> 4) & 0x0F;
		ctx->comp[ci].ac_table_id = tab & 0x0F;
	}
	return 0;
}

/* ── Main decoder ──────────────────────────────────────────────── */

int jpeg_decode(const void *data, uint32_t data_len, struct jpeg_image *img)
{
	const uint8_t *src = (const uint8_t *)data;
	uint32_t pos = 0;

	img->pixels = NULL;
	img->width  = 0;
	img->height = 0;

	/* SOI */
	if (data_len < 2) return -1;
	if (ru16be(src) != M_SOI) return -1;
	pos = 2;

	struct jpeg_ctx ctx;
	anx_memset(&ctx, 0, sizeof(ctx));

	int got_sof = 0, got_sos = 0;
	const uint8_t *sos_data = NULL;
	uint32_t sos_data_len = 0;

	/* Scan markers until SOS */
	while (pos + 4 <= data_len && !got_sos) {
		if (src[pos] != 0xFF) return -1;
		while (pos < data_len && src[pos] == 0xFF) pos++;
		if (pos >= data_len) return -1;
		uint8_t marker_lo = src[pos++];
		uint16_t marker = (uint16_t)(0xFF00 | marker_lo);

		if (marker == (uint16_t)M_EOI) break;
		if (marker == (uint16_t)M_SOI) continue;

		/* RST markers have no length */
		if (marker_lo >= 0xD0 && marker_lo <= 0xD7) continue;

		if (pos + 2 > data_len) return -1;
		uint16_t seg_len = ru16be(src + pos);
		if (seg_len < 2) return -1;
		uint32_t payload_len = seg_len - 2;
		pos += 2;
		if (pos + payload_len > data_len) return -1;
		const uint8_t *payload = src + pos;

		if (marker == (uint16_t)M_DQT) {
			if (parse_dqt(&ctx, payload, payload_len) < 0) return -1;
		} else if (marker == (uint16_t)M_DHT) {
			if (parse_dht(&ctx, payload, payload_len) < 0) return -1;
		} else if (marker == (uint16_t)M_SOF0) {
			if (parse_sof0(&ctx, payload, payload_len) < 0) return -1;
			got_sof = 1;
		} else if (marker == (uint16_t)M_SOS) {
			if (!got_sof) return -1;
			if (parse_sos_header(&ctx, payload, payload_len) < 0) return -1;
			/* SOS payload is followed by scan data */
			pos += payload_len;
			sos_data = src + pos;
			sos_data_len = data_len - pos;
			got_sos = 1;
			continue;	/* skip pos increment below */
		} else if (marker == (uint16_t)M_DRI) {
			if (payload_len >= 2)
				ctx.restart_interval = ru16be(payload);
		}
		/* APPn, COM, and unknowns: skip */

		pos += payload_len;
	}

	if (!got_sof || !got_sos) return -1;
	if (ctx.width == 0 || ctx.height == 0) return -1;

	/* Determine subsampling from luma component (comp[0]) */
	int h_samp = ctx.comp[0].h_samp;
	int v_samp = ctx.comp[0].v_samp;

	/* Only support 4:4:4 (1,1) and 4:2:0 (2,2) */
	if (ctx.n_comp == 3) {
		if (!((h_samp == 1 && v_samp == 1) ||
		      (h_samp == 2 && v_samp == 2)))
			return -1;
	}

	/* MCU dimensions */
	int mcu_w = (ctx.n_comp == 1) ? 8 : h_samp * 8;
	int mcu_h = (ctx.n_comp == 1) ? 8 : v_samp * 8;

	uint32_t mcu_cols = (ctx.width  + (uint32_t)mcu_w - 1) / (uint32_t)mcu_w;
	uint32_t mcu_rows = (ctx.height + (uint32_t)mcu_h - 1) / (uint32_t)mcu_h;

	uint32_t npix = ctx.width * ctx.height;
	uint32_t *pixels = (uint32_t *)anx_alloc(npix * 4);
	if (!pixels) return -1;
	anx_memset(pixels, 0, npix * 4);

	/* Per-component sample planes for one MCU row strip */
	uint8_t *Y_plane  = NULL;
	uint8_t *Cb_plane = NULL;
	uint8_t *Cr_plane = NULL;

	if (ctx.n_comp == 3) {
		Y_plane  = (uint8_t *)anx_alloc((uint32_t)(mcu_w * mcu_h));
		Cb_plane = (uint8_t *)anx_alloc(64);
		Cr_plane = (uint8_t *)anx_alloc(64);
		if (!Y_plane || !Cb_plane || !Cr_plane) goto oom;
	}

	struct br b;
	br_init(&b, sos_data, sos_data_len);

	int restart_count = 0;

	for (uint32_t mrow = 0; mrow < mcu_rows; mrow++) {
		for (uint32_t mcol = 0; mcol < mcu_cols; mcol++) {

			/* Restart interval handling */
			if (ctx.restart_interval > 0) {
				if (restart_count == ctx.restart_interval) {
					/* Align to byte, skip RST marker */
					b.cache = 0; b.cbits = 0;
					/* Skip 0xFF 0xDn */
					if (b.pos + 2 <= b.len &&
					    b.data[b.pos] == 0xFF) {
						b.pos += 2;
					}
					for (int ci = 0; ci < ctx.n_comp; ci++)
						ctx.comp[ci].dc_pred = 0;
					restart_count = 0;
				}
				restart_count++;
			}

			if (ctx.n_comp == 1) {
				/* Grayscale: single block */
				struct component *c = &ctx.comp[0];
				if (!ctx.dc_valid[c->dc_table_id] ||
				    !ctx.ac_valid[c->ac_table_id])
					goto err;
				uint8_t block[64];
				if (decode_block(&b,
					&ctx.dc_table[c->dc_table_id],
					&ctx.ac_table[c->ac_table_id],
					ctx.qtable[c->qtable_id],
					&c->dc_pred, block) < 0)
					goto err;

				uint32_t px = mcol * 8;
				uint32_t py = mrow * 8;
				for (int by = 0; by < 8; by++) {
					for (int bx = 0; bx < 8; bx++) {
						uint32_t ix = px + (uint32_t)bx;
						uint32_t iy = py + (uint32_t)by;
						if (ix < ctx.width && iy < ctx.height) {
							uint8_t luma = block[by * 8 + bx];
							pixels[iy * ctx.width + ix] =
								(uint32_t)((luma << 16) |
								(luma << 8) | luma);
						}
					}
				}

			} else {
				/* YCbCr: decode luma blocks then chroma */
				struct component *cy  = &ctx.comp[0];
				struct component *ccb = &ctx.comp[1];
				struct component *ccr = &ctx.comp[2];

				/* Luma: h_samp*v_samp blocks */
				for (int vy = 0; vy < v_samp; vy++) {
					for (int hx = 0; hx < h_samp; hx++) {
						uint8_t *dst = Y_plane +
							(vy * 8) * mcu_w + hx * 8;
						uint8_t tmp[64];
						if (decode_block(&b,
							&ctx.dc_table[cy->dc_table_id],
							&ctx.ac_table[cy->ac_table_id],
							ctx.qtable[cy->qtable_id],
							&cy->dc_pred, tmp) < 0)
							goto err;
						for (int r = 0; r < 8; r++)
							anx_memcpy(dst + r * mcu_w,
								tmp + r * 8, 8);
					}
				}

				/* Cb block */
				if (decode_block(&b,
					&ctx.dc_table[ccb->dc_table_id],
					&ctx.ac_table[ccb->ac_table_id],
					ctx.qtable[ccb->qtable_id],
					&ccb->dc_pred, Cb_plane) < 0)
					goto err;

				/* Cr block */
				if (decode_block(&b,
					&ctx.dc_table[ccr->dc_table_id],
					&ctx.ac_table[ccr->ac_table_id],
					ctx.qtable[ccr->qtable_id],
					&ccr->dc_pred, Cr_plane) < 0)
					goto err;

				/* Convert and write pixels */
				uint32_t px0 = mcol * (uint32_t)mcu_w;
				uint32_t py0 = mrow * (uint32_t)mcu_h;

				for (int ly = 0; ly < mcu_h; ly++) {
					for (int lx = 0; lx < mcu_w; lx++) {
						uint32_t ix = px0 + (uint32_t)lx;
						uint32_t iy = py0 + (uint32_t)ly;
						if (ix >= ctx.width || iy >= ctx.height)
							continue;

						int32_t Y_val  = (int32_t)Y_plane[ly * mcu_w + lx];

						int32_t cb_x, cb_y;
						if (h_samp == 2 && v_samp == 2) {
							/* 4:2:0: nearest-neighbor upsample */
							cb_x = lx / 2;
							cb_y = ly / 2;
						} else {
							cb_x = lx;
							cb_y = ly;
						}
						int32_t Cb_val = (int32_t)Cb_plane[cb_y * 8 + cb_x] - 128;
						int32_t Cr_val = (int32_t)Cr_plane[cb_y * 8 + cb_x] - 128;

						pixels[iy * ctx.width + ix] =
							ycbcr_to_xrgb(Y_val, Cb_val, Cr_val);
					}
				}
			}

			if (b.err) goto err;
		}
	}

	if (ctx.n_comp == 3) {
		anx_free(Y_plane);
		anx_free(Cb_plane);
		anx_free(Cr_plane);
	}

	img->width  = ctx.width;
	img->height = ctx.height;
	img->pixels = pixels;
	return 0;

err:
	if (ctx.n_comp == 3) {
		anx_free(Y_plane);
		anx_free(Cb_plane);
		anx_free(Cr_plane);
	}
	anx_free(pixels);
	return -1;

oom:
	anx_free(Y_plane);
	anx_free(Cb_plane);
	anx_free(Cr_plane);
	anx_free(pixels);
	return -1;
}

void jpeg_free(struct jpeg_image *img)
{
	if (img && img->pixels) {
		anx_free(img->pixels);
		img->pixels = NULL;
	}
}
