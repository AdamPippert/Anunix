/*
 * ed25519.c — Ed25519 digital signatures.
 *
 * Uses the twisted Edwards curve -x^2 + y^2 = 1 + d*x^2*y^2
 * where d = -121665/121666 mod p, p = 2^255 - 19.
 *
 * Extended coordinates (X, Y, Z, T) where x = X/Z, y = Y/Z, T = X*Y/Z.
 * Reference implementation: clarity over speed.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/crypto.h>
#include "fe25519.h"

/* --- Ed25519 group element in extended coordinates --- */

struct ge25519 {
	fe25519 X;
	fe25519 Y;
	fe25519 Z;
	fe25519 T;
};

/* d = -121665/121666 mod p (precomputed, little-endian) */
static const uint8_t d_bytes[32] = {
	0xa3, 0x78, 0x59, 0x13, 0xca, 0x4d, 0xeb, 0x75,
	0xab, 0xd8, 0x41, 0x41, 0x4d, 0x0a, 0x70, 0x00,
	0x98, 0xe8, 0x79, 0x77, 0x79, 0x40, 0xc7, 0x8c,
	0x73, 0xfe, 0x6f, 0x2b, 0xee, 0x6c, 0x03, 0x52,
};

/* 2*d mod p */
static const uint8_t d2_bytes[32] = {
	0x59, 0xf1, 0xb2, 0x26, 0x94, 0x9b, 0xd6, 0xeb,
	0x56, 0xb1, 0x83, 0x82, 0x9a, 0x14, 0xe0, 0x00,
	0x30, 0xd1, 0xf3, 0xee, 0xf2, 0x80, 0x8e, 0x19,
	0xe7, 0xfc, 0xdf, 0x56, 0xdc, 0xd9, 0x06, 0x24,
};

/* Basepoint B */
static const uint8_t B_y[32] = {
	0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
	0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
};

/* sqrt(-1) mod p, precomputed */
static const uint8_t sqrtm1_bytes[32] = {
	0xb0, 0xa0, 0x0e, 0x4a, 0x27, 0x1b, 0xee, 0xc4,
	0x78, 0xe4, 0x2f, 0xad, 0x06, 0x18, 0x43, 0x2f,
	0xa7, 0xd7, 0xfb, 0x3d, 0x99, 0x00, 0x4d, 0x2b,
	0x0b, 0xdf, 0xc1, 0x4f, 0x80, 0x24, 0x83, 0x2b,
};

/*
 * The Ed25519 group order:
 * l = 2^252 + 27742317777372353535851937790883648493
 * Kept for reference; reduction is done via sc_reduce using
 * the limb constants directly.
 */

/* --- Point operations in extended coordinates --- */

/* Neutral element (identity) */
static void ge_0(struct ge25519 *p)
{
	fe_0(p->X);
	fe_1(p->Y);
	fe_1(p->Z);
	fe_0(p->T);
}

/*
 * Point addition: r = p + q
 * Using the unified addition formula for extended coordinates.
 * Reference: HWCD08, Section 3.1
 */
static void ge_add(struct ge25519 *r, const struct ge25519 *p,
		   const struct ge25519 *q)
{
	fe25519 A, B, C, D, E, F, G, H;
	fe25519 d2;

	fe_frombytes(d2, d2_bytes);

	fe_sub(A, p->Y, p->X);
	fe_sub(B, q->Y, q->X);
	fe_mul(A, A, B);
	fe_add(B, p->Y, p->X);
	fe_add(C, q->Y, q->X);
	fe_mul(B, B, C);
	fe_mul(C, p->T, q->T);
	fe_mul(C, C, d2);
	fe_mul(D, p->Z, q->Z);
	fe_add(D, D, D);
	fe_sub(E, B, A);
	fe_sub(F, D, C);
	fe_add(G, D, C);
	fe_add(H, B, A);
	fe_mul(r->X, E, F);
	fe_mul(r->Y, G, H);
	fe_mul(r->Z, F, G);
	fe_mul(r->T, E, H);
}

/*
 * Point doubling: r = 2*p
 * Uses the dedicated doubling formula (faster than generic addition).
 */
static void ge_double(struct ge25519 *r, const struct ge25519 *p)
{
	fe25519 A, B, C, D, E, F, G, H;

	fe_sq(A, p->X);
	fe_sq(B, p->Y);
	fe_sq(C, p->Z);
	fe_add(C, C, C);
	fe_neg(D, A);
	fe_add(E, p->X, p->Y);
	fe_sq(E, E);
	fe_sub(E, E, A);
	fe_sub(E, E, B);
	fe_add(G, D, B);
	fe_sub(F, G, C);
	fe_sub(H, D, B);
	fe_mul(r->X, E, F);
	fe_mul(r->Y, G, H);
	fe_mul(r->Z, F, G);
	fe_mul(r->T, E, H);
}

/*
 * Scalar multiplication: r = s * p
 * Simple double-and-add, constant-time (always do both branches,
 * select via conditional move).
 */
static void ge_scalarmult(struct ge25519 *r, const uint8_t s[32],
			  const struct ge25519 *p)
{
	struct ge25519 Q, T;
	int i, bit;

	ge_0(&Q);

	/* Process from the most significant bit down */
	for (i = 255; i >= 0; i--) {
		ge_double(&Q, &Q);
		ge_add(&T, &Q, p);

		bit = (s[i / 8] >> (i & 7)) & 1;

		/* Constant-time select: Q = bit ? T : Q */
		fe_cswap(Q.X, T.X, (uint64_t)bit);
		fe_cswap(Q.Y, T.Y, (uint64_t)bit);
		fe_cswap(Q.Z, T.Z, (uint64_t)bit);
		fe_cswap(Q.T, T.T, (uint64_t)bit);
		/* After swap, Q has the value we want, T has the other */
		/* (We only keep Q, T is discarded) */
	}

	fe_copy(r->X, Q.X);
	fe_copy(r->Y, Q.Y);
	fe_copy(r->Z, Q.Z);
	fe_copy(r->T, Q.T);
}

/* Decode a point from 32 bytes (compressed Edwards point) */
static int ge_frombytes(struct ge25519 *p, const uint8_t s[32])
{
	fe25519 u, v, v3, vxx, check, sqrtm1;
	int x_sign;

	x_sign = s[31] >> 7;

	/* y coordinate is in the low 255 bits */
	fe_frombytes(p->Y, s);
	fe_1(p->Z);

	/* Recover x from y:
	 * x^2 = (y^2 - 1) / (d*y^2 + 1)
	 */
	fe25519 d;
	fe_frombytes(d, d_bytes);

	fe_sq(u, p->Y);		/* u = y^2 */
	fe_mul(v, u, d);		/* v = d*y^2 */
	fe_sub(u, u, p->Z);		/* u = y^2 - 1 */
	fe_add(v, v, p->Z);		/* v = d*y^2 + 1 */

	fe_sq(v3, v);
	fe_mul(v3, v3, v);		/* v3 = v^3 */
	fe_sq(p->X, v3);
	fe_mul(p->X, p->X, v);		/* X = v^7 */
	fe_mul(p->X, p->X, u);		/* X = u*v^7 */

	fe_pow2523(p->X, p->X);	/* X = (u*v^7)^((p-5)/8) */
	fe_mul(p->X, p->X, v3);	/* X = v^3 * (u*v^7)^((p-5)/8) */
	fe_mul(p->X, p->X, u);		/* X = u*v^3 * (u*v^7)^((p-5)/8) */

	/* Check: vx^2 = u */
	fe_sq(vxx, p->X);
	fe_mul(check, vxx, v);
	fe_sub(check, check, u);

	if (!fe_iszero(check)) {
		/*
		 * check = v*x^2 - u was non-zero. Try v*x^2 + u == 0 instead
		 * (i.e. x was computed as sqrt(-u/v) rather than sqrt(u/v)).
		 * We add 2*u to check, but fe_add does not carry — the limbs
		 * can grow to roughly 3*2^51, which overflows fe_tobytes's
		 * canonicalisation inside fe_iszero. Carry-reduce first.
		 */
		fe_add(check, check, u);
		fe_add(check, check, u);
		fe_carry(check);
		if (!fe_iszero(check))
			return -1; /* Not on curve */

		fe_frombytes(sqrtm1, sqrtm1_bytes);
		fe_mul(p->X, p->X, sqrtm1);
	}

	/* Adjust sign */
	if (fe_isneg(p->X) != x_sign)
		fe_neg(p->X, p->X);

	fe_mul(p->T, p->X, p->Y);

	return 0;
}

/* Encode a point to 32 bytes */
static void ge_tobytes(uint8_t s[32], const struct ge25519 *p)
{
	fe25519 x, y, z_inv;

	fe_invert(z_inv, p->Z);
	fe_mul(x, p->X, z_inv);
	fe_mul(y, p->Y, z_inv);
	fe_tobytes(s, y);
	s[31] ^= (uint8_t)(fe_isneg(x) << 7);
}

/* --- Scalar arithmetic mod l --- */

/*
 * Reduce a 64-byte (512-bit) scalar mod l.
 * Uses Barrett reduction with precomputed constants.
 * This is the simplest correct approach: schoolbook with carries.
 */
static void sc_reduce(uint8_t out[32], const uint8_t s[64])
{
	/*
	 * Reduce 512-bit s modulo l = 2^252 + 27742317777372353535851937790883648493
	 *
	 * We work with the scalar in 21 x 24-bit limbs for manageable carries.
	 * This follows the NaCl/ref10 approach.
	 */
	int64_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
	int64_t s12, s13, s14, s15, s16, s17, s18, s19, s20, s21, s22, s23;
	int64_t carry;

	s0 = (int64_t)(2097151 & ((uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16)));
	s1 = (int64_t)(2097151 & (((uint32_t)s[2] >> 5) | ((uint32_t)s[3] << 3) | ((uint32_t)s[4] << 11) | ((uint32_t)s[5] << 19)));
	s2 = (int64_t)(2097151 & (((uint32_t)s[5] >> 2) | ((uint32_t)s[6] << 6) | ((uint32_t)s[7] << 14)));
	s3 = (int64_t)(2097151 & (((uint32_t)s[7] >> 7) | ((uint32_t)s[8] << 1) | ((uint32_t)s[9] << 9) | ((uint32_t)s[10] << 17)));
	s4 = (int64_t)(2097151 & (((uint32_t)s[10] >> 4) | ((uint32_t)s[11] << 4) | ((uint32_t)s[12] << 12) | ((uint32_t)s[13] << 20)));
	s5 = (int64_t)(2097151 & (((uint32_t)s[13] >> 1) | ((uint32_t)s[14] << 7) | ((uint32_t)s[15] << 15)));
	s6 = (int64_t)(2097151 & (((uint32_t)s[15] >> 6) | ((uint32_t)s[16] << 2) | ((uint32_t)s[17] << 10) | ((uint32_t)s[18] << 18)));
	s7 = (int64_t)(2097151 & (((uint32_t)s[18] >> 3) | ((uint32_t)s[19] << 5) | ((uint32_t)s[20] << 13)));
	s8 = (int64_t)(2097151 & ((uint32_t)s[21] | ((uint32_t)s[22] << 8) | ((uint32_t)s[23] << 16)));
	s9 = (int64_t)(2097151 & (((uint32_t)s[23] >> 5) | ((uint32_t)s[24] << 3) | ((uint32_t)s[25] << 11) | ((uint32_t)s[26] << 19)));
	s10 = (int64_t)(2097151 & (((uint32_t)s[26] >> 2) | ((uint32_t)s[27] << 6) | ((uint32_t)s[28] << 14)));
	s11 = (int64_t)(2097151 & (((uint32_t)s[28] >> 7) | ((uint32_t)s[29] << 1) | ((uint32_t)s[30] << 9) | ((uint32_t)s[31] << 17)));
	s12 = (int64_t)(2097151 & (((uint32_t)s[31] >> 4) | ((uint32_t)s[32] << 4) | ((uint32_t)s[33] << 12) | ((uint32_t)s[34] << 20)));
	s13 = (int64_t)(2097151 & (((uint32_t)s[34] >> 1) | ((uint32_t)s[35] << 7) | ((uint32_t)s[36] << 15)));
	s14 = (int64_t)(2097151 & (((uint32_t)s[36] >> 6) | ((uint32_t)s[37] << 2) | ((uint32_t)s[38] << 10) | ((uint32_t)s[39] << 18)));
	s15 = (int64_t)(2097151 & (((uint32_t)s[39] >> 3) | ((uint32_t)s[40] << 5) | ((uint32_t)s[41] << 13)));
	s16 = (int64_t)(2097151 & ((uint32_t)s[42] | ((uint32_t)s[43] << 8) | ((uint32_t)s[44] << 16)));
	s17 = (int64_t)(2097151 & (((uint32_t)s[44] >> 5) | ((uint32_t)s[45] << 3) | ((uint32_t)s[46] << 11) | ((uint32_t)s[47] << 19)));
	s18 = (int64_t)(2097151 & (((uint32_t)s[47] >> 2) | ((uint32_t)s[48] << 6) | ((uint32_t)s[49] << 14)));
	s19 = (int64_t)(2097151 & (((uint32_t)s[49] >> 7) | ((uint32_t)s[50] << 1) | ((uint32_t)s[51] << 9) | ((uint32_t)s[52] << 17)));
	s20 = (int64_t)(2097151 & (((uint32_t)s[52] >> 4) | ((uint32_t)s[53] << 4) | ((uint32_t)s[54] << 12) | ((uint32_t)s[55] << 20)));
	s21 = (int64_t)(2097151 & (((uint32_t)s[55] >> 1) | ((uint32_t)s[56] << 7) | ((uint32_t)s[57] << 15)));
	s22 = (int64_t)(2097151 & (((uint32_t)s[57] >> 6) | ((uint32_t)s[58] << 2) | ((uint32_t)s[59] << 10) | ((uint32_t)s[60] << 18)));
	s23 = (int64_t)(((uint32_t)s[60] >> 3) | ((uint32_t)s[61] << 5) | ((uint32_t)s[62] << 13) | ((uint32_t)s[63] << 21));

	/*
	 * Reduce mod l by subtracting multiples of l from the top limbs.
	 * l = 2^252 + 27742317777372353535851937790883648493
	 * In 21-bit limbs, l[0..11] are the limb values of l.
	 *
	 * l in 21-bit limbs (each 2097151 max):
	 * l0=666643, l1=470296, l2=654183, l3=-997805, l4=136657, l5=-683901
	 * (from NaCl ref10)
	 */

	/* Reduce s23 */
	s11 += s23 * 666643;
	s12 += s23 * 470296;
	s13 += s23 * 654183;
	s14 -= s23 * 997805;
	s15 += s23 * 136657;
	s16 -= s23 * 683901;
	s23 = 0;

	s10 += s22 * 666643;
	s11 += s22 * 470296;
	s12 += s22 * 654183;
	s13 -= s22 * 997805;
	s14 += s22 * 136657;
	s15 -= s22 * 683901;
	s22 = 0;

	s9 += s21 * 666643;
	s10 += s21 * 470296;
	s11 += s21 * 654183;
	s12 -= s21 * 997805;
	s13 += s21 * 136657;
	s14 -= s21 * 683901;
	s21 = 0;

	s8 += s20 * 666643;
	s9 += s20 * 470296;
	s10 += s20 * 654183;
	s11 -= s20 * 997805;
	s12 += s20 * 136657;
	s13 -= s20 * 683901;
	s20 = 0;

	s7 += s19 * 666643;
	s8 += s19 * 470296;
	s9 += s19 * 654183;
	s10 -= s19 * 997805;
	s11 += s19 * 136657;
	s12 -= s19 * 683901;
	s19 = 0;

	s6 += s18 * 666643;
	s7 += s18 * 470296;
	s8 += s18 * 654183;
	s9 -= s18 * 997805;
	s10 += s18 * 136657;
	s11 -= s18 * 683901;
	s18 = 0;

	/* Carry */
	carry = (s6 + (1 << 20)) >> 21; s7 += carry; s6 -= carry << 21;
	carry = (s8 + (1 << 20)) >> 21; s9 += carry; s8 -= carry << 21;
	carry = (s10 + (1 << 20)) >> 21; s11 += carry; s10 -= carry << 21;
	carry = (s12 + (1 << 20)) >> 21; s13 += carry; s12 -= carry << 21;
	carry = (s14 + (1 << 20)) >> 21; s15 += carry; s14 -= carry << 21;
	carry = (s16 + (1 << 20)) >> 21; s17 += carry; s16 -= carry << 21;
	carry = (s7 + (1 << 20)) >> 21; s8 += carry; s7 -= carry << 21;
	carry = (s9 + (1 << 20)) >> 21; s10 += carry; s9 -= carry << 21;
	carry = (s11 + (1 << 20)) >> 21; s12 += carry; s11 -= carry << 21;
	carry = (s13 + (1 << 20)) >> 21; s14 += carry; s13 -= carry << 21;
	carry = (s15 + (1 << 20)) >> 21; s16 += carry; s15 -= carry << 21;

	/* Second reduction for s12..s17 */
	s5 += s17 * 666643;
	s6 += s17 * 470296;
	s7 += s17 * 654183;
	s8 -= s17 * 997805;
	s9 += s17 * 136657;
	s10 -= s17 * 683901;
	s17 = 0;

	s4 += s16 * 666643;
	s5 += s16 * 470296;
	s6 += s16 * 654183;
	s7 -= s16 * 997805;
	s8 += s16 * 136657;
	s9 -= s16 * 683901;
	s16 = 0;

	s3 += s15 * 666643;
	s4 += s15 * 470296;
	s5 += s15 * 654183;
	s6 -= s15 * 997805;
	s7 += s15 * 136657;
	s8 -= s15 * 683901;
	s15 = 0;

	s2 += s14 * 666643;
	s3 += s14 * 470296;
	s4 += s14 * 654183;
	s5 -= s14 * 997805;
	s6 += s14 * 136657;
	s7 -= s14 * 683901;
	s14 = 0;

	s1 += s13 * 666643;
	s2 += s13 * 470296;
	s3 += s13 * 654183;
	s4 -= s13 * 997805;
	s5 += s13 * 136657;
	s6 -= s13 * 683901;
	s13 = 0;

	s0 += s12 * 666643;
	s1 += s12 * 470296;
	s2 += s12 * 654183;
	s3 -= s12 * 997805;
	s4 += s12 * 136657;
	s5 -= s12 * 683901;
	s12 = 0;

	/* Centered carry, even pass then odd pass, before first fold */
	carry = (s0 + (1 << 20)) >> 21; s1 += carry; s0 -= carry << 21;
	carry = (s2 + (1 << 20)) >> 21; s3 += carry; s2 -= carry << 21;
	carry = (s4 + (1 << 20)) >> 21; s5 += carry; s4 -= carry << 21;
	carry = (s6 + (1 << 20)) >> 21; s7 += carry; s6 -= carry << 21;
	carry = (s8 + (1 << 20)) >> 21; s9 += carry; s8 -= carry << 21;
	carry = (s10 + (1 << 20)) >> 21; s11 += carry; s10 -= carry << 21;
	carry = (s1 + (1 << 20)) >> 21; s2 += carry; s1 -= carry << 21;
	carry = (s3 + (1 << 20)) >> 21; s4 += carry; s3 -= carry << 21;
	carry = (s5 + (1 << 20)) >> 21; s6 += carry; s5 -= carry << 21;
	carry = (s7 + (1 << 20)) >> 21; s8 += carry; s7 -= carry << 21;
	carry = (s9 + (1 << 20)) >> 21; s10 += carry; s9 -= carry << 21;
	carry = (s11 + (1 << 20)) >> 21; s12 += carry; s11 -= carry << 21;

	/* First fold of s12 */
	s0 += s12 * 666643;
	s1 += s12 * 470296;
	s2 += s12 * 654183;
	s3 -= s12 * 997805;
	s4 += s12 * 136657;
	s5 -= s12 * 683901;
	s12 = 0;

	/* Unsigned carry chain to canonicalise (may overflow into s12) */
	carry = s0 >> 21; s1 += carry; s0 -= carry << 21;
	carry = s1 >> 21; s2 += carry; s1 -= carry << 21;
	carry = s2 >> 21; s3 += carry; s2 -= carry << 21;
	carry = s3 >> 21; s4 += carry; s3 -= carry << 21;
	carry = s4 >> 21; s5 += carry; s4 -= carry << 21;
	carry = s5 >> 21; s6 += carry; s5 -= carry << 21;
	carry = s6 >> 21; s7 += carry; s6 -= carry << 21;
	carry = s7 >> 21; s8 += carry; s7 -= carry << 21;
	carry = s8 >> 21; s9 += carry; s8 -= carry << 21;
	carry = s9 >> 21; s10 += carry; s9 -= carry << 21;
	carry = s10 >> 21; s11 += carry; s10 -= carry << 21;
	carry = s11 >> 21; s12 += carry; s11 -= carry << 21;

	/* Second fold of s12 */
	s0 += s12 * 666643;
	s1 += s12 * 470296;
	s2 += s12 * 654183;
	s3 -= s12 * 997805;
	s4 += s12 * 136657;
	s5 -= s12 * 683901;

	/* Final unsigned carry chain (no s11 overflow possible now) */
	carry = s0 >> 21; s1 += carry; s0 -= carry << 21;
	carry = s1 >> 21; s2 += carry; s1 -= carry << 21;
	carry = s2 >> 21; s3 += carry; s2 -= carry << 21;
	carry = s3 >> 21; s4 += carry; s3 -= carry << 21;
	carry = s4 >> 21; s5 += carry; s4 -= carry << 21;
	carry = s5 >> 21; s6 += carry; s5 -= carry << 21;
	carry = s6 >> 21; s7 += carry; s6 -= carry << 21;
	carry = s7 >> 21; s8 += carry; s7 -= carry << 21;
	carry = s8 >> 21; s9 += carry; s8 -= carry << 21;
	carry = s9 >> 21; s10 += carry; s9 -= carry << 21;
	carry = s10 >> 21; s11 += carry; s10 -= carry << 21;

	/* Pack into bytes */
	out[0] = (uint8_t)(s0);
	out[1] = (uint8_t)(s0 >> 8);
	out[2] = (uint8_t)((s0 >> 16) | (s1 << 5));
	out[3] = (uint8_t)(s1 >> 3);
	out[4] = (uint8_t)(s1 >> 11);
	out[5] = (uint8_t)((s1 >> 19) | (s2 << 2));
	out[6] = (uint8_t)(s2 >> 6);
	out[7] = (uint8_t)((s2 >> 14) | (s3 << 7));
	out[8] = (uint8_t)(s3 >> 1);
	out[9] = (uint8_t)(s3 >> 9);
	out[10] = (uint8_t)((s3 >> 17) | (s4 << 4));
	out[11] = (uint8_t)(s4 >> 4);
	out[12] = (uint8_t)(s4 >> 12);
	out[13] = (uint8_t)((s4 >> 20) | (s5 << 1));
	out[14] = (uint8_t)(s5 >> 7);
	out[15] = (uint8_t)((s5 >> 15) | (s6 << 6));
	out[16] = (uint8_t)(s6 >> 2);
	out[17] = (uint8_t)(s6 >> 10);
	out[18] = (uint8_t)((s6 >> 18) | (s7 << 3));
	out[19] = (uint8_t)(s7 >> 5);
	out[20] = (uint8_t)(s7 >> 13);
	out[21] = (uint8_t)(s8);
	out[22] = (uint8_t)(s8 >> 8);
	out[23] = (uint8_t)((s8 >> 16) | (s9 << 5));
	out[24] = (uint8_t)(s9 >> 3);
	out[25] = (uint8_t)(s9 >> 11);
	out[26] = (uint8_t)((s9 >> 19) | (s10 << 2));
	out[27] = (uint8_t)(s10 >> 6);
	out[28] = (uint8_t)((s10 >> 14) | (s11 << 7));
	out[29] = (uint8_t)(s11 >> 1);
	out[30] = (uint8_t)(s11 >> 9);
	out[31] = (uint8_t)(s11 >> 17);
}

/*
 * Compute s = (a + b*c) mod l, where a and b are 32-byte scalars
 * and c is a 32-byte scalar. Used for S = r + H(R,A,M)*a.
 */
static void sc_muladd(uint8_t s[32], const uint8_t a[32],
		      const uint8_t b[32], const uint8_t c[32])
{
	/*
	 * s = c + a*b mod l
	 * (Note: in Ed25519 signing, s = r + h*a where r is nonce scalar,
	 *  h is hash, a is secret scalar.)
	 *
	 * We compute a*b as a 64-byte product, add c, then reduce mod l.
	 */
	int64_t a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11;
	int64_t b0, b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11;
	int64_t c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11;
	int64_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
	int64_t s12, s13, s14, s15, s16, s17, s18, s19, s20, s21, s22, s23;
	int64_t carry;

	a0 = 2097151 & ((uint32_t)a[0] | ((uint32_t)a[1] << 8) | ((uint32_t)a[2] << 16));
	a1 = 2097151 & (((uint32_t)a[2] >> 5) | ((uint32_t)a[3] << 3) | ((uint32_t)a[4] << 11) | ((uint32_t)a[5] << 19));
	a2 = 2097151 & (((uint32_t)a[5] >> 2) | ((uint32_t)a[6] << 6) | ((uint32_t)a[7] << 14));
	a3 = 2097151 & (((uint32_t)a[7] >> 7) | ((uint32_t)a[8] << 1) | ((uint32_t)a[9] << 9) | ((uint32_t)a[10] << 17));
	a4 = 2097151 & (((uint32_t)a[10] >> 4) | ((uint32_t)a[11] << 4) | ((uint32_t)a[12] << 12) | ((uint32_t)a[13] << 20));
	a5 = 2097151 & (((uint32_t)a[13] >> 1) | ((uint32_t)a[14] << 7) | ((uint32_t)a[15] << 15));
	a6 = 2097151 & (((uint32_t)a[15] >> 6) | ((uint32_t)a[16] << 2) | ((uint32_t)a[17] << 10) | ((uint32_t)a[18] << 18));
	a7 = 2097151 & (((uint32_t)a[18] >> 3) | ((uint32_t)a[19] << 5) | ((uint32_t)a[20] << 13));
	a8 = 2097151 & ((uint32_t)a[21] | ((uint32_t)a[22] << 8) | ((uint32_t)a[23] << 16));
	a9 = 2097151 & (((uint32_t)a[23] >> 5) | ((uint32_t)a[24] << 3) | ((uint32_t)a[25] << 11) | ((uint32_t)a[26] << 19));
	a10 = 2097151 & (((uint32_t)a[26] >> 2) | ((uint32_t)a[27] << 6) | ((uint32_t)a[28] << 14));
	a11 = (((uint32_t)a[28] >> 7) | ((uint32_t)a[29] << 1) | ((uint32_t)a[30] << 9) | ((uint32_t)a[31] << 17));

	b0 = 2097151 & ((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16));
	b1 = 2097151 & (((uint32_t)b[2] >> 5) | ((uint32_t)b[3] << 3) | ((uint32_t)b[4] << 11) | ((uint32_t)b[5] << 19));
	b2 = 2097151 & (((uint32_t)b[5] >> 2) | ((uint32_t)b[6] << 6) | ((uint32_t)b[7] << 14));
	b3 = 2097151 & (((uint32_t)b[7] >> 7) | ((uint32_t)b[8] << 1) | ((uint32_t)b[9] << 9) | ((uint32_t)b[10] << 17));
	b4 = 2097151 & (((uint32_t)b[10] >> 4) | ((uint32_t)b[11] << 4) | ((uint32_t)b[12] << 12) | ((uint32_t)b[13] << 20));
	b5 = 2097151 & (((uint32_t)b[13] >> 1) | ((uint32_t)b[14] << 7) | ((uint32_t)b[15] << 15));
	b6 = 2097151 & (((uint32_t)b[15] >> 6) | ((uint32_t)b[16] << 2) | ((uint32_t)b[17] << 10) | ((uint32_t)b[18] << 18));
	b7 = 2097151 & (((uint32_t)b[18] >> 3) | ((uint32_t)b[19] << 5) | ((uint32_t)b[20] << 13));
	b8 = 2097151 & ((uint32_t)b[21] | ((uint32_t)b[22] << 8) | ((uint32_t)b[23] << 16));
	b9 = 2097151 & (((uint32_t)b[23] >> 5) | ((uint32_t)b[24] << 3) | ((uint32_t)b[25] << 11) | ((uint32_t)b[26] << 19));
	b10 = 2097151 & (((uint32_t)b[26] >> 2) | ((uint32_t)b[27] << 6) | ((uint32_t)b[28] << 14));
	b11 = (((uint32_t)b[28] >> 7) | ((uint32_t)b[29] << 1) | ((uint32_t)b[30] << 9) | ((uint32_t)b[31] << 17));

	c0 = 2097151 & ((uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16));
	c1 = 2097151 & (((uint32_t)c[2] >> 5) | ((uint32_t)c[3] << 3) | ((uint32_t)c[4] << 11) | ((uint32_t)c[5] << 19));
	c2 = 2097151 & (((uint32_t)c[5] >> 2) | ((uint32_t)c[6] << 6) | ((uint32_t)c[7] << 14));
	c3 = 2097151 & (((uint32_t)c[7] >> 7) | ((uint32_t)c[8] << 1) | ((uint32_t)c[9] << 9) | ((uint32_t)c[10] << 17));
	c4 = 2097151 & (((uint32_t)c[10] >> 4) | ((uint32_t)c[11] << 4) | ((uint32_t)c[12] << 12) | ((uint32_t)c[13] << 20));
	c5 = 2097151 & (((uint32_t)c[13] >> 1) | ((uint32_t)c[14] << 7) | ((uint32_t)c[15] << 15));
	c6 = 2097151 & (((uint32_t)c[15] >> 6) | ((uint32_t)c[16] << 2) | ((uint32_t)c[17] << 10) | ((uint32_t)c[18] << 18));
	c7 = 2097151 & (((uint32_t)c[18] >> 3) | ((uint32_t)c[19] << 5) | ((uint32_t)c[20] << 13));
	c8 = 2097151 & ((uint32_t)c[21] | ((uint32_t)c[22] << 8) | ((uint32_t)c[23] << 16));
	c9 = 2097151 & (((uint32_t)c[23] >> 5) | ((uint32_t)c[24] << 3) | ((uint32_t)c[25] << 11) | ((uint32_t)c[26] << 19));
	c10 = 2097151 & (((uint32_t)c[26] >> 2) | ((uint32_t)c[27] << 6) | ((uint32_t)c[28] << 14));
	c11 = (((uint32_t)c[28] >> 7) | ((uint32_t)c[29] << 1) | ((uint32_t)c[30] << 9) | ((uint32_t)c[31] << 17));

	/* s = c + a*b */
	s0 = c0 + a0*b0;
	s1 = c1 + a0*b1 + a1*b0;
	s2 = c2 + a0*b2 + a1*b1 + a2*b0;
	s3 = c3 + a0*b3 + a1*b2 + a2*b1 + a3*b0;
	s4 = c4 + a0*b4 + a1*b3 + a2*b2 + a3*b1 + a4*b0;
	s5 = c5 + a0*b5 + a1*b4 + a2*b3 + a3*b2 + a4*b1 + a5*b0;
	s6 = c6 + a0*b6 + a1*b5 + a2*b4 + a3*b3 + a4*b2 + a5*b1 + a6*b0;
	s7 = c7 + a0*b7 + a1*b6 + a2*b5 + a3*b4 + a4*b3 + a5*b2 + a6*b1 + a7*b0;
	s8 = c8 + a0*b8 + a1*b7 + a2*b6 + a3*b5 + a4*b4 + a5*b3 + a6*b2 + a7*b1 + a8*b0;
	s9 = c9 + a0*b9 + a1*b8 + a2*b7 + a3*b6 + a4*b5 + a5*b4 + a6*b3 + a7*b2 + a8*b1 + a9*b0;
	s10 = c10 + a0*b10 + a1*b9 + a2*b8 + a3*b7 + a4*b6 + a5*b5 + a6*b4 + a7*b3 + a8*b2 + a9*b1 + a10*b0;
	s11 = c11 + a0*b11 + a1*b10 + a2*b9 + a3*b8 + a4*b7 + a5*b6 + a6*b5 + a7*b4 + a8*b3 + a9*b2 + a10*b1 + a11*b0;
	s12 = a1*b11 + a2*b10 + a3*b9 + a4*b8 + a5*b7 + a6*b6 + a7*b5 + a8*b4 + a9*b3 + a10*b2 + a11*b1;
	s13 = a2*b11 + a3*b10 + a4*b9 + a5*b8 + a6*b7 + a7*b6 + a8*b5 + a9*b4 + a10*b3 + a11*b2;
	s14 = a3*b11 + a4*b10 + a5*b9 + a6*b8 + a7*b7 + a8*b6 + a9*b5 + a10*b4 + a11*b3;
	s15 = a4*b11 + a5*b10 + a6*b9 + a7*b8 + a8*b7 + a9*b6 + a10*b5 + a11*b4;
	s16 = a5*b11 + a6*b10 + a7*b9 + a8*b8 + a9*b7 + a10*b6 + a11*b5;
	s17 = a6*b11 + a7*b10 + a8*b9 + a9*b8 + a10*b7 + a11*b6;
	s18 = a7*b11 + a8*b10 + a9*b9 + a10*b8 + a11*b7;
	s19 = a8*b11 + a9*b10 + a10*b9 + a11*b8;
	s20 = a9*b11 + a10*b10 + a11*b9;
	s21 = a10*b11 + a11*b10;
	s22 = a11*b11;
	s23 = 0;

	/*
	 * Normalize limbs to 21-bit range before reducing.
	 * Schoolbook multiply produces limbs up to ~12 * 2^42 ~ 2^46 wide;
	 * without this carry chain, subsequent s_i * 683901 can overflow int64.
	 */
	carry = (s0 + (1 << 20)) >> 21; s1 += carry; s0 -= carry << 21;
	carry = (s2 + (1 << 20)) >> 21; s3 += carry; s2 -= carry << 21;
	carry = (s4 + (1 << 20)) >> 21; s5 += carry; s4 -= carry << 21;
	carry = (s6 + (1 << 20)) >> 21; s7 += carry; s6 -= carry << 21;
	carry = (s8 + (1 << 20)) >> 21; s9 += carry; s8 -= carry << 21;
	carry = (s10 + (1 << 20)) >> 21; s11 += carry; s10 -= carry << 21;
	carry = (s12 + (1 << 20)) >> 21; s13 += carry; s12 -= carry << 21;
	carry = (s14 + (1 << 20)) >> 21; s15 += carry; s14 -= carry << 21;
	carry = (s16 + (1 << 20)) >> 21; s17 += carry; s16 -= carry << 21;
	carry = (s18 + (1 << 20)) >> 21; s19 += carry; s18 -= carry << 21;
	carry = (s20 + (1 << 20)) >> 21; s21 += carry; s20 -= carry << 21;
	carry = (s22 + (1 << 20)) >> 21; s23 += carry; s22 -= carry << 21;

	carry = (s1 + (1 << 20)) >> 21; s2 += carry; s1 -= carry << 21;
	carry = (s3 + (1 << 20)) >> 21; s4 += carry; s3 -= carry << 21;
	carry = (s5 + (1 << 20)) >> 21; s6 += carry; s5 -= carry << 21;
	carry = (s7 + (1 << 20)) >> 21; s8 += carry; s7 -= carry << 21;
	carry = (s9 + (1 << 20)) >> 21; s10 += carry; s9 -= carry << 21;
	carry = (s11 + (1 << 20)) >> 21; s12 += carry; s11 -= carry << 21;
	carry = (s13 + (1 << 20)) >> 21; s14 += carry; s13 -= carry << 21;
	carry = (s15 + (1 << 20)) >> 21; s16 += carry; s15 -= carry << 21;
	carry = (s17 + (1 << 20)) >> 21; s18 += carry; s17 -= carry << 21;
	carry = (s19 + (1 << 20)) >> 21; s20 += carry; s19 -= carry << 21;
	carry = (s21 + (1 << 20)) >> 21; s22 += carry; s21 -= carry << 21;

	/* Reduce from the top */
	s11 += s23 * 666643; s12 += s23 * 470296; s13 += s23 * 654183;
	s14 -= s23 * 997805; s15 += s23 * 136657; s16 -= s23 * 683901; s23 = 0;

	s10 += s22 * 666643; s11 += s22 * 470296; s12 += s22 * 654183;
	s13 -= s22 * 997805; s14 += s22 * 136657; s15 -= s22 * 683901; s22 = 0;

	s9 += s21 * 666643; s10 += s21 * 470296; s11 += s21 * 654183;
	s12 -= s21 * 997805; s13 += s21 * 136657; s14 -= s21 * 683901; s21 = 0;

	s8 += s20 * 666643; s9 += s20 * 470296; s10 += s20 * 654183;
	s11 -= s20 * 997805; s12 += s20 * 136657; s13 -= s20 * 683901; s20 = 0;

	s7 += s19 * 666643; s8 += s19 * 470296; s9 += s19 * 654183;
	s10 -= s19 * 997805; s11 += s19 * 136657; s12 -= s19 * 683901; s19 = 0;

	s6 += s18 * 666643; s7 += s18 * 470296; s8 += s18 * 654183;
	s9 -= s18 * 997805; s10 += s18 * 136657; s11 -= s18 * 683901; s18 = 0;

	carry = (s6 + (1 << 20)) >> 21; s7 += carry; s6 -= carry << 21;
	carry = (s8 + (1 << 20)) >> 21; s9 += carry; s8 -= carry << 21;
	carry = (s10 + (1 << 20)) >> 21; s11 += carry; s10 -= carry << 21;
	carry = (s12 + (1 << 20)) >> 21; s13 += carry; s12 -= carry << 21;
	carry = (s14 + (1 << 20)) >> 21; s15 += carry; s14 -= carry << 21;
	carry = (s16 + (1 << 20)) >> 21; s17 += carry; s16 -= carry << 21;
	carry = (s7 + (1 << 20)) >> 21; s8 += carry; s7 -= carry << 21;
	carry = (s9 + (1 << 20)) >> 21; s10 += carry; s9 -= carry << 21;
	carry = (s11 + (1 << 20)) >> 21; s12 += carry; s11 -= carry << 21;
	carry = (s13 + (1 << 20)) >> 21; s14 += carry; s13 -= carry << 21;
	carry = (s15 + (1 << 20)) >> 21; s16 += carry; s15 -= carry << 21;

	s5 += s17 * 666643; s6 += s17 * 470296; s7 += s17 * 654183;
	s8 -= s17 * 997805; s9 += s17 * 136657; s10 -= s17 * 683901; s17 = 0;

	s4 += s16 * 666643; s5 += s16 * 470296; s6 += s16 * 654183;
	s7 -= s16 * 997805; s8 += s16 * 136657; s9 -= s16 * 683901; s16 = 0;

	s3 += s15 * 666643; s4 += s15 * 470296; s5 += s15 * 654183;
	s6 -= s15 * 997805; s7 += s15 * 136657; s8 -= s15 * 683901; s15 = 0;

	s2 += s14 * 666643; s3 += s14 * 470296; s4 += s14 * 654183;
	s5 -= s14 * 997805; s6 += s14 * 136657; s7 -= s14 * 683901; s14 = 0;

	s1 += s13 * 666643; s2 += s13 * 470296; s3 += s13 * 654183;
	s4 -= s13 * 997805; s5 += s13 * 136657; s6 -= s13 * 683901; s13 = 0;

	s0 += s12 * 666643; s1 += s12 * 470296; s2 += s12 * 654183;
	s3 -= s12 * 997805; s4 += s12 * 136657; s5 -= s12 * 683901; s12 = 0;

	carry = (s0 + (1 << 20)) >> 21; s1 += carry; s0 -= carry << 21;
	carry = (s2 + (1 << 20)) >> 21; s3 += carry; s2 -= carry << 21;
	carry = (s4 + (1 << 20)) >> 21; s5 += carry; s4 -= carry << 21;
	carry = (s6 + (1 << 20)) >> 21; s7 += carry; s6 -= carry << 21;
	carry = (s8 + (1 << 20)) >> 21; s9 += carry; s8 -= carry << 21;
	carry = (s10 + (1 << 20)) >> 21; s11 += carry; s10 -= carry << 21;
	carry = (s1 + (1 << 20)) >> 21; s2 += carry; s1 -= carry << 21;
	carry = (s3 + (1 << 20)) >> 21; s4 += carry; s3 -= carry << 21;
	carry = (s5 + (1 << 20)) >> 21; s6 += carry; s5 -= carry << 21;
	carry = (s7 + (1 << 20)) >> 21; s8 += carry; s7 -= carry << 21;
	carry = (s9 + (1 << 20)) >> 21; s10 += carry; s9 -= carry << 21;
	carry = (s11 + (1 << 20)) >> 21; s12 += carry; s11 -= carry << 21;

	s0 += s12 * 666643; s1 += s12 * 470296; s2 += s12 * 654183;
	s3 -= s12 * 997805; s4 += s12 * 136657; s5 -= s12 * 683901; s12 = 0;

	carry = s0 >> 21; s1 += carry; s0 -= carry << 21;
	carry = s1 >> 21; s2 += carry; s1 -= carry << 21;
	carry = s2 >> 21; s3 += carry; s2 -= carry << 21;
	carry = s3 >> 21; s4 += carry; s3 -= carry << 21;
	carry = s4 >> 21; s5 += carry; s4 -= carry << 21;
	carry = s5 >> 21; s6 += carry; s5 -= carry << 21;
	carry = s6 >> 21; s7 += carry; s6 -= carry << 21;
	carry = s7 >> 21; s8 += carry; s7 -= carry << 21;
	carry = s8 >> 21; s9 += carry; s8 -= carry << 21;
	carry = s9 >> 21; s10 += carry; s9 -= carry << 21;
	carry = s10 >> 21; s11 += carry; s10 -= carry << 21;
	carry = s11 >> 21; s12 += carry; s11 -= carry << 21;

	s0 += s12 * 666643; s1 += s12 * 470296; s2 += s12 * 654183;
	s3 -= s12 * 997805; s4 += s12 * 136657; s5 -= s12 * 683901;

	carry = s0 >> 21; s1 += carry; s0 -= carry << 21;
	carry = s1 >> 21; s2 += carry; s1 -= carry << 21;
	carry = s2 >> 21; s3 += carry; s2 -= carry << 21;
	carry = s3 >> 21; s4 += carry; s3 -= carry << 21;
	carry = s4 >> 21; s5 += carry; s4 -= carry << 21;
	carry = s5 >> 21; s6 += carry; s5 -= carry << 21;
	carry = s6 >> 21; s7 += carry; s6 -= carry << 21;
	carry = s7 >> 21; s8 += carry; s7 -= carry << 21;
	carry = s8 >> 21; s9 += carry; s8 -= carry << 21;
	carry = s9 >> 21; s10 += carry; s9 -= carry << 21;
	carry = s10 >> 21; s11 += carry; s10 -= carry << 21;

	s[0] = (uint8_t)(s0); s[1] = (uint8_t)(s0 >> 8);
	s[2] = (uint8_t)((s0 >> 16) | (s1 << 5)); s[3] = (uint8_t)(s1 >> 3);
	s[4] = (uint8_t)(s1 >> 11); s[5] = (uint8_t)((s1 >> 19) | (s2 << 2));
	s[6] = (uint8_t)(s2 >> 6); s[7] = (uint8_t)((s2 >> 14) | (s3 << 7));
	s[8] = (uint8_t)(s3 >> 1); s[9] = (uint8_t)(s3 >> 9);
	s[10] = (uint8_t)((s3 >> 17) | (s4 << 4)); s[11] = (uint8_t)(s4 >> 4);
	s[12] = (uint8_t)(s4 >> 12); s[13] = (uint8_t)((s4 >> 20) | (s5 << 1));
	s[14] = (uint8_t)(s5 >> 7); s[15] = (uint8_t)((s5 >> 15) | (s6 << 6));
	s[16] = (uint8_t)(s6 >> 2); s[17] = (uint8_t)(s6 >> 10);
	s[18] = (uint8_t)((s6 >> 18) | (s7 << 3)); s[19] = (uint8_t)(s7 >> 5);
	s[20] = (uint8_t)(s7 >> 13); s[21] = (uint8_t)(s8);
	s[22] = (uint8_t)(s8 >> 8); s[23] = (uint8_t)((s8 >> 16) | (s9 << 5));
	s[24] = (uint8_t)(s9 >> 3); s[25] = (uint8_t)(s9 >> 11);
	s[26] = (uint8_t)((s9 >> 19) | (s10 << 2)); s[27] = (uint8_t)(s10 >> 6);
	s[28] = (uint8_t)((s10 >> 14) | (s11 << 7)); s[29] = (uint8_t)(s11 >> 1);
	s[30] = (uint8_t)(s11 >> 9); s[31] = (uint8_t)(s11 >> 17);
}

/* --- Ed25519 public API --- */

void anx_ed25519_keypair(uint8_t pub[32], uint8_t priv[64],
			 const uint8_t seed[32])
{
	uint8_t hash[64];
	struct ge25519 A;

	/* Hash the seed to get the secret scalar and prefix */
	anx_sha512(seed, 32, hash);

	/* Clamp the scalar (first 32 bytes of hash) */
	hash[0] &= 248;
	hash[31] &= 127;
	hash[31] |= 64;

	/* Compute A = scalar * B */
	struct ge25519 B;
	if (ge_frombytes(&B, B_y) != 0) {
		/* Basepoint is hardcoded, this should never fail */
		anx_memset(pub, 0, 32);
		anx_memset(priv, 0, 64);
		return;
	}
	ge_scalarmult(&A, hash, &B);
	ge_tobytes(pub, &A);

	/* Private key = seed || public key */
	anx_memcpy(priv, seed, 32);
	anx_memcpy(priv + 32, pub, 32);

	anx_memset(hash, 0, sizeof(hash));
}

void anx_ed25519_sign(uint8_t sig[64], const void *msg, uint32_t len,
		      const uint8_t priv[64])
{
	uint8_t hash[64];
	uint8_t nonce[64];
	uint8_t hram[64];
	struct ge25519 R, B;
	uint8_t r_scalar[32];
	struct anx_sha512_ctx ctx;

	/* Derive secret scalar and nonce prefix from seed */
	anx_sha512(priv, 32, hash);
	hash[0] &= 248;
	hash[31] &= 127;
	hash[31] |= 64;

	/* r = SHA-512(prefix || msg) mod l */
	anx_sha512_init(&ctx);
	anx_sha512_update(&ctx, hash + 32, 32); /* nonce prefix */
	anx_sha512_update(&ctx, msg, len);
	anx_sha512_final(&ctx, nonce);
	sc_reduce(r_scalar, nonce);

	/* R = r * B */
	if (ge_frombytes(&B, B_y) != 0) {
		anx_memset(sig, 0, 64);
		return;
	}
	ge_scalarmult(&R, r_scalar, &B);
	ge_tobytes(sig, &R); /* First 32 bytes of signature = R */

	/* S = r + SHA-512(R || A || msg) * a mod l */
	anx_sha512_init(&ctx);
	anx_sha512_update(&ctx, sig, 32);        /* R */
	anx_sha512_update(&ctx, priv + 32, 32);  /* A (public key) */
	anx_sha512_update(&ctx, msg, len);
	anx_sha512_final(&ctx, hram);

	uint8_t hram_reduced[32];
	sc_reduce(hram_reduced, hram);

	/* sig[32..63] = (r + hram * a) mod l */
	sc_muladd(sig + 32, hram_reduced, hash, r_scalar);

	anx_memset(hash, 0, sizeof(hash));
	anx_memset(nonce, 0, sizeof(nonce));
	anx_memset(r_scalar, 0, sizeof(r_scalar));
}

int anx_ed25519_verify(const uint8_t sig[64], const void *msg,
		       uint32_t len, const uint8_t pub[32])
{
	struct ge25519 A, R, check_point;
	uint8_t hram[64];
	uint8_t hram_reduced[32];
	uint8_t R_bytes[32];
	struct anx_sha512_ctx ctx;
	struct ge25519 B;
	struct ge25519 sB, hA;

	/* Decode public key A */
	if (ge_frombytes(&A, pub) != 0)
		return -1;

	/* Decode R from signature */
	if (ge_frombytes(&R, sig) != 0)
		return -1;

	/* Compute h = SHA-512(R || A || msg) mod l */
	anx_sha512_init(&ctx);
	anx_sha512_update(&ctx, sig, 32);
	anx_sha512_update(&ctx, pub, 32);
	anx_sha512_update(&ctx, msg, len);
	anx_sha512_final(&ctx, hram);
	sc_reduce(hram_reduced, hram);

	/* Check: S*B = R + h*A
	 * Equivalently: S*B - h*A = R
	 */
	if (ge_frombytes(&B, B_y) != 0)
		return -1;

	/* sB = S * B */
	ge_scalarmult(&sB, sig + 32, &B);

	/* hA = h * A */
	ge_scalarmult(&hA, hram_reduced, &A);

	/* Negate hA: negate the X and T coordinates */
	fe_neg(hA.X, hA.X);
	fe_neg(hA.T, hA.T);

	/* check_point = sB + (-hA) = sB - hA, should equal R */
	ge_add(&check_point, &sB, &hA);

	/* Encode check_point and compare with R */
	ge_tobytes(R_bytes, &check_point);

	/* Constant-time compare with sig[0..31] */
	uint8_t diff = 0;
	uint32_t i;
	for (i = 0; i < 32; i++)
		diff |= R_bytes[i] ^ sig[i];

	return (diff == 0) ? 0 : -1;
}
