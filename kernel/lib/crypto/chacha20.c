/*
 * chacha20.c — ChaCha20 stream cipher.
 *
 * Standard quarter-round based implementation, 20 rounds.
 * RFC 7539 compliant (96-bit nonce, 32-bit counter).
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

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void quarter_round(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	*a += *b; *d ^= *a; *d = ROTL32(*d, 16);
	*c += *d; *b ^= *c; *b = ROTL32(*b, 12);
	*a += *b; *d ^= *a; *d = ROTL32(*d, 8);
	*c += *d; *b ^= *c; *b = ROTL32(*b, 7);
}

static void chacha20_block(const uint32_t input[16], uint8_t out[64])
{
	uint32_t x[16];
	uint32_t i;

	for (i = 0; i < 16; i++)
		x[i] = input[i];

	/* 20 rounds = 10 double-rounds */
	for (i = 0; i < 10; i++) {
		/* Column rounds */
		quarter_round(&x[0], &x[4], &x[8],  &x[12]);
		quarter_round(&x[1], &x[5], &x[9],  &x[13]);
		quarter_round(&x[2], &x[6], &x[10], &x[14]);
		quarter_round(&x[3], &x[7], &x[11], &x[15]);
		/* Diagonal rounds */
		quarter_round(&x[0], &x[5], &x[10], &x[15]);
		quarter_round(&x[1], &x[6], &x[11], &x[12]);
		quarter_round(&x[2], &x[7], &x[8],  &x[13]);
		quarter_round(&x[3], &x[4], &x[9],  &x[14]);
	}

	for (i = 0; i < 16; i++)
		store_le32(out + i * 4, x[i] + input[i]);
}

void anx_chacha20(const uint8_t key[32], const uint8_t nonce[12],
		  uint32_t counter, void *data, uint32_t len)
{
	uint32_t state[16];
	uint8_t keystream[64];
	uint8_t *p = (uint8_t *)data;
	uint32_t i, block_len;

	/* "expand 32-byte k" */
	state[0] = 0x61707865;
	state[1] = 0x3320646e;
	state[2] = 0x79622d32;
	state[3] = 0x6b206574;

	/* Key */
	for (i = 0; i < 8; i++)
		state[4 + i] = load_le32(key + i * 4);

	/* Counter */
	state[12] = counter;

	/* Nonce */
	state[13] = load_le32(nonce);
	state[14] = load_le32(nonce + 4);
	state[15] = load_le32(nonce + 8);

	while (len > 0) {
		chacha20_block(state, keystream);
		block_len = len < 64 ? len : 64;

		for (i = 0; i < block_len; i++)
			p[i] ^= keystream[i];

		p += block_len;
		len -= block_len;
		state[12]++;
	}

	anx_memset(keystream, 0, sizeof(keystream));
	anx_memset(state, 0, sizeof(state));
}
