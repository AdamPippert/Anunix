/*
 * sha1.c — SHA-1, HMAC-SHA-1, PBKDF2-HMAC-SHA-1, PRF-SHA-1.
 *
 * Used for WPA2-PSK key derivation:
 *   PMK  = PBKDF2(HMAC-SHA-1, PSK, SSID, 4096, 32)
 *   PTK  = PRF-512(PMK, "Pairwise key expansion", AA||SPA||ANonce||SNonce)
 *   MIC  = HMAC-SHA-1(KCK, EAPOL_frame)[0:16]
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/crypto.h>

#define ROL32(x, n)  (((x) << (n)) | ((x) >> (32 - (n))))

static uint32_t load_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}

static void store_be64(uint8_t *p, uint64_t v)
{
	store_be32(p, (uint32_t)(v >> 32));
	store_be32(p + 4, (uint32_t)v);
}

static void sha1_compress(uint32_t state[5], const uint8_t block[64])
{
	uint32_t w[80];
	uint32_t a, b, c, d, e, f, k, tmp;
	uint32_t i;

	for (i = 0; i < 16; i++)
		w[i] = load_be32(block + i * 4);
	for (i = 16; i < 80; i++)
		w[i] = ROL32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

	a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

	for (i = 0; i < 80; i++) {
		if (i < 20) {
			f = (b & c) | (~b & d); k = 0x5A827999;
		} else if (i < 40) {
			f = b ^ c ^ d;           k = 0x6ED9EBA1;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC;
		} else {
			f = b ^ c ^ d;           k = 0xCA62C1D6;
		}
		tmp = ROL32(a, 5) + f + e + k + w[i];
		e = d; d = c; c = ROL32(b, 30); b = a; a = tmp;
	}

	state[0] += a; state[1] += b; state[2] += c;
	state[3] += d; state[4] += e;
}

void anx_sha1_init(struct anx_sha1_ctx *ctx)
{
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xEFCDAB89;
	ctx->state[2] = 0x98BADCFE;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xC3D2E1F0;
	ctx->total = 0;
}

void anx_sha1_update(struct anx_sha1_ctx *ctx, const void *data, uint32_t len)
{
	const uint8_t *p = (const uint8_t *)data;
	uint32_t buffered = (uint32_t)(ctx->total & 63);
	uint32_t space;

	ctx->total += len;

	if (buffered > 0) {
		space = 64 - buffered;
		if (len < space) {
			anx_memcpy(ctx->buf + buffered, p, len);
			return;
		}
		anx_memcpy(ctx->buf + buffered, p, space);
		sha1_compress(ctx->state, ctx->buf);
		p += space; len -= space;
	}

	while (len >= 64) {
		sha1_compress(ctx->state, p);
		p += 64; len -= 64;
	}

	if (len > 0)
		anx_memcpy(ctx->buf, p, len);
}

void anx_sha1_final(struct anx_sha1_ctx *ctx, uint8_t out[20])
{
	uint64_t total_bits = ctx->total * 8;
	uint32_t buffered = (uint32_t)(ctx->total & 63);
	uint32_t i;

	ctx->buf[buffered++] = 0x80;
	if (buffered > 56) {
		anx_memset(ctx->buf + buffered, 0, 64 - buffered);
		sha1_compress(ctx->state, ctx->buf);
		buffered = 0;
	}
	anx_memset(ctx->buf + buffered, 0, 56 - buffered);
	store_be64(ctx->buf + 56, total_bits);
	sha1_compress(ctx->state, ctx->buf);

	for (i = 0; i < 5; i++)
		store_be32(out + i * 4, ctx->state[i]);

	anx_memset(ctx, 0, sizeof(*ctx));
}

void anx_sha1(const void *data, uint32_t len, uint8_t out[20])
{
	struct anx_sha1_ctx ctx;

	anx_sha1_init(&ctx);
	anx_sha1_update(&ctx, data, len);
	anx_sha1_final(&ctx, out);
}

void anx_hmac_sha1(const void *key, uint32_t key_len,
		   const void *data, uint32_t data_len,
		   uint8_t out[20])
{
	struct anx_sha1_ctx ctx;
	uint8_t kpad[64], tk[20];
	uint32_t i;

	if (key_len > 64) {
		anx_sha1(key, key_len, tk);
		key = tk; key_len = 20;
	}

	/* Inner hash: H(ipad || data) */
	anx_memset(kpad, 0x36, 64);
	for (i = 0; i < key_len; i++)
		kpad[i] ^= ((const uint8_t *)key)[i];

	anx_sha1_init(&ctx);
	anx_sha1_update(&ctx, kpad, 64);
	anx_sha1_update(&ctx, data, data_len);
	anx_sha1_final(&ctx, out);

	/* Outer hash: H(opad || inner) */
	anx_memset(kpad, 0x5c, 64);
	for (i = 0; i < key_len; i++)
		kpad[i] ^= ((const uint8_t *)key)[i];

	anx_sha1_init(&ctx);
	anx_sha1_update(&ctx, kpad, 64);
	anx_sha1_update(&ctx, out, 20);
	anx_sha1_final(&ctx, out);

	anx_memset(kpad, 0, sizeof(kpad));
	anx_memset(tk, 0, sizeof(tk));
}

/* PBKDF2(HMAC-SHA-1, password, salt, iterations, dkLen) — RFC 2898 */
void anx_pbkdf2_hmac_sha1(const void *pass, uint32_t pass_len,
			   const void *salt, uint32_t salt_len,
			   uint32_t iters, uint8_t *out, uint32_t out_len)
{
	uint8_t block[20], U[20], ibuf[256];
	uint32_t blk, i, j, take;

	if (salt_len > 252) salt_len = 252;

	for (blk = 1; out_len > 0; blk++) {
		/* U1 = HMAC(pass, salt || INT(blk)) */
		anx_memcpy(ibuf, salt, salt_len);
		ibuf[salt_len + 0] = (uint8_t)(blk >> 24);
		ibuf[salt_len + 1] = (uint8_t)(blk >> 16);
		ibuf[salt_len + 2] = (uint8_t)(blk >>  8);
		ibuf[salt_len + 3] = (uint8_t) blk;
		anx_hmac_sha1(pass, pass_len, ibuf, salt_len + 4, block);
		anx_memcpy(U, block, 20);

		for (i = 1; i < iters; i++) {
			anx_hmac_sha1(pass, pass_len, U, 20, U);
			for (j = 0; j < 20; j++)
				block[j] ^= U[j];
		}

		take = (out_len > 20) ? 20 : out_len;
		anx_memcpy(out, block, take);
		out += take; out_len -= take;
	}

	anx_memset(block, 0, sizeof(block));
	anx_memset(U, 0, sizeof(U));
}

/*
 * PRF-SHA-1 (IEEE 802.11-2020 §12.7.1.2):
 *   output = HMAC-SHA-1(K, A || 0x00 || B || counter)
 * Used for PTK = PRF-512(PMK, label, data).
 */
void anx_prf_sha1(const void *key, uint32_t key_len,
		  const char *label, uint32_t label_len,
		  const void *data, uint32_t data_len,
		  uint8_t *out, uint32_t out_len)
{
	uint8_t ibuf[512];
	uint32_t base_len, take;
	uint8_t hash[20];
	uint8_t counter = 0;

	/* ibuf = label || 0x00 || data (counter appended per iteration) */
	anx_memcpy(ibuf, label, label_len);
	ibuf[label_len] = 0x00;
	anx_memcpy(ibuf + label_len + 1, data, data_len);
	base_len = label_len + 1 + data_len;

	while (out_len > 0) {
		ibuf[base_len] = counter++;
		anx_hmac_sha1(key, key_len, ibuf, base_len + 1, hash);
		take = (out_len > 20) ? 20 : out_len;
		anx_memcpy(out, hash, take);
		out += take; out_len -= take;
	}

	anx_memset(hash, 0, sizeof(hash));
}
