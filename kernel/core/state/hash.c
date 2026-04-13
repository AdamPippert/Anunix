/*
 * hash.c — SHA-256 implementation for content hashing.
 *
 * Minimal, correct implementation. Based on the FIPS 180-4 specification.
 */

#include <anx/types.h>
#include <anx/string.h>

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROTR(x, n)	(((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)	(((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)	(((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)		(ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x)		(ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x)		(ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x)		(ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
	uint32_t W[64], a, b, c, d, e, f, g, h, t1, t2;

	for (int i = 0; i < 16; i++)
		W[i] = ((uint32_t)block[i * 4] << 24) |
		       ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8) |
		       ((uint32_t)block[i * 4 + 3]);

	for (int i = 16; i < 64; i++)
		W[i] = SIG1(W[i - 2]) + W[i - 7] + SIG0(W[i - 15]) + W[i - 16];

	a = state[0]; b = state[1]; c = state[2]; d = state[3];
	e = state[4]; f = state[5]; g = state[6]; h = state[7];

	for (int i = 0; i < 64; i++) {
		t1 = h + EP1(e) + CH(e, f, g) + K[i] + W[i];
		t2 = EP0(a) + MAJ(a, b, c);
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void anx_sha256(const void *data, uint64_t len, struct anx_hash *out)
{
	uint32_t state[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	const uint8_t *p = data;
	uint64_t remaining = len;

	/* Process complete 64-byte blocks */
	while (remaining >= 64) {
		sha256_transform(state, p);
		p += 64;
		remaining -= 64;
	}

	/* Final block with padding */
	uint8_t block[64];
	anx_memset(block, 0, 64);
	anx_memcpy(block, p, remaining);
	block[remaining] = 0x80;

	if (remaining >= 56) {
		sha256_transform(state, block);
		anx_memset(block, 0, 64);
	}

	/* Append length in bits (big-endian) */
	uint64_t bits = len * 8;
	block[56] = bits >> 56;
	block[57] = bits >> 48;
	block[58] = bits >> 40;
	block[59] = bits >> 32;
	block[60] = bits >> 24;
	block[61] = bits >> 16;
	block[62] = bits >> 8;
	block[63] = bits;

	sha256_transform(state, block);

	/* Write output in big-endian */
	for (int i = 0; i < 8; i++) {
		out->bytes[i * 4]     = state[i] >> 24;
		out->bytes[i * 4 + 1] = state[i] >> 16;
		out->bytes[i * 4 + 2] = state[i] >> 8;
		out->bytes[i * 4 + 3] = state[i];
	}
}
