/*
 * poly1305.c — Poly1305 one-time authenticator.
 *
 * Uses 130-bit arithmetic with uint64_t accumulators.
 * Reference: RFC 7539, D.J. Bernstein's poly1305-donna.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/crypto.h>

static uint32_t load_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/*
 * Accumulator and key are represented as five 26-bit limbs.
 * r is clamped per the spec. Arithmetic mod 2^130-5.
 */

void anx_poly1305(const uint8_t key[32], const void *msg, uint32_t len,
		  uint8_t tag[16])
{
	const uint8_t *m = (const uint8_t *)msg;

	/* Clamp r */
	uint32_t r0 = load_le32(key + 0) & 0x03ffffff;
	uint32_t r1 = (load_le32(key + 3) >> 2) & 0x03ffff03;
	uint32_t r2 = (load_le32(key + 6) >> 4) & 0x03ffc0ff;
	uint32_t r3 = (load_le32(key + 9) >> 6) & 0x03f03fff;
	uint32_t r4 = (load_le32(key + 12) >> 8) & 0x000fffff;

	/* Precompute r * 5 for modular reduction */
	uint32_t s1 = r1 * 5;
	uint32_t s2 = r2 * 5;
	uint32_t s3 = r3 * 5;
	uint32_t s4 = r4 * 5;

	/* Accumulator */
	uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;

	uint64_t d0, d1, d2, d3, d4;
	uint32_t c;

	while (len > 0) {
		uint8_t block[17];
		uint32_t blen = len < 16 ? len : 16;

		anx_memset(block, 0, sizeof(block));
		anx_memcpy(block, m, blen);
		block[blen] = 1; /* hibit */

		/* Add message block to accumulator */
		h0 += load_le32(block + 0) & 0x03ffffff;
		h1 += (load_le32(block + 3) >> 2) & 0x03ffffff;
		h2 += (load_le32(block + 6) >> 4) & 0x03ffffff;
		h3 += (load_le32(block + 9) >> 6) & 0x03ffffff;
		h4 += (load_le32(block + 12) >> 8);
		if (blen == 16)
			h4 |= (1 << 24); /* already set via block[16]=1 path */

		/* h *= r mod 2^130-5 */
		d0 = ((uint64_t)h0 * r0) + ((uint64_t)h1 * s4) +
		     ((uint64_t)h2 * s3) + ((uint64_t)h3 * s2) +
		     ((uint64_t)h4 * s1);
		d1 = ((uint64_t)h0 * r1) + ((uint64_t)h1 * r0) +
		     ((uint64_t)h2 * s4) + ((uint64_t)h3 * s3) +
		     ((uint64_t)h4 * s2);
		d2 = ((uint64_t)h0 * r2) + ((uint64_t)h1 * r1) +
		     ((uint64_t)h2 * r0) + ((uint64_t)h3 * s4) +
		     ((uint64_t)h4 * s3);
		d3 = ((uint64_t)h0 * r3) + ((uint64_t)h1 * r2) +
		     ((uint64_t)h2 * r1) + ((uint64_t)h3 * r0) +
		     ((uint64_t)h4 * s4);
		d4 = ((uint64_t)h0 * r4) + ((uint64_t)h1 * r3) +
		     ((uint64_t)h2 * r2) + ((uint64_t)h3 * r1) +
		     ((uint64_t)h4 * r0);

		/* Partial reduction */
		c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x03ffffff; d1 += c;
		c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x03ffffff; d2 += c;
		c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x03ffffff; d3 += c;
		c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x03ffffff; d4 += c;
		c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x03ffffff;
		h0 += c * 5;
		c = h0 >> 26; h0 &= 0x03ffffff; h1 += c;

		m += blen;
		len -= blen;
	}

	/* Final reduction: fully carry h so that h < 2^130 */
	c = h1 >> 26; h1 &= 0x03ffffff; h2 += c;
	c = h2 >> 26; h2 &= 0x03ffffff; h3 += c;
	c = h3 >> 26; h3 &= 0x03ffffff; h4 += c;
	c = h4 >> 26; h4 &= 0x03ffffff; h0 += c * 5;
	c = h0 >> 26; h0 &= 0x03ffffff; h1 += c;

	/* Compute h - p = h - (2^130 - 5) */
	uint32_t g0, g1, g2, g3, g4;
	uint32_t mask;

	g0 = h0 + 5; c = g0 >> 26; g0 &= 0x03ffffff;
	g1 = h1 + c; c = g1 >> 26; g1 &= 0x03ffffff;
	g2 = h2 + c; c = g2 >> 26; g2 &= 0x03ffffff;
	g3 = h3 + c; c = g3 >> 26; g3 &= 0x03ffffff;
	g4 = h4 + c - (1 << 26);

	/* Select h or g based on whether g underflowed */
	mask = (g4 >> 31) - 1; /* 0xffffffff if no underflow, 0 if underflow */
	g0 &= mask;
	g1 &= mask;
	g2 &= mask;
	g3 &= mask;
	g4 &= mask;
	mask = ~mask;
	h0 = (h0 & mask) | g0;
	h1 = (h1 & mask) | g1;
	h2 = (h2 & mask) | g2;
	h3 = (h3 & mask) | g3;
	h4 = (h4 & mask) | g4;

	/* Assemble h into 4 x 32-bit */
	uint64_t f0, f1, f2, f3;

	f0 = h0 | ((uint64_t)h1 << 26);
	f1 = (h1 >> 6) | ((uint64_t)h2 << 20);
	f2 = (h2 >> 12) | ((uint64_t)h3 << 14);
	f3 = (h3 >> 18) | ((uint64_t)h4 << 8);

	/* Add s (second half of key) */
	f0 += load_le32(key + 16);
	f1 += load_le32(key + 20) + (f0 >> 32);
	f2 += load_le32(key + 24) + (f1 >> 32);
	f3 += load_le32(key + 28) + (f2 >> 32);

	store_le32(tag + 0, (uint32_t)f0);
	store_le32(tag + 4, (uint32_t)f1);
	store_le32(tag + 8, (uint32_t)f2);
	store_le32(tag + 12, (uint32_t)f3);
}

int anx_poly1305_verify(const uint8_t key[32], const void *msg,
			uint32_t len, const uint8_t tag[16])
{
	uint8_t computed[16];
	uint8_t diff;
	uint32_t i;

	anx_poly1305(key, msg, len, computed);

	/* Constant-time comparison */
	diff = 0;
	for (i = 0; i < 16; i++)
		diff |= computed[i] ^ tag[i];

	anx_memset(computed, 0, sizeof(computed));

	/* Return 0 on match, -1 on mismatch */
	return (diff == 0) ? 0 : -1;
}
