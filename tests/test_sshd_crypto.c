/*
 * test_sshd_crypto.c — Tests for chacha20-poly1305@openssh.com as
 * used by the SSH server.  This mimics the exact key/nonce layout the
 * sshd uses so we catch regressions in either primitive without
 * needing a full TCP handshake.
 */

#include <anx/types.h>
#include <anx/crypto.h>
#include <anx/string.h>

static void put_u64(uint8_t *p, uint64_t v)
{
	p[0] = (uint8_t)(v >> 56);
	p[1] = (uint8_t)(v >> 48);
	p[2] = (uint8_t)(v >> 40);
	p[3] = (uint8_t)(v >> 32);
	p[4] = (uint8_t)(v >> 24);
	p[5] = (uint8_t)(v >> 16);
	p[6] = (uint8_t)(v >> 8);
	p[7] = (uint8_t)v;
}

static void ssh_nonce(uint8_t nonce[12], uint64_t seq)
{
	nonce[0] = 0; nonce[1] = 0; nonce[2] = 0; nonce[3] = 0;
	put_u64(nonce + 4, seq);
}

int test_sshd_crypto(void)
{
	/* The SSH chacha20-poly1305 layout:
	 *   K_1 (length) = key[32..63]
	 *   K_2 (payload) = key[0..31]
	 *   nonce = [0,0,0,0] || BE(seq, 8 bytes)
	 *   length encryption: ChaCha20(K_1, nonce, ctr=0) XORed with 4-byte length
	 *   poly key = first 32 bytes of ChaCha20(K_2, nonce, ctr=0) keystream
	 *   payload encryption: ChaCha20(K_2, nonce, ctr=1)
	 *   MAC: Poly1305(poly_key, enc_length || enc_payload)
	 */

	/* Test 1: simple round-trip encrypt then decrypt */
	{
		uint8_t key[64];
		uint32_t i;
		uint8_t nonce[12];
		uint8_t payload[40];
		uint8_t original[40];
		uint8_t encrypted[40];
		uint8_t len_buf[4] = { 0, 0, 0, 40 };
		uint8_t len_enc[4];
		uint8_t poly_key[32];
		uint8_t mac[16];
		uint8_t len_dec[4];
		uint8_t payload_dec[40];

		for (i = 0; i < 64; i++)
			key[i] = (uint8_t)(i * 7 + 3);
		for (i = 0; i < 40; i++)
			payload[i] = (uint8_t)(0x80 + i);
		anx_memcpy(original, payload, 40);
		anx_memcpy(encrypted, payload, 40);

		ssh_nonce(nonce, 42);

		/* Encrypt length */
		anx_memcpy(len_enc, len_buf, 4);
		anx_chacha20(key + 32, nonce, 0, len_enc, 4);

		/* Derive poly key */
		{
			uint8_t block[64];

			anx_memset(block, 0, sizeof(block));
			anx_chacha20(key, nonce, 0, block, 64);
			anx_memcpy(poly_key, block, 32);
		}

		/* Encrypt payload */
		anx_chacha20(key, nonce, 1, encrypted, 40);

		/* MAC over enc_length || enc_payload */
		{
			uint8_t mac_input[44];

			anx_memcpy(mac_input, len_enc, 4);
			anx_memcpy(mac_input + 4, encrypted, 40);
			anx_poly1305(poly_key, mac_input, 44, mac);
		}

		/* Now "receive": decrypt len, verify MAC, decrypt payload */

		/* Decrypt length */
		anx_memcpy(len_dec, len_enc, 4);
		anx_chacha20(key + 32, nonce, 0, len_dec, 4);
		if (len_dec[3] != 40)
			return -1;

		/* Verify MAC */
		{
			uint8_t mac_input[44];
			uint8_t verify_poly[32];
			uint8_t verify_block[64];

			anx_memset(verify_block, 0, sizeof(verify_block));
			anx_chacha20(key, nonce, 0, verify_block, 64);
			anx_memcpy(verify_poly, verify_block, 32);

			anx_memcpy(mac_input, len_enc, 4);
			anx_memcpy(mac_input + 4, encrypted, 40);
			if (anx_poly1305_verify(verify_poly, mac_input, 44, mac) != 0)
				return -2;
		}

		/* Decrypt payload */
		anx_memcpy(payload_dec, encrypted, 40);
		anx_chacha20(key, nonce, 1, payload_dec, 40);
		if (anx_memcmp(payload_dec, original, 40) != 0)
			return -3;
	}

	/* Test 2: seq > 0 produces different ciphertext */
	{
		uint8_t key[32];
		uint8_t nonce_a[12];
		uint8_t nonce_b[12];
		uint8_t data_a[16];
		uint8_t data_b[16];
		uint32_t i;

		for (i = 0; i < 32; i++)
			key[i] = (uint8_t)i;
		for (i = 0; i < 16; i++) {
			data_a[i] = 0xaa;
			data_b[i] = 0xaa;
		}

		ssh_nonce(nonce_a, 0);
		ssh_nonce(nonce_b, 1);
		anx_chacha20(key, nonce_a, 0, data_a, 16);
		anx_chacha20(key, nonce_b, 0, data_b, 16);

		if (anx_memcmp(data_a, data_b, 16) == 0)
			return -10;
	}

	/* Test 3: Poly1305 over 44-byte input using known-good vector */
	{
		const uint8_t key[32] = {
			0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33,
			0x7f, 0x44, 0x52, 0xfe, 0x42, 0xd5, 0x06, 0xa8,
			0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd,
			0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b
		};
		const char *msg = "Cryptographic Forum Research Groupxxxxxxxxxx";
		const uint8_t expected[16] = {
			0x3b, 0x4d, 0xd3, 0x6d, 0x56, 0x60, 0x3b, 0x90,
			0xaa, 0x1f, 0xa5, 0x68, 0x66, 0x18, 0x87, 0x9a
		};
		uint8_t tag[16];

		anx_poly1305(key, msg, (uint32_t)anx_strlen(msg), tag);
		if (anx_memcmp(tag, expected, 16) != 0)
			return -20;
	}

	/* Test 4: Poly1305 over various lengths */
	{
		const uint8_t key[32] = {
			0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33,
			0x7f, 0x44, 0x52, 0xfe, 0x42, 0xd5, 0x06, 0xa8,
			0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd,
			0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b
		};
		uint8_t msg[128];
		uint8_t tag1[16];
		uint8_t tag2[16];
		uint32_t i;
		uint32_t n;

		for (i = 0; i < 128; i++)
			msg[i] = (uint8_t)(i * 17 + 31);

		/* Tag should differ for different lengths */
		for (n = 1; n < 128; n++) {
			anx_poly1305(key, msg, n, tag1);
			anx_poly1305(key, msg, n + 1, tag2);
			if (anx_memcmp(tag1, tag2, 16) == 0)
				return -(30 + (int)n);
		}
	}

	return 0;
}
