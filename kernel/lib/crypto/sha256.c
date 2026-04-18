/*
 * sha256.c — SHA-256 hash and HMAC-SHA-256.
 *
 * Streaming context-based API extracted from the inline implementation
 * in auth.c. Follows FIPS 180-4.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/crypto.h>

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

#define ROR32(x, n)	(((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)	(((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)	(((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)		(ROR32(x, 2) ^ ROR32(x, 13) ^ ROR32(x, 22))
#define EP1(x)		(ROR32(x, 6) ^ ROR32(x, 11) ^ ROR32(x, 25))
#define SIG0(x)		(ROR32(x, 7) ^ ROR32(x, 18) ^ ((x) >> 3))
#define SIG1(x)		(ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

static uint32_t load_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void store_be64(uint8_t *p, uint64_t v)
{
	store_be32(p, (uint32_t)(v >> 32));
	store_be32(p + 4, (uint32_t)v);
}

static void sha256_compress(uint32_t state[8], const uint8_t block[64])
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t t1, t2;
	uint32_t i;

	for (i = 0; i < 16; i++)
		w[i] = load_be32(block + i * 4);
	for (i = 16; i < 64; i++)
		w[i] = SIG1(w[i - 2]) + w[i - 7] + SIG0(w[i - 15]) + w[i - 16];

	a = state[0]; b = state[1]; c = state[2]; d = state[3];
	e = state[4]; f = state[5]; g = state[6]; h = state[7];

	for (i = 0; i < 64; i++) {
		t1 = h + EP1(e) + CH(e, f, g) + K[i] + w[i];
		t2 = EP0(a) + MAJ(a, b, c);
		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	state[0] += a; state[1] += b; state[2] += c; state[3] += d;
	state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void anx_sha256_init(struct anx_sha256_ctx *ctx)
{
	ctx->state[0] = 0x6a09e667;
	ctx->state[1] = 0xbb67ae85;
	ctx->state[2] = 0x3c6ef372;
	ctx->state[3] = 0xa54ff53a;
	ctx->state[4] = 0x510e527f;
	ctx->state[5] = 0x9b05688c;
	ctx->state[6] = 0x1f83d9ab;
	ctx->state[7] = 0x5be0cd19;
	ctx->total = 0;
}

void anx_sha256_update(struct anx_sha256_ctx *ctx, const void *data,
		       uint32_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t buffered = (uint32_t)(ctx->total & 63);
	uint32_t space;

	ctx->total += len;

	/* Fill partial buffer first */
	if (buffered > 0) {
		space = 64 - buffered;
		if (len < space) {
			anx_memcpy(ctx->buf + buffered, p, len);
			return;
		}
		anx_memcpy(ctx->buf + buffered, p, space);
		sha256_compress(ctx->state, ctx->buf);
		p += space;
		len -= space;
	}

	/* Process full blocks */
	while (len >= 64) {
		sha256_compress(ctx->state, p);
		p += 64;
		len -= 64;
	}

	/* Buffer remainder */
	if (len > 0)
		anx_memcpy(ctx->buf, p, len);
}

void anx_sha256_final(struct anx_sha256_ctx *ctx, uint8_t out[32])
{
	uint64_t total_bits = ctx->total * 8;
	uint32_t buffered = (uint32_t)(ctx->total & 63);
	uint32_t i;

	/* Pad with 0x80 */
	ctx->buf[buffered++] = 0x80;

	/* If not enough room for 8-byte length, pad and compress */
	if (buffered > 56) {
		anx_memset(ctx->buf + buffered, 0, 64 - buffered);
		sha256_compress(ctx->state, ctx->buf);
		buffered = 0;
	}

	anx_memset(ctx->buf + buffered, 0, 56 - buffered);
	store_be64(ctx->buf + 56, total_bits);
	sha256_compress(ctx->state, ctx->buf);

	for (i = 0; i < 8; i++)
		store_be32(out + i * 4, ctx->state[i]);

	/* Wipe sensitive state */
	anx_memset(ctx, 0, sizeof(*ctx));
}

void anx_sha256(const void *data, uint32_t len, uint8_t out[32])
{
	struct anx_sha256_ctx ctx;

	anx_sha256_init(&ctx);
	anx_sha256_update(&ctx, data, len);
	anx_sha256_final(&ctx, out);
}

void anx_hmac_sha256(const void *key, uint32_t key_len,
		     const void *data, uint32_t data_len,
		     uint8_t out[32])
{
	struct anx_sha256_ctx ctx;
	uint8_t kpad[64];
	uint8_t tk[32];
	uint32_t i;

	/* If key is longer than block size, hash it first */
	if (key_len > 64) {
		anx_sha256(key, key_len, tk);
		key = tk;
		key_len = 32;
	}

	/* Inner pad */
	anx_memset(kpad, 0x36, 64);
	for (i = 0; i < key_len; i++)
		kpad[i] ^= ((const uint8_t *)key)[i];

	anx_sha256_init(&ctx);
	anx_sha256_update(&ctx, kpad, 64);
	anx_sha256_update(&ctx, data, data_len);
	anx_sha256_final(&ctx, out);

	/* Outer pad */
	anx_memset(kpad, 0x5c, 64);
	for (i = 0; i < key_len; i++)
		kpad[i] ^= ((const uint8_t *)key)[i];

	anx_sha256_init(&ctx);
	anx_sha256_update(&ctx, kpad, 64);
	anx_sha256_update(&ctx, out, 32);
	anx_sha256_final(&ctx, out);

	anx_memset(kpad, 0, sizeof(kpad));
	anx_memset(tk, 0, sizeof(tk));
}
