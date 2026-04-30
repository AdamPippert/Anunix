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

/* --- SHA-1 (needed for WPA2 key derivation) --- */

struct anx_sha1_ctx {
	uint32_t state[5];
	uint8_t  buf[64];
	uint64_t total;
};

void anx_sha1_init(struct anx_sha1_ctx *ctx);
void anx_sha1_update(struct anx_sha1_ctx *ctx, const void *data, uint32_t len);
void anx_sha1_final(struct anx_sha1_ctx *ctx, uint8_t out[20]);
void anx_sha1(const void *data, uint32_t len, uint8_t out[20]);

/* HMAC-SHA-1 */
void anx_hmac_sha1(const void *key, uint32_t key_len,
		   const void *data, uint32_t data_len,
		   uint8_t out[20]);

/* PBKDF2(HMAC-SHA-1, password, salt, iterations, dkLen) — for WPA2 PMK */
void anx_pbkdf2_hmac_sha1(const void *pass, uint32_t pass_len,
			   const void *salt, uint32_t salt_len,
			   uint32_t iters, uint8_t *out, uint32_t out_len);

/* PRF-SHA-1 (IEEE 802.11 §12.7.1.2) — for WPA2 PTK */
void anx_prf_sha1(const void *key, uint32_t key_len,
		  const char *label, uint32_t label_len,
		  const void *data, uint32_t data_len,
		  uint8_t *out, uint32_t out_len);

/* RFC 3394 AES-128 key unwrap — for GTK decryption from EAPOL M3.
 * kek: 16-byte Key Encryption Key; wrapped_len must be multiple of 8.
 * Returns 0 on success, -1 if integrity check fails. */
int anx_aes128_unwrap(const uint8_t kek[16],
		      const uint8_t *wrapped, uint32_t wrapped_len,
		      uint8_t *out, uint32_t out_len);

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
