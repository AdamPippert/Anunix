/*
 * curve25519.c — Curve25519 Diffie-Hellman.
 *
 * Montgomery ladder on Curve25519 (y^2 = x^3 + 486662x^2 + x mod p).
 * Constant-time implementation suitable for key agreement.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/crypto.h>
#include "fe25519.h"

/* The Curve25519 basepoint (u = 9) */
static const uint8_t basepoint[32] = { 9 };

void anx_curve25519(uint8_t out[32], const uint8_t scalar[32],
		    const uint8_t point[32])
{
	uint8_t e[32];
	fe25519 x1, x2, z2, x3, z3, tmp0, tmp1;
	int pos;
	uint64_t swap, b;

	/* Clamp the scalar per RFC 7748 */
	anx_memcpy(e, scalar, 32);
	e[0] &= 248;
	e[31] &= 127;
	e[31] |= 64;

	fe_frombytes(x1, point);
	fe_1(x2);
	fe_0(z2);
	fe_copy(x3, x1);
	fe_1(z3);

	swap = 0;

	/* Montgomery ladder: iterate from bit 254 down to 0 */
	for (pos = 254; pos >= 0; pos--) {
		b = (uint64_t)(e[pos / 8] >> (pos & 7)) & 1;
		swap ^= b;
		fe_cswap(x2, x3, swap);
		fe_cswap(z2, z3, swap);
		swap = b;

		fe_sub(tmp0, x3, z3);
		fe_sub(tmp1, x2, z2);
		fe_add(x2, x2, z2);
		fe_add(z2, x3, z3);
		fe_mul(z3, tmp0, x2);
		fe_mul(z2, z2, tmp1);
		fe_sq(tmp0, tmp1);
		fe_sq(tmp1, x2);
		fe_add(x3, z3, z2);
		fe_sub(z2, z3, z2);
		fe_mul(x2, tmp1, tmp0);
		fe_sub(tmp1, tmp1, tmp0);
		fe_sq(z2, z2);
		fe_mul121666(z3, tmp1);
		fe_sq(x3, x3);
		fe_add(tmp0, tmp0, z3);
		fe_mul(z3, x1, z2);
		fe_mul(z2, tmp1, tmp0);
	}

	fe_cswap(x2, x3, swap);
	fe_cswap(z2, z3, swap);

	/* Result = x2 / z2 */
	fe_invert(z2, z2);
	fe_mul(x2, x2, z2);
	fe_tobytes(out, x2);

	/* Wipe temporaries */
	anx_memset(e, 0, sizeof(e));
}

void anx_curve25519_base(uint8_t pub[32], const uint8_t priv[32])
{
	anx_curve25519(pub, priv, basepoint);
}
