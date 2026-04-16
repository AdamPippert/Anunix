/*
 * jpeg.c — Baseline JPEG decoder for kernel splash screen.
 *
 * Decodes baseline sequential DCT JPEG (the most common format)
 * to XRGB8888 pixels. Supports YCbCr 4:2:0, 4:2:2, and 4:4:4
 * subsampling with standard Huffman tables.
 *
 * This is a minimal decoder — no progressive, no arithmetic coding,
 * no EXIF parsing. Sufficient for logo display at boot.
 */

#include <anx/types.h>
#include <anx/jpeg.h>
#include <anx/fb.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* JPEG markers */
#define M_SOI	0xFFD8
#define M_SOF0	0xFFC0	/* baseline DCT */
#define M_DHT	0xFFC4	/* define Huffman table */
#define M_DQT	0xFFDB	/* define quantization table */
#define M_DRI	0xFFDD	/* define restart interval */
#define M_SOS	0xFFDA	/* start of scan */
#define M_EOI	0xFFD9
#define M_APP0	0xFFE0

#define MAX_COMPONENTS	3
#define MAX_HTABLES	4

/* Decoder context */
struct jpeg_ctx {
	const uint8_t *data;
	uint32_t len;
	uint32_t pos;

	/* Image properties */
	uint16_t width, height;
	uint8_t num_components;
	struct {
		uint8_t id;
		uint8_t h_samp, v_samp;
		uint8_t qt_id;
		uint8_t dc_table, ac_table;
	} comp[MAX_COMPONENTS];
	uint8_t max_h_samp, max_v_samp;

	/* Quantization tables */
	int32_t qt[4][64];

	/* Huffman tables (DC and AC, 2 of each) */
	struct {
		uint8_t bits[17];	/* bits[i] = count of codes of length i */
		uint8_t vals[256];
		/* Derived for fast decode */
		int32_t maxcode[17];
		int32_t mincode[17];
		int32_t valptr[17];
		bool valid;
	} huff[MAX_HTABLES];	/* 0,1 = DC; 2,3 = AC */

	/* Bitstream reader */
	uint32_t bitbuf;
	int bits_left;

	/* DC prediction */
	int32_t dc_pred[MAX_COMPONENTS];

	/* Restart interval */
	uint16_t restart_interval;
	uint16_t restarts_left;

	/* Output */
	uint32_t *pixels;
};

/* --- Bitstream reader --- */

static uint8_t read_byte(struct jpeg_ctx *c)
{
	if (c->pos >= c->len)
		return 0;
	return c->data[c->pos++];
}

static uint16_t read_u16(struct jpeg_ctx *c)
{
	uint16_t v;

	v = (uint16_t)read_byte(c) << 8;
	v |= read_byte(c);
	return v;
}

static void fill_bits(struct jpeg_ctx *c)
{
	while (c->bits_left < 25 && c->pos < c->len) {
		uint8_t b = c->data[c->pos++];

		if (b == 0xFF) {
			uint8_t next;

			if (c->pos < c->len)
				next = c->data[c->pos];
			else
				next = 0;
			if (next == 0x00) {
				c->pos++;	/* byte-stuffed 0xFF */
			} else if (next >= 0xD0 && next <= 0xD7) {
				c->pos++;	/* restart marker */
				continue;
			} else {
				continue;	/* other marker — stop */
			}
		}

		c->bitbuf = (c->bitbuf << 8) | b;
		c->bits_left += 8;
	}
}

static int32_t get_bits(struct jpeg_ctx *c, int n)
{
	int32_t val;

	if (n == 0)
		return 0;

	while (c->bits_left < n)
		fill_bits(c);

	c->bits_left -= n;
	val = (int32_t)(c->bitbuf >> c->bits_left) & ((1 << n) - 1);
	return val;
}

static int32_t peek_bits(struct jpeg_ctx *c, int n)
{
	while (c->bits_left < n)
		fill_bits(c);
	return (int32_t)(c->bitbuf >> (c->bits_left - n)) & ((1 << n) - 1);
}

static void skip_bits(struct jpeg_ctx *c, int n)
{
	c->bits_left -= n;
}

/* --- Huffman decoding --- */

static void build_huffman(struct jpeg_ctx *c, int table_id)
{
	int i, j, code;
	int val_idx = 0;

	code = 0;
	for (i = 1; i <= 16; i++) {
		c->huff[table_id].mincode[i] = code;
		c->huff[table_id].valptr[i] = val_idx;
		for (j = 0; j < c->huff[table_id].bits[i]; j++) {
			code++;
			val_idx++;
		}
		c->huff[table_id].maxcode[i] = code - 1;
		code <<= 1;
	}
}

static int32_t huff_decode(struct jpeg_ctx *c, int table_id)
{
	int len;
	int32_t code;

	for (len = 1; len <= 16; len++) {
		code = peek_bits(c, len);
		if (code <= c->huff[table_id].maxcode[len]) {
			skip_bits(c, len);
			return c->huff[table_id].vals[
				c->huff[table_id].valptr[len] +
				code - c->huff[table_id].mincode[len]];
		}
	}
	return -1;
}

/* Extend a value to its sign-magnitude */
static int32_t extend(int32_t v, int bits)
{
	if (bits == 0)
		return 0;
	if (v < (1 << (bits - 1)))
		v = v - (1 << bits) + 1;
	return v;
}

/* --- Inverse DCT (Loeffler algorithm, integer approximation) --- */

/* Simple 2D IDCT: row pass then column pass */
static void idct_block(int32_t block[64])
{
	int i, j;
	int32_t tmp[64];

	/* Row pass */
	for (i = 0; i < 8; i++) {
		int32_t *row = &block[i * 8];
		int32_t s0 = row[0], s1 = row[1], s2 = row[2], s3 = row[3];
		int32_t s4 = row[4], s5 = row[5], s6 = row[6], s7 = row[7];

		/* Even part */
		int32_t e0 = s0 + s4, e1 = s0 - s4;
		int32_t e2 = s2 - s6, e3 = s2 + s6;

		int32_t a0 = e0 + e3, a3 = e0 - e3;
		int32_t a1 = e1 + e2, a2 = e1 - e2;

		/* Odd part (simplified) */
		int32_t o0 = s1 + s7, o1 = s5 + s3;
		int32_t o2 = s1 + s3, o3 = s5 + s7;

		int32_t b0 = s1 - s7, b1 = s5 - s3;
		int32_t z = (o2 + o3) * 362 >> 8;

		int32_t d0 = b0 * 669 >> 8;
		int32_t d1 = b1 * 277 >> 8;
		int32_t d2 = o0 * (-473) >> 8;
		int32_t d3 = o1 * (-669) >> 8;

		int32_t f0 = z + d2 + d0;
		int32_t f1 = z + d3 + d1;
		int32_t f2 = d2 - d0 + (o0 * 362 >> 8);
		int32_t f3 = d3 - d1 - (o1 * 362 >> 8);

		(void)f2; (void)f3;

		row[0] = a0 + f1; row[7] = a0 - f1;
		row[1] = a1 + f0; row[6] = a1 - f0;
		row[2] = a2 + b1; row[5] = a2 - b1;
		row[3] = a3 + b0; row[4] = a3 - b0;
	}

	/* Column pass */
	for (j = 0; j < 8; j++) {
		int32_t s0, s1, s2, s3, s4, s5, s6, s7;
		int32_t e0, e1, a0, a1, a2, a3;

		s0 = block[0*8+j]; s1 = block[1*8+j];
		s2 = block[2*8+j]; s3 = block[3*8+j];
		s4 = block[4*8+j]; s5 = block[5*8+j];
		s6 = block[6*8+j]; s7 = block[7*8+j];

		e0 = s0 + s4; e1 = s0 - s4;
		a0 = e0 + (s2 + s6);
		a3 = e0 - (s2 + s6);
		a1 = e1 + (s2 - s6);
		a2 = e1 - (s2 - s6);

		tmp[0*8+j] = (a0 + s1 + s7) >> 3;
		tmp[1*8+j] = (a1 + s3 + s5) >> 3;
		tmp[2*8+j] = (a2 + s3 - s5) >> 3;
		tmp[3*8+j] = (a3 + s1 - s7) >> 3;
		tmp[4*8+j] = (a3 - s1 + s7) >> 3;
		tmp[5*8+j] = (a2 - s3 + s5) >> 3;
		tmp[6*8+j] = (a1 - s3 - s5) >> 3;
		tmp[7*8+j] = (a0 - s1 - s7) >> 3;
	}

	anx_memcpy(block, tmp, 64 * sizeof(int32_t));
}

/* Zigzag order */
static const uint8_t zigzag[64] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63,
};

/* --- Marker parsing --- */

static int parse_dqt(struct jpeg_ctx *c)
{
	uint16_t len = read_u16(c);
	uint32_t end = c->pos + len - 2;
	int i;

	while (c->pos < end) {
		uint8_t info = read_byte(c);
		uint8_t prec = info >> 4;
		uint8_t id = info & 0x0F;

		if (id > 3)
			return ANX_EINVAL;

		for (i = 0; i < 64; i++) {
			if (prec)
				c->qt[id][zigzag[i]] = (int32_t)read_u16(c);
			else
				c->qt[id][zigzag[i]] = (int32_t)read_byte(c);
		}
	}
	return ANX_OK;
}

static int parse_dht(struct jpeg_ctx *c)
{
	uint16_t len = read_u16(c);
	uint32_t end = c->pos + len - 2;
	int i, total;

	while (c->pos < end) {
		uint8_t info = read_byte(c);
		uint8_t cls = info >> 4;	/* 0=DC, 1=AC */
		uint8_t id = info & 0x0F;
		int table_id = cls * 2 + id;

		if (table_id >= MAX_HTABLES)
			return ANX_EINVAL;

		total = 0;
		for (i = 1; i <= 16; i++) {
			c->huff[table_id].bits[i] = read_byte(c);
			total += c->huff[table_id].bits[i];
		}

		for (i = 0; i < total && i < 256; i++)
			c->huff[table_id].vals[i] = read_byte(c);

		c->huff[table_id].valid = true;
		build_huffman(c, table_id);
	}
	return ANX_OK;
}

static int parse_sof0(struct jpeg_ctx *c)
{
	uint16_t len;
	uint8_t prec;
	int i;

	len = read_u16(c);
	(void)len;
	prec = read_byte(c);
	if (prec != 8)
		return ANX_EINVAL;	/* only 8-bit */

	c->height = read_u16(c);
	c->width = read_u16(c);
	c->num_components = read_byte(c);
	if (c->num_components > MAX_COMPONENTS)
		return ANX_EINVAL;

	c->max_h_samp = 1;
	c->max_v_samp = 1;

	for (i = 0; i < c->num_components; i++) {
		c->comp[i].id = read_byte(c);
		{
			uint8_t samp = read_byte(c);

			c->comp[i].h_samp = samp >> 4;
			c->comp[i].v_samp = samp & 0x0F;
		}
		c->comp[i].qt_id = read_byte(c);
		if (c->comp[i].h_samp > c->max_h_samp)
			c->max_h_samp = c->comp[i].h_samp;
		if (c->comp[i].v_samp > c->max_v_samp)
			c->max_v_samp = c->comp[i].v_samp;
	}
	return ANX_OK;
}

static int parse_sos(struct jpeg_ctx *c)
{
	uint16_t len;
	uint8_t num;
	int i;

	len = read_u16(c);
	(void)len;
	num = read_byte(c);

	for (i = 0; i < num && i < MAX_COMPONENTS; i++) {
		uint8_t id = read_byte(c);
		uint8_t tables = read_byte(c);
		int ci;

		for (ci = 0; ci < c->num_components; ci++) {
			if (c->comp[ci].id == id) {
				c->comp[ci].dc_table = tables >> 4;
				c->comp[ci].ac_table = tables & 0x0F;
				break;
			}
		}
	}

	/* Skip spectral selection and successive approximation */
	read_byte(c); read_byte(c); read_byte(c);
	return ANX_OK;
}

/* --- Block decoding --- */

static int decode_block(struct jpeg_ctx *c, int32_t block[64],
			 int comp_idx)
{
	int32_t dc_val, ac_val;
	int dc_table, ac_table, qt_id;
	int i, run, size;
	int32_t sym;

	anx_memset(block, 0, 64 * sizeof(int32_t));

	dc_table = c->comp[comp_idx].dc_table;
	ac_table = c->comp[comp_idx].ac_table + 2;
	qt_id = c->comp[comp_idx].qt_id;

	/* DC coefficient */
	sym = huff_decode(c, dc_table);
	if (sym < 0)
		return ANX_EIO;
	dc_val = extend(get_bits(c, sym), sym);
	dc_val += c->dc_pred[comp_idx];
	c->dc_pred[comp_idx] = dc_val;
	block[0] = dc_val * c->qt[qt_id][0];

	/* AC coefficients */
	for (i = 1; i < 64; i++) {
		sym = huff_decode(c, ac_table);
		if (sym < 0)
			return ANX_EIO;
		if (sym == 0)
			break;	/* EOB */
		run = sym >> 4;
		size = sym & 0x0F;
		i += run;
		if (i >= 64)
			break;
		ac_val = extend(get_bits(c, size), size);
		block[zigzag[i]] = ac_val * c->qt[qt_id][zigzag[i]];
	}

	idct_block(block);
	return ANX_OK;
}

/* Clamp to 0-255 */
static uint8_t clamp8(int32_t v)
{
	if (v < 0) return 0;
	if (v > 255) return 255;
	return (uint8_t)v;
}

/* --- Main decode loop --- */

static int decode_scan(struct jpeg_ctx *c)
{
	uint32_t mcu_w, mcu_h, mcu_cols, mcu_rows;
	uint32_t mcu_x, mcu_y;
	int32_t block[64];
	int ci, bx, by;

	/* Component planes (Y, Cb, Cr) */
	int32_t *planes[MAX_COMPONENTS];
	uint32_t plane_w[MAX_COMPONENTS];
	uint32_t plane_h[MAX_COMPONENTS];
	int ret;

	mcu_w = (uint32_t)c->max_h_samp * 8;
	mcu_h = (uint32_t)c->max_v_samp * 8;
	mcu_cols = (c->width + mcu_w - 1) / mcu_w;
	mcu_rows = (c->height + mcu_h - 1) / mcu_h;

	/* Allocate component planes */
	for (ci = 0; ci < c->num_components; ci++) {
		plane_w[ci] = mcu_cols * c->comp[ci].h_samp * 8;
		plane_h[ci] = mcu_rows * c->comp[ci].v_samp * 8;
		planes[ci] = anx_zalloc(plane_w[ci] * plane_h[ci] *
					sizeof(int32_t));
		if (!planes[ci]) {
			int j;
			for (j = 0; j < ci; j++)
				anx_free(planes[j]);
			return ANX_ENOMEM;
		}
	}

	/* Reset bitstream and DC predictors */
	c->bitbuf = 0;
	c->bits_left = 0;
	for (ci = 0; ci < MAX_COMPONENTS; ci++)
		c->dc_pred[ci] = 0;
	c->restarts_left = c->restart_interval;

	/* Decode MCUs */
	for (mcu_y = 0; mcu_y < mcu_rows; mcu_y++) {
		for (mcu_x = 0; mcu_x < mcu_cols; mcu_x++) {
			for (ci = 0; ci < c->num_components; ci++) {
				int h = c->comp[ci].h_samp;
				int v = c->comp[ci].v_samp;

				for (by = 0; by < v; by++) {
					for (bx = 0; bx < h; bx++) {
						uint32_t px, py, pi, pj;

						ret = decode_block(c, block, ci);
						if (ret != ANX_OK)
							goto cleanup;

						/* Copy block to plane */
						px = (mcu_x * h + bx) * 8;
						py = (mcu_y * v + by) * 8;
						for (pi = 0; pi < 8; pi++) {
							for (pj = 0; pj < 8; pj++) {
								uint32_t idx;

								idx = (py + pi) * plane_w[ci] + (px + pj);
								if (idx < plane_w[ci] * plane_h[ci])
									planes[ci][idx] = block[pi * 8 + pj] + 128;
							}
						}
					}
				}
			}

			/* Handle restart markers */
			if (c->restart_interval > 0) {
				c->restarts_left--;
				if (c->restarts_left == 0) {
					c->restarts_left = c->restart_interval;
					c->bitbuf = 0;
					c->bits_left = 0;
					for (ci = 0; ci < MAX_COMPONENTS; ci++)
						c->dc_pred[ci] = 0;
					/* Skip restart marker bytes */
					while (c->pos < c->len &&
					       c->data[c->pos] != 0xFF)
						c->pos++;
					if (c->pos + 1 < c->len)
						c->pos += 2;
				}
			}
		}
	}

	/* Convert YCbCr to RGB and store in output pixels */
	{
		uint32_t x, y;

		for (y = 0; y < c->height; y++) {
			for (x = 0; x < c->width; x++) {
				int32_t yy, cb, cr;
				int32_t r, g, b;
				uint32_t cx, cy;

				/* Y is at full resolution */
				yy = planes[0][y * plane_w[0] + x];

				if (c->num_components >= 3) {
					/* Cb/Cr may be subsampled */
					cx = x * c->comp[1].h_samp / c->max_h_samp;
					cy = y * c->comp[1].v_samp / c->max_v_samp;
					cb = planes[1][cy * plane_w[1] + cx];
					cr = planes[2][cy * plane_w[2] + cx];
				} else {
					cb = 128;
					cr = 128;
				}

				/* YCbCr to RGB */
				cb -= 128;
				cr -= 128;
				r = yy + ((cr * 359) >> 8);
				g = yy - ((cb * 88 + cr * 183) >> 8);
				b = yy + ((cb * 454) >> 8);

				c->pixels[y * c->width + x] =
					((uint32_t)clamp8(r) << 16) |
					((uint32_t)clamp8(g) << 8) |
					(uint32_t)clamp8(b);
			}
		}
	}

cleanup:
	for (ci = 0; ci < c->num_components; ci++) {
		if (planes[ci])
			anx_free(planes[ci]);
	}
	return ANX_OK;
}

/* --- Public API --- */

int anx_jpeg_decode(const void *data, uint32_t data_len,
		     struct anx_jpeg_image *out)
{
	struct jpeg_ctx ctx;
	uint16_t marker;
	int ret;

	anx_memset(&ctx, 0, sizeof(ctx));
	ctx.data = (const uint8_t *)data;
	ctx.len = data_len;
	ctx.pos = 0;

	out->width = 0;
	out->height = 0;
	out->pixels = NULL;

	/* Check SOI marker */
	marker = read_u16(&ctx);
	if (marker != M_SOI)
		return ANX_EINVAL;

	/* Parse markers */
	while (ctx.pos < ctx.len) {
		uint8_t b = read_byte(&ctx);

		if (b != 0xFF)
			continue;

		while (ctx.pos < ctx.len && ctx.data[ctx.pos] == 0xFF)
			ctx.pos++;

		b = read_byte(&ctx);
		marker = 0xFF00 | b;

		switch (marker) {
		case M_SOF0:
			ret = parse_sof0(&ctx);
			if (ret != ANX_OK)
				return ret;
			break;
		case M_DHT:
			ret = parse_dht(&ctx);
			if (ret != ANX_OK)
				return ret;
			break;
		case M_DQT:
			ret = parse_dqt(&ctx);
			if (ret != ANX_OK)
				return ret;
			break;
		case M_DRI:
			read_u16(&ctx);	/* length */
			ctx.restart_interval = read_u16(&ctx);
			break;
		case M_SOS:
			ret = parse_sos(&ctx);
			if (ret != ANX_OK)
				return ret;

			/* Check if image fits in available memory.
			 * Decode needs: pixels (w*h*4) + 3 component
			 * planes (~w*h*4 total). Cap at 4MB pixel buffer
			 * to leave heap room for everything else.
			 */
			{
				uint64_t pixel_bytes;

				pixel_bytes = (uint64_t)ctx.width *
					      ctx.height * sizeof(uint32_t);
				if (pixel_bytes > 4 * 1024 * 1024)
					return ANX_ENOMEM;
			}

			/* Allocate output pixels */
			ctx.pixels = anx_zalloc(
				(uint64_t)ctx.width * ctx.height *
				sizeof(uint32_t));
			if (!ctx.pixels)
				return ANX_ENOMEM;

			ret = decode_scan(&ctx);
			if (ret != ANX_OK) {
				anx_free(ctx.pixels);
				return ret;
			}

			out->width = ctx.width;
			out->height = ctx.height;
			out->pixels = ctx.pixels;
			return ANX_OK;

		case M_EOI:
			return ANX_OK;

		default:
			/* Skip unknown marker */
			if (marker >= 0xFFC0) {
				uint16_t len = read_u16(&ctx);

				if (len >= 2)
					ctx.pos += len - 2;
			}
			break;
		}
	}

	return ANX_EINVAL;
}

void anx_jpeg_free(struct anx_jpeg_image *img)
{
	if (img->pixels) {
		anx_free(img->pixels);
		img->pixels = NULL;
	}
	img->width = 0;
	img->height = 0;
}

void anx_jpeg_blit_scaled(const struct anx_jpeg_image *img,
			   uint32_t fb_width, uint32_t fb_height)
{
	uint32_t x, y;
	uint32_t src_x, src_y;

	if (!img->pixels || !anx_fb_available())
		return;

	for (y = 0; y < fb_height; y++) {
		uint32_t *row = anx_fb_row_ptr(y);

		src_y = y * img->height / fb_height;
		if (src_y >= img->height)
			src_y = img->height - 1;

		for (x = 0; x < fb_width; x++) {
			src_x = x * img->width / fb_width;
			if (src_x >= img->width)
				src_x = img->width - 1;

			row[x] = img->pixels[src_y * img->width + src_x];
		}
	}
}
