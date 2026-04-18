/*
 * fe25519.h — Field arithmetic for Curve25519/Ed25519.
 *
 * Field elements mod p = 2^255 - 19, represented as 5 x 51-bit limbs
 * in uint64_t. All operations are constant-time.
 *
 * This is an internal header shared between curve25519.c and ed25519.c.
 */

#ifndef FE25519_H
#define FE25519_H

#include <anx/types.h>
#include <anx/string.h>

/* Field element: 5 limbs, each holding up to 51 bits + carry room */
typedef uint64_t fe25519[5];

static inline void fe_0(fe25519 f)
{
	f[0] = f[1] = f[2] = f[3] = f[4] = 0;
}

static inline void fe_1(fe25519 f)
{
	f[0] = 1;
	f[1] = f[2] = f[3] = f[4] = 0;
}

static inline void fe_copy(fe25519 f, const fe25519 g)
{
	f[0] = g[0]; f[1] = g[1]; f[2] = g[2]; f[3] = g[3]; f[4] = g[4];
}

/*
 * Load 32 bytes (little-endian) into a field element.
 * Clears bit 255 (as required by Curve25519).
 */
static inline void fe_frombytes(fe25519 f, const uint8_t s[32])
{
	uint64_t h0, h1, h2, h3, h4;

	h0 = (uint64_t)s[0] | ((uint64_t)s[1] << 8) | ((uint64_t)s[2] << 16) |
	     ((uint64_t)s[3] << 24) | ((uint64_t)s[4] << 32) | ((uint64_t)s[5] << 40) |
	     ((uint64_t)(s[6] & 0x07) << 48);

	h1 = ((uint64_t)s[6] >> 3) | ((uint64_t)s[7] << 5) | ((uint64_t)s[8] << 13) |
	     ((uint64_t)s[9] << 21) | ((uint64_t)s[10] << 29) | ((uint64_t)s[11] << 37) |
	     ((uint64_t)(s[12] & 0x3f) << 45);

	h2 = ((uint64_t)s[12] >> 6) | ((uint64_t)s[13] << 2) | ((uint64_t)s[14] << 10) |
	     ((uint64_t)s[15] << 18) | ((uint64_t)s[16] << 26) | ((uint64_t)s[17] << 34) |
	     ((uint64_t)(s[18] & 0x01) << 42) | ((uint64_t)s[19] << 43);

	/* Recompute: we need exactly 51-bit limbs */
	/* Direct extraction is cleaner with masking */
	(void)h0; (void)h1; (void)h2; (void)h3; (void)h4;

	/*
	 * Read as a 256-bit little-endian number, mask bit 255,
	 * then split into 5 x 51-bit limbs.
	 */
	uint64_t lo, hi;

	lo = (uint64_t)s[0] | ((uint64_t)s[1] << 8) | ((uint64_t)s[2] << 16) |
	     ((uint64_t)s[3] << 24) | ((uint64_t)s[4] << 32) | ((uint64_t)s[5] << 40) |
	     ((uint64_t)s[6] << 48) | ((uint64_t)s[7] << 56);

	f[0] = lo & 0x7ffffffffffffULL;        /* bits 0..50 */
	f[1] = (lo >> 51) & 0x7ffffffffffffULL; /* This gets bits 51..63 from lo */

	/* We need more bits from the next chunk */
	hi = (uint64_t)s[8] | ((uint64_t)s[9] << 8) | ((uint64_t)s[10] << 16) |
	     ((uint64_t)s[11] << 24) | ((uint64_t)s[12] << 32) | ((uint64_t)s[13] << 40) |
	     ((uint64_t)s[14] << 48) | ((uint64_t)s[15] << 56);

	/* f[1] has 13 bits from lo; need 38 more from hi */
	f[1] |= (hi << 13) & 0x7ffffffffffffULL;
	f[2] = (hi >> 38) & 0x7ffffffffffffULL; /* 26 bits from hi */

	lo = (uint64_t)s[16] | ((uint64_t)s[17] << 8) | ((uint64_t)s[18] << 16) |
	     ((uint64_t)s[19] << 24) | ((uint64_t)s[20] << 32) | ((uint64_t)s[21] << 40) |
	     ((uint64_t)s[22] << 48) | ((uint64_t)s[23] << 56);

	/* f[2] has 26 bits from prev hi; need 25 more from lo */
	f[2] |= (lo << 26) & 0x7ffffffffffffULL;
	f[3] = (lo >> 25) & 0x7ffffffffffffULL; /* 39 bits from lo */

	hi = (uint64_t)s[24] | ((uint64_t)s[25] << 8) | ((uint64_t)s[26] << 16) |
	     ((uint64_t)s[27] << 24) | ((uint64_t)s[28] << 32) | ((uint64_t)s[29] << 40) |
	     ((uint64_t)s[30] << 48) | ((uint64_t)(s[31] & 0x7f) << 56);

	/* f[3] has 39 bits from lo; need 12 more from hi */
	f[3] |= (hi << 39) & 0x7ffffffffffffULL;
	f[4] = (hi >> 12) & 0x7ffffffffffffULL;
}

/* Reduce and store field element to 32 bytes (little-endian) */
static inline void fe_tobytes(uint8_t s[32], const fe25519 h)
{
	int64_t t[5];
	int64_t q;
	uint32_t i;

	t[0] = (int64_t)h[0]; t[1] = (int64_t)h[1];
	t[2] = (int64_t)h[2]; t[3] = (int64_t)h[3];
	t[4] = (int64_t)h[4];

	/* Carry to get canonical form */
	q = (t[0] + 19) >> 51;
	q = (t[1] + q) >> 51;
	q = (t[2] + q) >> 51;
	q = (t[3] + q) >> 51;
	q = (t[4] + q) >> 51;

	t[0] += 19 * q;

	/* Now carry normally */
	t[1] += t[0] >> 51; t[0] &= 0x7ffffffffffffLL;
	t[2] += t[1] >> 51; t[1] &= 0x7ffffffffffffLL;
	t[3] += t[2] >> 51; t[2] &= 0x7ffffffffffffLL;
	t[4] += t[3] >> 51; t[3] &= 0x7ffffffffffffLL;
	t[4] &= 0x7ffffffffffffLL;

	/*
	 * Pack 5 x 51-bit limbs into 32 bytes, little-endian.
	 * Limb layout: [0..50], [51..101], [102..152], [153..203], [204..254]
	 */
	anx_memset(s, 0, 32);

	uint64_t v;
	/* Limb 0: bits 0..50 -> bytes 0..6 (bits 0..55 of output) */
	v = (uint64_t)t[0];
	s[0] = (uint8_t)v; s[1] = (uint8_t)(v >> 8);
	s[2] = (uint8_t)(v >> 16); s[3] = (uint8_t)(v >> 24);
	s[4] = (uint8_t)(v >> 32); s[5] = (uint8_t)(v >> 40);
	s[6] = (uint8_t)(v >> 48); /* only 3 bits used */

	/* Limb 1: bits 51..101 -> starts at bit 51 */
	v = (uint64_t)t[1];
	s[6] |= (uint8_t)((v << 3) & 0xff);
	s[7] = (uint8_t)(v >> 5); s[8] = (uint8_t)(v >> 13);
	s[9] = (uint8_t)(v >> 21); s[10] = (uint8_t)(v >> 29);
	s[11] = (uint8_t)(v >> 37); s[12] = (uint8_t)(v >> 45); /* 6 bits */

	/* Limb 2: bits 102..152 -> starts at bit 102 */
	v = (uint64_t)t[2];
	s[12] |= (uint8_t)((v << 6) & 0xff);
	s[13] = (uint8_t)(v >> 2); s[14] = (uint8_t)(v >> 10);
	s[15] = (uint8_t)(v >> 18); s[16] = (uint8_t)(v >> 26);
	s[17] = (uint8_t)(v >> 34); s[18] = (uint8_t)(v >> 42);
	s[19] = (uint8_t)(v >> 50); /* 1 bit */

	/* Limb 3: bits 153..203 -> starts at bit 153 */
	v = (uint64_t)t[3];
	s[19] |= (uint8_t)((v << 1) & 0xff);
	s[20] = (uint8_t)(v >> 7); s[21] = (uint8_t)(v >> 15);
	s[22] = (uint8_t)(v >> 23); s[23] = (uint8_t)(v >> 31);
	s[24] = (uint8_t)(v >> 39); s[25] = (uint8_t)(v >> 47); /* 4 bits */

	/* Limb 4: bits 204..254 -> starts at bit 204 */
	v = (uint64_t)t[4];
	s[25] |= (uint8_t)((v << 4) & 0xff);
	s[26] = (uint8_t)(v >> 4); s[27] = (uint8_t)(v >> 12);
	s[28] = (uint8_t)(v >> 20); s[29] = (uint8_t)(v >> 28);
	s[30] = (uint8_t)(v >> 36); s[31] = (uint8_t)(v >> 44);

	/* Clear unused upper bits -- not needed since t[4] < 2^51 */
	(void)i;
}

/* Carry-reduce a field element after addition */
static inline void fe_carry(fe25519 f)
{
	uint64_t c;

	c = f[0] >> 51; f[0] &= 0x7ffffffffffffULL; f[1] += c;
	c = f[1] >> 51; f[1] &= 0x7ffffffffffffULL; f[2] += c;
	c = f[2] >> 51; f[2] &= 0x7ffffffffffffULL; f[3] += c;
	c = f[3] >> 51; f[3] &= 0x7ffffffffffffULL; f[4] += c;
	c = f[4] >> 51; f[4] &= 0x7ffffffffffffULL; f[0] += c * 19;
}

/* f = g + h */
static inline void fe_add(fe25519 f, const fe25519 g, const fe25519 h)
{
	f[0] = g[0] + h[0];
	f[1] = g[1] + h[1];
	f[2] = g[2] + h[2];
	f[3] = g[3] + h[3];
	f[4] = g[4] + h[4];
}

/* f = g - h (add 2p to avoid underflow before reducing) */
static inline void fe_sub(fe25519 f, const fe25519 g, const fe25519 h)
{
	/*
	 * Add 2*p to each limb to ensure no underflow.
	 * 2*p in 51-bit limbs: each limb is 2*(2^51 - 1) for inner limbs,
	 * and first limb adjusted by 2*19 = 38 for the reduction.
	 * Simpler: add a large multiple of p that's safe.
	 */
	f[0] = (g[0] + 0xfffffffffffdaULL) - h[0]; /* 2*(2^51-1) - 2*19 + 2 = 2^52-38 */
	f[1] = (g[1] + 0xffffffffffffeULL) - h[1]; /* 2*(2^51-1) */
	f[2] = (g[2] + 0xffffffffffffeULL) - h[2];
	f[3] = (g[3] + 0xffffffffffffeULL) - h[3];
	f[4] = (g[4] + 0xffffffffffffeULL) - h[4];
	fe_carry(f);
}

/*
 * Multiplication mod p using 128-bit intermediates.
 * Since we can't use __int128 reliably with -mgeneral-regs-only,
 * we use a schoolbook multiply with 64-bit results that fit in uint64_t
 * by splitting into 26-bit sub-limbs. But actually, with 51-bit limbs,
 * the products fit in 102 bits, so we need 128-bit intermediates.
 *
 * We'll use __uint128_t which is a compiler extension supported by
 * clang and gcc on 64-bit targets. It uses general-purpose registers.
 */
typedef unsigned __int128 uint128_t;

static inline void fe_mul(fe25519 f, const fe25519 g, const fe25519 h)
{
	uint128_t t0, t1, t2, t3, t4;
	uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
	uint64_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
	uint64_t h1_19 = h1 * 19, h2_19 = h2 * 19;
	uint64_t h3_19 = h3 * 19, h4_19 = h4 * 19;

	t0 = (uint128_t)g0 * h0 + (uint128_t)g1 * h4_19 +
	     (uint128_t)g2 * h3_19 + (uint128_t)g3 * h2_19 +
	     (uint128_t)g4 * h1_19;

	t1 = (uint128_t)g0 * h1 + (uint128_t)g1 * h0 +
	     (uint128_t)g2 * h4_19 + (uint128_t)g3 * h3_19 +
	     (uint128_t)g4 * h2_19;

	t2 = (uint128_t)g0 * h2 + (uint128_t)g1 * h1 +
	     (uint128_t)g2 * h0 + (uint128_t)g3 * h4_19 +
	     (uint128_t)g4 * h3_19;

	t3 = (uint128_t)g0 * h3 + (uint128_t)g1 * h2 +
	     (uint128_t)g2 * h1 + (uint128_t)g3 * h0 +
	     (uint128_t)g4 * h4_19;

	t4 = (uint128_t)g0 * h4 + (uint128_t)g1 * h3 +
	     (uint128_t)g2 * h2 + (uint128_t)g3 * h1 +
	     (uint128_t)g4 * h0;

	/* Carry chain */
	uint64_t c;
	f[0] = (uint64_t)t0 & 0x7ffffffffffffULL; c = (uint64_t)(t0 >> 51);
	t1 += c;
	f[1] = (uint64_t)t1 & 0x7ffffffffffffULL; c = (uint64_t)(t1 >> 51);
	t2 += c;
	f[2] = (uint64_t)t2 & 0x7ffffffffffffULL; c = (uint64_t)(t2 >> 51);
	t3 += c;
	f[3] = (uint64_t)t3 & 0x7ffffffffffffULL; c = (uint64_t)(t3 >> 51);
	t4 += c;
	f[4] = (uint64_t)t4 & 0x7ffffffffffffULL; c = (uint64_t)(t4 >> 51);
	f[0] += c * 19;
	c = f[0] >> 51; f[0] &= 0x7ffffffffffffULL; f[1] += c;
}

/* f = g^2 (slightly faster than fe_mul(f, g, g)) */
static inline void fe_sq(fe25519 f, const fe25519 g)
{
	uint128_t t0, t1, t2, t3, t4;
	uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
	uint64_t g0_2 = g0 * 2, g1_2 = g1 * 2, g3_2 = g3 * 2;
	uint64_t g1_38 = g1 * 38, g2_19 = g2 * 19, g3_19 = g3 * 19;
	uint64_t g3_38 = g3 * 38, g4_19 = g4 * 19, g4_38 = g4 * 38;

	/*
	 * Squaring (a0 + a1*2^51 + a2*2^102 + a3*2^153 + a4*2^204)^2:
	 *   t0 = a0^2 + 2*a1*a4*19 + 2*a2*a3*19
	 *   t1 = 2*a0*a1 + a3^2*19 + 2*a2*a4*19
	 *   t2 = 2*a0*a2 + a1^2 + 2*a3*a4*19
	 *   t3 = 2*a0*a3 + 2*a1*a2 + a4^2*19
	 *   t4 = 2*a0*a4 + 2*a1*a3 + a2^2
	 */
	t0 = (uint128_t)g0 * g0 + (uint128_t)g1_38 * g4 +
	     (uint128_t)g2_19 * g3_2;

	t1 = (uint128_t)g0_2 * g1 + (uint128_t)g3_19 * g3 +
	     (uint128_t)g2 * g4_38;

	t2 = (uint128_t)g0_2 * g2 + (uint128_t)g1 * g1 +
	     (uint128_t)g3_38 * g4;

	t3 = (uint128_t)g0_2 * g3 + (uint128_t)g1_2 * g2 +
	     (uint128_t)g4_19 * g4;

	t4 = (uint128_t)g0_2 * g4 + (uint128_t)g1_2 * g3 +
	     (uint128_t)g2 * g2;

	uint64_t c;
	f[0] = (uint64_t)t0 & 0x7ffffffffffffULL; c = (uint64_t)(t0 >> 51);
	t1 += c;
	f[1] = (uint64_t)t1 & 0x7ffffffffffffULL; c = (uint64_t)(t1 >> 51);
	t2 += c;
	f[2] = (uint64_t)t2 & 0x7ffffffffffffULL; c = (uint64_t)(t2 >> 51);
	t3 += c;
	f[3] = (uint64_t)t3 & 0x7ffffffffffffULL; c = (uint64_t)(t3 >> 51);
	t4 += c;
	f[4] = (uint64_t)t4 & 0x7ffffffffffffULL; c = (uint64_t)(t4 >> 51);
	f[0] += c * 19;
	c = f[0] >> 51; f[0] &= 0x7ffffffffffffULL; f[1] += c;
}

/* f = f * 121666 (used in Curve25519 scalar multiplication) */
static inline void fe_mul121666(fe25519 f, const fe25519 g)
{
	uint128_t t;
	uint64_t c;

	t = (uint128_t)g[0] * 121666;
	f[0] = (uint64_t)t & 0x7ffffffffffffULL; c = (uint64_t)(t >> 51);
	t = (uint128_t)g[1] * 121666 + c;
	f[1] = (uint64_t)t & 0x7ffffffffffffULL; c = (uint64_t)(t >> 51);
	t = (uint128_t)g[2] * 121666 + c;
	f[2] = (uint64_t)t & 0x7ffffffffffffULL; c = (uint64_t)(t >> 51);
	t = (uint128_t)g[3] * 121666 + c;
	f[3] = (uint64_t)t & 0x7ffffffffffffULL; c = (uint64_t)(t >> 51);
	t = (uint128_t)g[4] * 121666 + c;
	f[4] = (uint64_t)t & 0x7ffffffffffffULL; c = (uint64_t)(t >> 51);
	f[0] += c * 19;
}

/* f = 1/g (mod p), via Fermat's little theorem: g^(p-2) mod p */
static inline void fe_invert(fe25519 f, const fe25519 g)
{
	fe25519 t0, t1, t2, t3;
	int i;

	/* p-2 = 2^255 - 21 = 2^255 - 19 - 2 */
	/* Use addition chain from ref10 */

	fe_sq(t0, g);				/* t0 = g^2 */
	fe_sq(t1, t0);				/* t1 = g^4 */
	fe_sq(t1, t1);				/* t1 = g^8 */
	fe_mul(t1, g, t1);			/* t1 = g^9 */
	fe_mul(t0, t0, t1);			/* t0 = g^11 */
	fe_sq(t2, t0);				/* t2 = g^22 */
	fe_mul(t1, t1, t2);			/* t1 = g^(2^5 - 1) */
	fe_sq(t2, t1);
	for (i = 0; i < 4; i++)
		fe_sq(t2, t2);			/* t2 = g^(2^10 - 2^5) */
	fe_mul(t1, t2, t1);			/* t1 = g^(2^10 - 1) */
	fe_sq(t2, t1);
	for (i = 0; i < 9; i++)
		fe_sq(t2, t2);			/* t2 = g^(2^20 - 2^10) */
	fe_mul(t2, t2, t1);			/* t2 = g^(2^20 - 1) */
	fe_sq(t3, t2);
	for (i = 0; i < 19; i++)
		fe_sq(t3, t3);			/* t3 = g^(2^40 - 2^20) */
	fe_mul(t2, t3, t2);			/* t2 = g^(2^40 - 1) */
	fe_sq(t2, t2);
	for (i = 0; i < 9; i++)
		fe_sq(t2, t2);			/* t2 = g^(2^50 - 2^10) */
	fe_mul(t1, t2, t1);			/* t1 = g^(2^50 - 1) */
	fe_sq(t2, t1);
	for (i = 0; i < 49; i++)
		fe_sq(t2, t2);			/* t2 = g^(2^100 - 2^50) */
	fe_mul(t2, t2, t1);			/* t2 = g^(2^100 - 1) */
	fe_sq(t3, t2);
	for (i = 0; i < 99; i++)
		fe_sq(t3, t3);			/* t3 = g^(2^200 - 2^100) */
	fe_mul(t2, t3, t2);			/* t2 = g^(2^200 - 1) */
	fe_sq(t2, t2);
	for (i = 0; i < 49; i++)
		fe_sq(t2, t2);			/* t2 = g^(2^250 - 2^50) */
	fe_mul(t1, t2, t1);			/* t1 = g^(2^250 - 1) */
	fe_sq(t1, t1);				/* t1 = g^(2^251 - 2) */
	fe_sq(t1, t1);				/* t1 = g^(2^252 - 4) */
	fe_sq(t1, t1);				/* t1 = g^(2^253 - 8) */
	fe_sq(t1, t1);				/* t1 = g^(2^254 - 16) */
	fe_sq(t1, t1);				/* t1 = g^(2^255 - 32) */
	fe_mul(f, t1, t0);			/* f  = g^(2^255 - 21) */
}

/* Constant-time conditional swap: swap f and g if b is 1 */
static inline void fe_cswap(fe25519 f, fe25519 g, uint64_t b)
{
	uint64_t mask = (uint64_t)(-(int64_t)b);
	uint64_t x;
	int i;

	for (i = 0; i < 5; i++) {
		x = (f[i] ^ g[i]) & mask;
		f[i] ^= x;
		g[i] ^= x;
	}
}

/* f = -g (mod p) */
static inline void fe_neg(fe25519 f, const fe25519 g)
{
	fe25519 zero;
	fe_0(zero);
	fe_sub(f, zero, g);
}

/* Return 1 if f == 0 (mod p), 0 otherwise. Constant-time. */
static inline int fe_iszero(const fe25519 f)
{
	uint8_t s[32];
	uint8_t d = 0;
	int i;

	fe_tobytes(s, f);
	for (i = 0; i < 32; i++)
		d |= s[i];

	return (d == 0) ? 1 : 0;
}

/* Return bit 0 of f (after full reduction) */
static inline int fe_isneg(const fe25519 f)
{
	uint8_t s[32];

	fe_tobytes(s, f);
	return s[0] & 1;
}

/*
 * Compute f = g^((p-5)/8) = g^(2^252 - 3).
 * Used for square root computation in Ed25519 point decompression.
 */
static inline void fe_pow2523(fe25519 f, const fe25519 g)
{
	fe25519 t0, t1, t2;
	int i;

	fe_sq(t0, g);
	fe_sq(t1, t0);
	fe_sq(t1, t1);
	fe_mul(t1, g, t1);
	fe_mul(t0, t0, t1);
	fe_sq(t0, t0);
	fe_mul(t0, t1, t0);
	fe_sq(t1, t0);
	for (i = 0; i < 4; i++)
		fe_sq(t1, t1);
	fe_mul(t0, t1, t0);
	fe_sq(t1, t0);
	for (i = 0; i < 9; i++)
		fe_sq(t1, t1);
	fe_mul(t1, t1, t0);
	fe_sq(t2, t1);
	for (i = 0; i < 19; i++)
		fe_sq(t2, t2);
	fe_mul(t1, t2, t1);
	fe_sq(t1, t1);
	for (i = 0; i < 9; i++)
		fe_sq(t1, t1);
	fe_mul(t0, t1, t0);
	fe_sq(t1, t0);
	for (i = 0; i < 49; i++)
		fe_sq(t1, t1);
	fe_mul(t1, t1, t0);
	fe_sq(t2, t1);
	for (i = 0; i < 99; i++)
		fe_sq(t2, t2);
	fe_mul(t1, t2, t1);
	fe_sq(t1, t1);
	for (i = 0; i < 49; i++)
		fe_sq(t1, t1);
	fe_mul(t0, t1, t0);
	fe_sq(t0, t0);
	fe_sq(t0, t0);
	fe_mul(f, t0, g);
}

#endif /* FE25519_H */
