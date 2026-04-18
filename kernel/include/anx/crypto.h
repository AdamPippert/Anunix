/*
 * anx/crypto.h — Cryptographic primitives for the Anunix kernel.
 *
 * Provides SHA-256, SHA-512, HMAC-SHA-256, ChaCha20, Poly1305,
 * AES-256-CTR, Curve25519, Ed25519, and secure random generation.
 * All implementations are constant-time where security requires it.
 * No floating point — integer arithmetic only.
 */

#ifndef ANX_CRYPTO_H
#define ANX_CRYPTO_H

#include <anx/types.h>

/* --- SHA-256 --- */

struct anx_sha256_ctx {
	uint32_t state[8];
	uint8_t buf[64];
	uint64_t total;
};

/* Initialize SHA-256 context */
void anx_sha256_init(struct anx_sha256_ctx *ctx);

/* Feed data into SHA-256 */
void anx_sha256_update(struct anx_sha256_ctx *ctx, const void *data,
		       uint32_t len);

/* Finalize and produce 32-byte digest */
void anx_sha256_final(struct anx_sha256_ctx *ctx, uint8_t out[32]);

/* One-shot SHA-256 */
void anx_sha256(const void *data, uint32_t len, uint8_t out[32]);

/* HMAC-SHA-256 */
void anx_hmac_sha256(const void *key, uint32_t key_len,
		     const void *data, uint32_t data_len,
		     uint8_t out[32]);

/* --- SHA-512 (needed by Ed25519) --- */

struct anx_sha512_ctx {
	uint64_t state[8];
	uint8_t buf[128];
	uint64_t total;
};

/* Initialize SHA-512 context */
void anx_sha512_init(struct anx_sha512_ctx *ctx);

/* Feed data into SHA-512 */
void anx_sha512_update(struct anx_sha512_ctx *ctx, const void *data,
		       uint32_t len);

/* Finalize and produce 64-byte digest */
void anx_sha512_final(struct anx_sha512_ctx *ctx, uint8_t out[64]);

/* One-shot SHA-512 */
void anx_sha512(const void *data, uint32_t len, uint8_t out[64]);

/* --- ChaCha20 stream cipher --- */

/* Encrypt/decrypt data in place with ChaCha20 */
void anx_chacha20(const uint8_t key[32], const uint8_t nonce[12],
		  uint32_t counter, void *data, uint32_t len);

/* --- Poly1305 one-time authenticator --- */

/* Compute a 16-byte Poly1305 tag */
void anx_poly1305(const uint8_t key[32], const void *msg, uint32_t len,
		  uint8_t tag[16]);

/* Verify a Poly1305 tag in constant time (0 = match) */
int anx_poly1305_verify(const uint8_t key[32], const void *msg,
			uint32_t len, const uint8_t tag[16]);

/* --- AES-256-CTR --- */

/* Encrypt/decrypt data in place with AES-256 in CTR mode */
void anx_aes256_ctr(const uint8_t key[32], const uint8_t iv[16],
		    void *data, uint32_t len);

/* --- Curve25519 ECDH --- */

/* Scalar multiplication: out = scalar * point */
void anx_curve25519(uint8_t out[32], const uint8_t scalar[32],
		    const uint8_t point[32]);

/* Public key from private: pub = scalar * basepoint */
void anx_curve25519_base(uint8_t pub[32], const uint8_t priv[32]);

/* --- Ed25519 signatures --- */

/* Derive keypair from 32-byte seed */
void anx_ed25519_keypair(uint8_t pub[32], uint8_t priv[64],
			 const uint8_t seed[32]);

/* Sign a message */
void anx_ed25519_sign(uint8_t sig[64], const void *msg, uint32_t len,
		      const uint8_t priv[64]);

/* Verify a signature (0 = valid) */
int anx_ed25519_verify(const uint8_t sig[64], const void *msg,
		       uint32_t len, const uint8_t pub[32]);

/* --- Random number generation --- */

/* Fill buffer with random bytes */
void anx_random_bytes(void *buf, uint32_t len);

/* Return a random 32-bit value */
uint32_t anx_random_u32(void);

#endif /* ANX_CRYPTO_H */
