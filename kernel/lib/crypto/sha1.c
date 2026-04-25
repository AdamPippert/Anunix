/*
 * sha1.c — SHA-1 digest (RFC 3174).
 *
 * Minimal implementation for WebSocket Sec-WebSocket-Accept computation.
 * Not suitable for security-critical uses; SHA-256 is preferred for those.
 */

#include "sha1.h"
#include <anx/string.h>

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

#define SHA1_BLOCK_LEN 64

/* Write a 32-bit big-endian word */
static void be32(uint32_t v, uint8_t *p)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >>  8);
	p[3] = (uint8_t)(v);
}

/* Read a 32-bit big-endian word */
static uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
	     | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static void sha1_block(uint32_t h[5], const uint8_t blk[SHA1_BLOCK_LEN])
{
	uint32_t w[80];
	uint32_t a, b, c, d, e, f, k, tmp;
	int t;

	for (t = 0; t < 16; t++)
		w[t] = rd32(blk + t * 4);
	for (t = 16; t < 80; t++)
		w[t] = ROTL32(w[t-3] ^ w[t-8] ^ w[t-14] ^ w[t-16], 1);

	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];

	for (t = 0; t < 80; t++) {
		if (t < 20) {
			f = (b & c) | (~b & d);
			k = 0x5A827999U;
		} else if (t < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1U;
		} else if (t < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDCU;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6U;
		}
		tmp = ROTL32(a, 5) + f + e + k + w[t];
		e = d; d = c; c = ROTL32(b, 30); b = a; a = tmp;
	}

	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void anx_sha1(const uint8_t *data, size_t len, uint8_t out[SHA1_DIGEST_LEN])
{
	uint32_t h[5] = {
		0x67452301U, 0xEFCDAB89U, 0x98BADCFEU,
		0x10325476U, 0xC3D2E1F0U,
	};
	uint8_t  block[SHA1_BLOCK_LEN];
	size_t   processed = 0;
	uint64_t bit_len = (uint64_t)len * 8;
	int      i;

	/* Process complete blocks */
	while (processed + SHA1_BLOCK_LEN <= len) {
		sha1_block(h, data + processed);
		processed += SHA1_BLOCK_LEN;
	}

	/* Final block(s): copy remainder, append 0x80, zero-pad, append length */
	size_t tail = len - processed;
	anx_memcpy(block, data + processed, tail);
	block[tail++] = 0x80;

	if (tail > 56) {
		/* Need an extra block */
		anx_memset(block + tail, 0, SHA1_BLOCK_LEN - tail);
		sha1_block(h, block);
		anx_memset(block, 0, 56);
	} else {
		anx_memset(block + tail, 0, 56 - tail);
	}

	/* Append original bit length as 64-bit big-endian */
	block[56] = (uint8_t)(bit_len >> 56);
	block[57] = (uint8_t)(bit_len >> 48);
	block[58] = (uint8_t)(bit_len >> 40);
	block[59] = (uint8_t)(bit_len >> 32);
	block[60] = (uint8_t)(bit_len >> 24);
	block[61] = (uint8_t)(bit_len >> 16);
	block[62] = (uint8_t)(bit_len >>  8);
	block[63] = (uint8_t)(bit_len);
	sha1_block(h, block);

	for (i = 0; i < 5; i++)
		be32(h[i], out + i * 4);
}
