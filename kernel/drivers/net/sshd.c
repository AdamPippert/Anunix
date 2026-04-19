/*
 * sshd.c — SSH-2.0 server for the Anunix kernel.
 *
 * Implements RFC 4251-4254 with the following algorithm suite:
 *   kex:        curve25519-sha256 (RFC 8731)
 *   hostkey:    ssh-ed25519 (RFC 8709)
 *   cipher:     chacha20-poly1305@openssh.com
 *   mac:        implicit (AEAD)
 *   compress:   none
 *
 * Single connection at a time, synchronous protocol handling on
 * the TCP receive buffer. Host key is persisted in the credential
 * store under the name "ssh-host-key".
 *
 * Shell channel integration:
 *   exec  — runs a single command and returns its captured output
 *   shell — line-by-line interactive loop, each newline-terminated
 *           line is dispatched to anx_shell_execute() and the
 *           captured kprintf output is sent as CHANNEL_DATA.
 */

#include <anx/types.h>
#include <anx/arch.h>
#include <anx/sshd.h>
#include <anx/net.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>
#include <anx/crypto.h>
#include <anx/credential.h>
#include <anx/shell.h>

/* --- SSH message numbers (RFC 4250 section 4.1) --- */
#define SSH_MSG_DISCONNECT			1
#define SSH_MSG_IGNORE				2
#define SSH_MSG_UNIMPLEMENTED			3
#define SSH_MSG_DEBUG				4
#define SSH_MSG_SERVICE_REQUEST			5
#define SSH_MSG_SERVICE_ACCEPT			6
#define SSH_MSG_KEXINIT				20
#define SSH_MSG_NEWKEYS				21
#define SSH_MSG_KEX_ECDH_INIT			30
#define SSH_MSG_KEX_ECDH_REPLY			31
#define SSH_MSG_USERAUTH_REQUEST		50
#define SSH_MSG_USERAUTH_FAILURE		51
#define SSH_MSG_USERAUTH_SUCCESS		52
#define SSH_MSG_USERAUTH_BANNER			53
#define SSH_MSG_USERAUTH_PK_OK			60
#define SSH_MSG_GLOBAL_REQUEST			80
#define SSH_MSG_REQUEST_SUCCESS			81
#define SSH_MSG_REQUEST_FAILURE			82
#define SSH_MSG_CHANNEL_OPEN			90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION	91
#define SSH_MSG_CHANNEL_OPEN_FAILURE		92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST		93
#define SSH_MSG_CHANNEL_DATA			94
#define SSH_MSG_CHANNEL_EXTENDED_DATA		95
#define SSH_MSG_CHANNEL_EOF			96
#define SSH_MSG_CHANNEL_CLOSE			97
#define SSH_MSG_CHANNEL_REQUEST			98
#define SSH_MSG_CHANNEL_SUCCESS			99
#define SSH_MSG_CHANNEL_FAILURE			100

/* Disconnect reason codes */
#define SSH_DISCONNECT_PROTOCOL_ERROR		2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED	3
#define SSH_DISCONNECT_MAC_ERROR		5
#define SSH_DISCONNECT_SERVICE_NOT_AVAILABLE	7
#define SSH_DISCONNECT_BY_APPLICATION		11

/* Channel open failure codes */
#define SSH_OPEN_ADMINISTRATIVELY_PROHIBITED	1
#define SSH_OPEN_CONNECT_FAILED			2
#define SSH_OPEN_UNKNOWN_CHANNEL_TYPE		3
#define SSH_OPEN_RESOURCE_SHORTAGE		4

/* --- Buffer sizes --- */
#define SSHD_RECV_TIMEOUT_MS	30000
#define SSHD_MAX_PACKET		32768
#define SSHD_MAX_PAYLOAD	32000
#define SSHD_VERSION_MAX	255
#define SSHD_CAPTURE_SIZE	8192
#define SSHD_LINE_MAX		512

/* Our window size for the session channel */
#define SSHD_WINDOW_SIZE	65536
#define SSHD_MAX_PACKET_SIZE	32768

#define SSHD_VERSION_STRING	"SSH-2.0-Anunix_1.0"
#define SSHD_DEFAULT_PASSWORD	"anunix"

/* --- Connection state --- */

enum sshd_phase {
	SSHD_PHASE_VERSION,
	SSHD_PHASE_KEX,
	SSHD_PHASE_AUTH,
	SSHD_PHASE_CONNECTION,
	SSHD_PHASE_CLOSED,
};

struct sshd_state {
	struct anx_tcp_conn *conn;
	enum sshd_phase phase;

	/* Version strings (without trailing CR/LF) */
	char v_client[SSHD_VERSION_MAX];
	uint32_t v_client_len;
	char v_server[SSHD_VERSION_MAX];
	uint32_t v_server_len;

	/* KEXINIT payloads for the exchange hash */
	uint8_t *i_client;
	uint32_t i_client_len;
	uint8_t *i_server;
	uint32_t i_server_len;

	/* Host key (Ed25519) */
	uint8_t host_pub[32];
	uint8_t host_priv[64];

	/* Ephemeral Curve25519 key pair */
	uint8_t eph_priv[32];
	uint8_t eph_pub[32];

	/* Shared secret K (32 bytes) and exchange hash H (SHA-256) */
	uint8_t K[32];
	uint32_t K_len;	/* 32 by construction from curve25519 */
	uint8_t H[32];
	uint8_t session_id[32];
	bool have_session_id;

	/* Key material derived from K+H.
	 * For chacha20-poly1305, we use 64-byte "encryption" keys only
	 * (first 32 bytes for length encryption, second 32 for payload).
	 * IVs are unused by chacha20-poly1305; MAC keys are unused.
	 */
	uint8_t key_c2s[64];	/* client -> server payload+length keys */
	uint8_t key_s2c[64];	/* server -> client payload+length keys */
	bool encrypted_c2s;
	bool encrypted_s2c;

	/* Packet sequence numbers */
	uint32_t seq_c2s;
	uint32_t seq_s2c;

	/* Session channel state */
	uint32_t server_channel;
	uint32_t client_channel;
	uint32_t client_window;
	uint32_t client_max_packet;
	bool channel_open;
	bool shell_mode;

	/* Shell line buffer */
	char line[SSHD_LINE_MAX];
	uint32_t line_len;
};

/* --- Globals --- */

static struct anx_tcp_conn *g_pending_conn;
static bool g_sshd_ready;

/* --- Forward decls --- */
static int sshd_send_disconnect(struct sshd_state *s, uint32_t reason,
				const char *desc);

/* --- Byte-order / buffer helpers --- */

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

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

/* Simple growable builder backed by a caller-provided buffer */
struct ssh_buf {
	uint8_t *data;
	uint32_t len;
	uint32_t cap;
	bool overflow;
};

static void buf_init(struct ssh_buf *b, uint8_t *data, uint32_t cap)
{
	b->data = data;
	b->len = 0;
	b->cap = cap;
	b->overflow = false;
}

static void buf_u8(struct ssh_buf *b, uint8_t v)
{
	if (b->len + 1 > b->cap) { b->overflow = true; return; }
	b->data[b->len++] = v;
}

static void buf_u32(struct ssh_buf *b, uint32_t v)
{
	if (b->len + 4 > b->cap) { b->overflow = true; return; }
	put_u32(b->data + b->len, v);
	b->len += 4;
}

static void buf_bytes(struct ssh_buf *b, const void *src, uint32_t n)
{
	if (b->len + n > b->cap) { b->overflow = true; return; }
	anx_memcpy(b->data + b->len, src, n);
	b->len += n;
}

static void buf_string(struct ssh_buf *b, const void *src, uint32_t n)
{
	buf_u32(b, n);
	buf_bytes(b, src, n);
}

static void buf_cstr(struct ssh_buf *b, const char *s)
{
	buf_string(b, s, (uint32_t)anx_strlen(s));
}

/* mpint: integer encoded with leading zero byte if MSB would set sign */
static void buf_mpint(struct ssh_buf *b, const uint8_t *num, uint32_t n)
{
	uint32_t skip = 0;

	while (skip < n && num[skip] == 0)
		skip++;
	if (skip == n) {
		buf_u32(b, 0);
		return;
	}
	if (num[skip] & 0x80) {
		buf_u32(b, (n - skip) + 1);
		buf_u8(b, 0);
		buf_bytes(b, num + skip, n - skip);
	} else {
		buf_u32(b, n - skip);
		buf_bytes(b, num + skip, n - skip);
	}
}

/* --- ChaCha20-Poly1305@openssh.com helpers ---
 *
 * The openssh variant uses the *original* 64-bit-nonce ChaCha20 with
 * a 64-bit counter in state words 12-13 and an 8-byte big-endian
 * packet sequence number as the nonce in words 14-15.
 *
 * Our IETF-style anx_chacha20() takes a 12-byte nonce (words 13-15)
 * and a 32-bit counter (word 12). To match the openssh layout with
 * our API we pass:
 *
 *   counter = desired block counter (low 32 bits only — fine for
 *             packets < 2^38 bytes)
 *   nonce   = [ 0 0 0 0 ][ seq big-endian 8 bytes ]
 *
 * This puts 0 into word 13 (ctr high), and the 8-byte BE seqnum
 * into words 14-15.
 */

static void ssh_chacha_nonce(uint8_t nonce[12], uint64_t seq)
{
	nonce[0] = 0; nonce[1] = 0; nonce[2] = 0; nonce[3] = 0;
	put_u64(nonce + 4, seq);
}

/* Encrypt/decrypt the 4-byte packet length field with K_1 */
static void ssh_cp_len_crypt(const uint8_t *key_main /* 64 bytes */,
			     uint64_t seq, uint8_t len_bytes[4])
{
	uint8_t nonce[12];
	const uint8_t *k1 = key_main + 32;

	ssh_chacha_nonce(nonce, seq);
	anx_chacha20(k1, nonce, 0, len_bytes, 4);
}

/* Derive the Poly1305 one-time key: first 32 bytes of ChaCha20(K_2, seq, ctr=0) */
static void ssh_cp_poly_key(const uint8_t *key_main, uint64_t seq,
			    uint8_t poly_key[32])
{
	uint8_t nonce[12];
	uint8_t block[64];
	const uint8_t *k2 = key_main;

	ssh_chacha_nonce(nonce, seq);
	anx_memset(block, 0, sizeof(block));
	anx_chacha20(k2, nonce, 0, block, 64);
	anx_memcpy(poly_key, block, 32);
	anx_memset(block, 0, sizeof(block));
}

/* Encrypt/decrypt the packet payload (everything after the 4-byte length)
 * with K_2 starting at counter=1. */
static void ssh_cp_payload_crypt(const uint8_t *key_main, uint64_t seq,
				 uint8_t *data, uint32_t len)
{
	uint8_t nonce[12];
	const uint8_t *k2 = key_main;

	ssh_chacha_nonce(nonce, seq);
	anx_chacha20(k2, nonce, 1, data, len);
}

/* --- TCP receive with framing ---
 *
 * Read exactly n bytes or fail. Uses anx_tcp_srv_recv in a loop.
 */
static int tcp_recv_exact(struct anx_tcp_conn *conn, uint8_t *buf,
			  uint32_t n, uint32_t timeout_ms)
{
	uint32_t got = 0;
	int r;

	while (got < n) {
		r = anx_tcp_srv_recv(conn, buf + got, n - got, timeout_ms);
		if (r <= 0)
			return r == 0 ? ANX_ECONNRESET : r;
		got += (uint32_t)r;
	}
	return (int)got;
}

/* --- SSH version banner exchange --- */

static int sshd_send_version(struct sshd_state *s)
{
	char line[SSHD_VERSION_MAX + 4];
	uint32_t n;

	n = (uint32_t)anx_strlen(SSHD_VERSION_STRING);
	anx_memcpy(line, SSHD_VERSION_STRING, n);
	line[n++] = '\r';
	line[n++] = '\n';

	anx_strlcpy(s->v_server, SSHD_VERSION_STRING, sizeof(s->v_server));
	s->v_server_len = (uint32_t)anx_strlen(SSHD_VERSION_STRING);

	if (anx_tcp_srv_send(s->conn, line, n) != ANX_OK)
		return ANX_EIO;
	return ANX_OK;
}

/* Read lines from the client until we see an SSH-2.0 banner.  Per RFC 4253
 * section 4.2 the server MUST tolerate other preceding lines before the
 * version string. */
static int sshd_recv_version(struct sshd_state *s)
{
	char buf[SSHD_VERSION_MAX];
	uint32_t len;
	int lines = 0;
	uint8_t c;
	int r;

	while (lines < 16) {
		len = 0;
		while (len < SSHD_VERSION_MAX - 1) {
			r = tcp_recv_exact(s->conn, &c, 1,
					    SSHD_RECV_TIMEOUT_MS);
			if (r <= 0)
				return ANX_EIO;
			if (c == '\n') {
				if (len > 0 && buf[len - 1] == '\r')
					len--;
				break;
			}
			buf[len++] = (char)c;
		}
		buf[len] = '\0';

		if (len >= 4 && buf[0] == 'S' && buf[1] == 'S' &&
		    buf[2] == 'H' && buf[3] == '-') {
			if (len > sizeof(s->v_client) - 1)
				len = sizeof(s->v_client) - 1;
			anx_memcpy(s->v_client, buf, len);
			s->v_client[len] = '\0';
			s->v_client_len = len;
			return ANX_OK;
		}
		lines++;
	}
	return ANX_EIO;
}

/* --- Packet send / recv ---
 *
 * SSH binary packet protocol (RFC 4253 section 6):
 *   uint32 packet_length
 *   byte   padding_length
 *   byte[] payload
 *   byte[] random_padding
 *   byte[] mac
 *
 * For chacha20-poly1305@openssh.com the 4-byte length is encrypted
 * separately and not included in the MAC computation as plaintext —
 * instead the MAC covers the encrypted length + encrypted payload.
 */

static int sshd_send_packet(struct sshd_state *s, const uint8_t *payload,
			    uint32_t payload_len)
{
	uint8_t header[5];
	uint8_t padding[256];
	uint32_t block_size;
	uint32_t packet_len;
	uint32_t pad_len;
	uint32_t total;
	uint8_t *pkt;
	int ret = ANX_EIO;

	/* Padding rules:
	 *   packet_length + padding_length + payload + padding must be a
	 *   multiple of block_size (min 8). For chacha20-poly1305 the spec
	 *   excludes the 4-byte length from the alignment check. */
	block_size = 8;

	if (s->encrypted_s2c) {
		/* Align (padding_length + payload + padding) to block_size */
		uint32_t rem = (1 + payload_len) % block_size;

		pad_len = block_size - rem;
		if (pad_len < 4)
			pad_len += block_size;
	} else {
		/* Include the 4-byte length in the alignment */
		uint32_t rem = (4 + 1 + payload_len) % block_size;

		pad_len = block_size - rem;
		if (pad_len < 4)
			pad_len += block_size;
	}
	if (pad_len > sizeof(padding))
		return ANX_EINVAL;

	packet_len = 1 + payload_len + pad_len;
	total = 4 + packet_len;
	if (s->encrypted_s2c)
		total += 16;	/* poly1305 tag */

	pkt = anx_alloc(total);
	if (!pkt)
		return ANX_ENOMEM;

	put_u32(header, packet_len);
	header[4] = (uint8_t)pad_len;

	/* Build plaintext packet: [len(4) | pad_len(1) | payload | pad] */
	anx_memcpy(pkt, header, 5);
	if (payload_len > 0)
		anx_memcpy(pkt + 5, payload, payload_len);
	anx_random_bytes(padding, pad_len);
	anx_memcpy(pkt + 5 + payload_len, padding, pad_len);

	if (s->encrypted_s2c) {
		uint8_t poly_key[32];
		uint8_t *len_ct = pkt;
		uint8_t *payload_ct = pkt + 4;
		uint32_t payload_ct_len = 1 + payload_len + pad_len;

		/* Derive poly1305 key from K_2 with seq (before encryption) */
		ssh_cp_poly_key(s->key_s2c, s->seq_s2c, poly_key);

		/* Encrypt length with K_1 */
		ssh_cp_len_crypt(s->key_s2c, s->seq_s2c, len_ct);
		/* Encrypt payload+pad with K_2 ctr=1 */
		ssh_cp_payload_crypt(s->key_s2c, s->seq_s2c, payload_ct,
				     payload_ct_len);
		/* MAC covers encrypted length + encrypted payload */
		anx_poly1305(poly_key, pkt, 4 + payload_ct_len,
			     pkt + 4 + payload_ct_len);

		anx_memset(poly_key, 0, sizeof(poly_key));
	}

	ret = anx_tcp_srv_send(s->conn, pkt, total);
	s->seq_s2c++;

	anx_free(pkt);
	return ret;
}

/* Receive a packet. On success, *payload is a newly-allocated buffer
 * of size *payload_len that the caller must free. */
static int sshd_recv_packet(struct sshd_state *s, uint8_t **payload_out,
			    uint32_t *payload_len_out)
{
	uint8_t len_buf[4];
	uint32_t packet_len;
	uint32_t rest_len;
	uint8_t *body;
	uint8_t pad_len;
	uint32_t payload_len;
	uint8_t *payload;
	int r;

	/* 1. Read encrypted-or-plaintext length field */
	r = tcp_recv_exact(s->conn, len_buf, 4, SSHD_RECV_TIMEOUT_MS);
	if (r <= 0)
		return ANX_EIO;

	if (s->encrypted_c2s) {
		/* Decrypt length with K_1 — save original for MAC */
		uint8_t enc_len[4];

		anx_memcpy(enc_len, len_buf, 4);
		ssh_cp_len_crypt(s->key_c2s, s->seq_c2s, len_buf);
		packet_len = get_u32(len_buf);

		if (packet_len < 8 || packet_len > SSHD_MAX_PACKET)
			return ANX_EIO;

		rest_len = packet_len + 16;	/* +poly1305 tag */
		body = anx_alloc(4 + rest_len);
		if (!body)
			return ANX_ENOMEM;

		/* Re-store the encrypted length at body[0..3] for MAC */
		anx_memcpy(body, enc_len, 4);

		r = tcp_recv_exact(s->conn, body + 4, rest_len,
				    SSHD_RECV_TIMEOUT_MS);
		if (r <= 0) {
			anx_free(body);
			return ANX_EIO;
		}

		{
			uint8_t poly_key[32];
			int vr;

			ssh_cp_poly_key(s->key_c2s, s->seq_c2s, poly_key);
			vr = anx_poly1305_verify(poly_key, body,
						  4 + packet_len,
						  body + 4 + packet_len);
			anx_memset(poly_key, 0, sizeof(poly_key));
			if (vr != 0) {
				anx_free(body);
				sshd_send_disconnect(s,
					SSH_DISCONNECT_MAC_ERROR,
					"bad poly1305 tag");
				return ANX_EIO;
			}
		}

		/* Decrypt payload portion */
		ssh_cp_payload_crypt(s->key_c2s, s->seq_c2s,
				     body + 4, packet_len);

		pad_len = body[4];
		if (pad_len + 1u > packet_len) {
			anx_free(body);
			return ANX_EIO;
		}
		payload_len = packet_len - pad_len - 1;
		payload = anx_alloc(payload_len == 0 ? 1 : payload_len);
		if (!payload) {
			anx_free(body);
			return ANX_ENOMEM;
		}
		if (payload_len > 0)
			anx_memcpy(payload, body + 5, payload_len);
		anx_free(body);
	} else {
		packet_len = get_u32(len_buf);
		if (packet_len < 8 || packet_len > SSHD_MAX_PACKET)
			return ANX_EIO;

		body = anx_alloc(packet_len);
		if (!body)
			return ANX_ENOMEM;

		r = tcp_recv_exact(s->conn, body, packet_len,
				    SSHD_RECV_TIMEOUT_MS);
		if (r <= 0) {
			anx_free(body);
			return ANX_EIO;
		}

		pad_len = body[0];
		if (pad_len + 1u > packet_len) {
			anx_free(body);
			return ANX_EIO;
		}
		payload_len = packet_len - pad_len - 1;
		payload = anx_alloc(payload_len == 0 ? 1 : payload_len);
		if (!payload) {
			anx_free(body);
			return ANX_ENOMEM;
		}
		if (payload_len > 0)
			anx_memcpy(payload, body + 1, payload_len);
		anx_free(body);
	}

	s->seq_c2s++;
	*payload_out = payload;
	*payload_len_out = payload_len;
	return ANX_OK;
}

/* --- DISCONNECT --- */

static int sshd_send_disconnect(struct sshd_state *s, uint32_t reason,
				const char *desc)
{
	uint8_t buf[256];
	struct ssh_buf b;

	buf_init(&b, buf, sizeof(buf));
	buf_u8(&b, SSH_MSG_DISCONNECT);
	buf_u32(&b, reason);
	buf_cstr(&b, desc);
	buf_cstr(&b, "");	/* language tag */
	if (b.overflow)
		return ANX_ENOMEM;
	return sshd_send_packet(s, b.data, b.len);
}

/* --- Host key management --- */

/* Load or create the Ed25519 host key, stored in the credential store
 * as a 96-byte blob: 32-byte public || 64-byte private. */
static int ensure_host_key(uint8_t pub[32], uint8_t priv[64])
{
	uint8_t blob[96];
	uint32_t actual;
	int r;

	if (anx_credential_exists("ssh-host-key")) {
		r = anx_credential_read("ssh-host-key", blob, sizeof(blob),
					 &actual);
		if (r == ANX_OK && actual == 96) {
			anx_memcpy(pub, blob, 32);
			anx_memcpy(priv, blob + 32, 64);
			anx_memset(blob, 0, sizeof(blob));
			return ANX_OK;
		}
		/* Corrupt or unexpected length — regenerate */
		anx_credential_revoke("ssh-host-key");
	}

	{
		uint8_t seed[32];

		anx_random_bytes(seed, sizeof(seed));
		anx_ed25519_keypair(pub, priv, seed);
		anx_memset(seed, 0, sizeof(seed));
	}

	anx_memcpy(blob, pub, 32);
	anx_memcpy(blob + 32, priv, 64);
	r = anx_credential_create("ssh-host-key", ANX_CRED_PRIVATE_KEY,
				   blob, 96);
	anx_memset(blob, 0, sizeof(blob));
	if (r != ANX_OK)
		return r;
	return ANX_OK;
}

/* Build the SSH wire encoding of the ed25519 host key blob:
 *   string "ssh-ed25519"
 *   string pubkey(32)
 */
static uint32_t build_host_key_blob(const uint8_t pub[32],
				    uint8_t *out, uint32_t cap)
{
	struct ssh_buf b;

	buf_init(&b, out, cap);
	buf_cstr(&b, "ssh-ed25519");
	buf_string(&b, pub, 32);
	return b.overflow ? 0 : b.len;
}

/* Build the SSH wire encoding of the ed25519 signature:
 *   string "ssh-ed25519"
 *   string signature(64)
 */
static uint32_t build_signature_blob(const uint8_t sig[64], uint8_t *out,
				     uint32_t cap)
{
	struct ssh_buf b;

	buf_init(&b, out, cap);
	buf_cstr(&b, "ssh-ed25519");
	buf_string(&b, sig, 64);
	return b.overflow ? 0 : b.len;
}

/* --- KEX --- */

static const char *kex_algos =
	"curve25519-sha256,curve25519-sha256@libssh.org";
static const char *hostkey_algos = "ssh-ed25519";
static const char *cipher_algos = "chacha20-poly1305@openssh.com";
static const char *mac_algos = "hmac-sha2-256";	/* implicit with AEAD */
static const char *compress_algos = "none";
static const char *lang_algos = "";

/* Build SSH_MSG_KEXINIT and store the payload (including the message
 * byte) in s->i_server for later hashing. */
static int sshd_send_kexinit(struct sshd_state *s)
{
	uint8_t *buf;
	struct ssh_buf b;
	uint8_t cookie[16];
	int ret;

	buf = anx_alloc(2048);
	if (!buf)
		return ANX_ENOMEM;

	buf_init(&b, buf, 2048);
	buf_u8(&b, SSH_MSG_KEXINIT);

	anx_random_bytes(cookie, sizeof(cookie));
	buf_bytes(&b, cookie, sizeof(cookie));

	buf_cstr(&b, kex_algos);
	buf_cstr(&b, hostkey_algos);
	buf_cstr(&b, cipher_algos);	/* encryption c2s */
	buf_cstr(&b, cipher_algos);	/* encryption s2c */
	buf_cstr(&b, mac_algos);	/* mac c2s */
	buf_cstr(&b, mac_algos);	/* mac s2c */
	buf_cstr(&b, compress_algos);	/* compress c2s */
	buf_cstr(&b, compress_algos);	/* compress s2c */
	buf_cstr(&b, lang_algos);	/* lang c2s */
	buf_cstr(&b, lang_algos);	/* lang s2c */
	buf_u8(&b, 0);			/* first_kex_packet_follows */
	buf_u32(&b, 0);			/* reserved */

	if (b.overflow) {
		anx_free(buf);
		return ANX_ENOMEM;
	}

	/* Save I_S (payload including the SSH_MSG_KEXINIT byte) for H */
	s->i_server = anx_alloc(b.len);
	if (!s->i_server) {
		anx_free(buf);
		return ANX_ENOMEM;
	}
	anx_memcpy(s->i_server, b.data, b.len);
	s->i_server_len = b.len;

	ret = sshd_send_packet(s, b.data, b.len);
	anx_free(buf);
	return ret;
}

/* Read (and store) the client's KEXINIT. */
static int sshd_recv_kexinit(struct sshd_state *s)
{
	uint8_t *payload;
	uint32_t payload_len;
	int ret;

	ret = sshd_recv_packet(s, &payload, &payload_len);
	if (ret != ANX_OK)
		return ret;

	if (payload_len < 1 || payload[0] != SSH_MSG_KEXINIT) {
		anx_free(payload);
		return ANX_EIO;
	}

	s->i_client = anx_alloc(payload_len);
	if (!s->i_client) {
		anx_free(payload);
		return ANX_ENOMEM;
	}
	anx_memcpy(s->i_client, payload, payload_len);
	s->i_client_len = payload_len;

	anx_free(payload);
	return ANX_OK;
}

/* Compute the exchange hash H (RFC 5656):
 *   H = HASH(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K)
 * where each element is an SSH string (uint32 len + bytes), except K
 * which is an mpint.
 */
static void compute_exchange_hash(struct sshd_state *s,
				  const uint8_t *k_s, uint32_t k_s_len,
				  const uint8_t *q_c, uint32_t q_c_len,
				  const uint8_t *q_s, uint32_t q_s_len,
				  const uint8_t *k, uint32_t k_len,
				  uint8_t out[32])
{
	struct anx_sha256_ctx ctx;
	uint8_t hdr[4];
	uint8_t mpint_buf[64];
	struct ssh_buf mb;

	anx_sha256_init(&ctx);

	put_u32(hdr, s->v_client_len);
	anx_sha256_update(&ctx, hdr, 4);
	anx_sha256_update(&ctx, s->v_client, s->v_client_len);

	put_u32(hdr, s->v_server_len);
	anx_sha256_update(&ctx, hdr, 4);
	anx_sha256_update(&ctx, s->v_server, s->v_server_len);

	put_u32(hdr, s->i_client_len);
	anx_sha256_update(&ctx, hdr, 4);
	anx_sha256_update(&ctx, s->i_client, s->i_client_len);

	put_u32(hdr, s->i_server_len);
	anx_sha256_update(&ctx, hdr, 4);
	anx_sha256_update(&ctx, s->i_server, s->i_server_len);

	put_u32(hdr, k_s_len);
	anx_sha256_update(&ctx, hdr, 4);
	anx_sha256_update(&ctx, k_s, k_s_len);

	put_u32(hdr, q_c_len);
	anx_sha256_update(&ctx, hdr, 4);
	anx_sha256_update(&ctx, q_c, q_c_len);

	put_u32(hdr, q_s_len);
	anx_sha256_update(&ctx, hdr, 4);
	anx_sha256_update(&ctx, q_s, q_s_len);

	/* K is mpint */
	buf_init(&mb, mpint_buf, sizeof(mpint_buf));
	buf_mpint(&mb, k, k_len);
	anx_sha256_update(&ctx, mb.data, mb.len);

	anx_sha256_final(&ctx, out);
}

/* Derive a key of the requested length from (K, H, letter, session_id)
 * using SHA-256 as defined in RFC 4253 section 7.2:
 *
 *   K1 = HASH(K || H || letter || session_id)
 *   Kn = HASH(K || H || K1 || K2 || ... Kn-1)
 *   key = K1 || K2 || ...
 */
static void derive_key(const uint8_t *k, uint32_t k_len,
		       const uint8_t h[32], char letter,
		       const uint8_t session_id[32],
		       uint8_t *out, uint32_t out_len)
{
	struct anx_sha256_ctx ctx;
	uint8_t hdr[4];
	uint8_t block[32];
	uint32_t produced = 0;
	uint8_t mpint_buf[64];
	struct ssh_buf mb;

	buf_init(&mb, mpint_buf, sizeof(mpint_buf));
	buf_mpint(&mb, k, k_len);

	anx_sha256_init(&ctx);
	anx_sha256_update(&ctx, mb.data, mb.len);
	anx_sha256_update(&ctx, h, 32);
	{
		uint8_t lb = (uint8_t)letter;

		anx_sha256_update(&ctx, &lb, 1);
	}
	put_u32(hdr, 32);	/* session id is 32 bytes, but raw not string */
	(void)hdr;
	anx_sha256_update(&ctx, session_id, 32);
	anx_sha256_final(&ctx, block);

	{
		uint32_t n = out_len < 32 ? out_len : 32;

		anx_memcpy(out, block, n);
		produced = n;
	}

	while (produced < out_len) {
		anx_sha256_init(&ctx);
		anx_sha256_update(&ctx, mb.data, mb.len);
		anx_sha256_update(&ctx, h, 32);
		anx_sha256_update(&ctx, out, produced);
		anx_sha256_final(&ctx, block);

		{
			uint32_t n = out_len - produced;

			if (n > 32) n = 32;
			anx_memcpy(out + produced, block, n);
			produced += n;
		}
	}

	anx_memset(block, 0, sizeof(block));
}

/* Process SSH_MSG_KEX_ECDH_INIT, perform ECDH, send reply + NEWKEYS,
 * receive client NEWKEYS, derive session keys. */
static int sshd_do_kex_ecdh(struct sshd_state *s)
{
	uint8_t *payload;
	uint32_t payload_len;
	int ret;
	uint32_t qc_len;
	uint8_t q_c[32];
	uint8_t k_s[256];
	uint32_t k_s_len;
	uint8_t sig[64];
	uint8_t sig_blob[128];
	uint32_t sig_blob_len;
	uint8_t *reply;
	struct ssh_buf rb;

	/* --- Read SSH_MSG_KEX_ECDH_INIT (30) --- */
	ret = sshd_recv_packet(s, &payload, &payload_len);
	if (ret != ANX_OK)
		return ret;

	if (payload_len < 1 + 4 + 32 || payload[0] != SSH_MSG_KEX_ECDH_INIT) {
		anx_free(payload);
		return ANX_EIO;
	}
	qc_len = get_u32(payload + 1);
	if (qc_len != 32 || payload_len < 1 + 4 + qc_len) {
		anx_free(payload);
		return ANX_EIO;
	}
	anx_memcpy(q_c, payload + 5, 32);
	anx_free(payload);

	/* --- Generate ephemeral keypair --- */
	anx_random_bytes(s->eph_priv, 32);
	/* Curve25519 clamping is handled inside anx_curve25519_base */
	anx_curve25519_base(s->eph_pub, s->eph_priv);

	/* --- Shared secret K --- */
	anx_curve25519(s->K, s->eph_priv, q_c);
	s->K_len = 32;

	/* --- Host key blob K_S --- */
	k_s_len = build_host_key_blob(s->host_pub, k_s, sizeof(k_s));
	if (k_s_len == 0)
		return ANX_ENOMEM;

	/* --- Exchange hash H --- */
	compute_exchange_hash(s, k_s, k_s_len, q_c, 32,
			      s->eph_pub, 32, s->K, s->K_len, s->H);

	/* First H becomes session_id (sticky across rekey) */
	if (!s->have_session_id) {
		anx_memcpy(s->session_id, s->H, 32);
		s->have_session_id = true;
	}

	/* --- Sign H with host Ed25519 key --- */
	anx_ed25519_sign(sig, s->H, 32, s->host_priv);

	sig_blob_len = build_signature_blob(sig, sig_blob, sizeof(sig_blob));
	if (sig_blob_len == 0)
		return ANX_ENOMEM;

	/* --- Build SSH_MSG_KEX_ECDH_REPLY --- */
	reply = anx_alloc(512);
	if (!reply)
		return ANX_ENOMEM;
	buf_init(&rb, reply, 512);
	buf_u8(&rb, SSH_MSG_KEX_ECDH_REPLY);
	buf_string(&rb, k_s, k_s_len);
	buf_string(&rb, s->eph_pub, 32);
	buf_string(&rb, sig_blob, sig_blob_len);
	if (rb.overflow) {
		anx_free(reply);
		return ANX_ENOMEM;
	}

	ret = sshd_send_packet(s, rb.data, rb.len);
	anx_free(reply);
	if (ret != ANX_OK)
		return ret;

	/* --- Send SSH_MSG_NEWKEYS --- */
	{
		uint8_t msg = SSH_MSG_NEWKEYS;

		ret = sshd_send_packet(s, &msg, 1);
		if (ret != ANX_OK)
			return ret;
	}

	/* Enable s2c encryption starting with the next packet */
	/* Derive keys. For chacha20-poly1305@openssh.com we need 64 bytes
	 * of "encryption key" for each direction. IVs and MAC keys are
	 * unused (the MAC key is derived implicitly from the encryption
	 * key per packet). Letters per RFC 4253 section 7.2:
	 *   A = IV c2s       C = enc key c2s     E = mac key c2s
	 *   B = IV s2c       D = enc key s2c     F = mac key s2c
	 */
	derive_key(s->K, s->K_len, s->H, 'C', s->session_id,
		   s->key_c2s, 64);
	derive_key(s->K, s->K_len, s->H, 'D', s->session_id,
		   s->key_s2c, 64);

	s->encrypted_s2c = true;

	/* --- Wait for client's SSH_MSG_NEWKEYS --- */
	ret = sshd_recv_packet(s, &payload, &payload_len);
	if (ret != ANX_OK)
		return ret;
	if (payload_len < 1 || payload[0] != SSH_MSG_NEWKEYS) {
		anx_free(payload);
		return ANX_EIO;
	}
	anx_free(payload);

	s->encrypted_c2s = true;
	return ANX_OK;
}

/* --- Service + authentication --- */

/* Extract a string (uint32 length + bytes) from a payload cursor */
static int pkt_get_string(const uint8_t *pl, uint32_t pl_len, uint32_t *off,
			  const uint8_t **out, uint32_t *out_len)
{
	uint32_t len;

	if (*off + 4 > pl_len)
		return ANX_EIO;
	len = get_u32(pl + *off);
	*off += 4;
	if (*off + len > pl_len)
		return ANX_EIO;
	*out = pl + *off;
	*out_len = len;
	*off += len;
	return ANX_OK;
}

static bool str_eq(const uint8_t *a, uint32_t a_len, const char *b)
{
	uint32_t bl = (uint32_t)anx_strlen(b);
	uint32_t i;

	if (a_len != bl)
		return false;
	for (i = 0; i < a_len; i++)
		if (a[i] != (uint8_t)b[i])
			return false;
	return true;
}

/* Check submitted password against configured value.
 * Priority: credential "ssh-password" if present, else hardcoded default. */
static bool check_password(const uint8_t *p, uint32_t len)
{
	char expected[128];
	uint32_t actual;
	uint32_t i;

	if (anx_credential_exists("ssh-password")) {
		int r = anx_credential_read("ssh-password", expected,
					     sizeof(expected) - 1, &actual);

		if (r == ANX_OK && actual > 0) {
			bool ok;

			if (actual != len) {
				anx_memset(expected, 0, sizeof(expected));
				return false;
			}
			ok = true;
			for (i = 0; i < actual; i++)
				if ((uint8_t)expected[i] != p[i])
					ok = false;
			anx_memset(expected, 0, sizeof(expected));
			return ok;
		}
	}

	{
		const char *def = SSHD_DEFAULT_PASSWORD;
		uint32_t dl = (uint32_t)anx_strlen(def);
		bool ok;

		if (dl != len)
			return false;
		ok = true;
		for (i = 0; i < dl; i++)
			if ((uint8_t)def[i] != p[i])
				ok = false;
		return ok;
	}
}

static int sshd_send_service_accept(struct sshd_state *s, const char *name)
{
	uint8_t buf[128];
	struct ssh_buf b;

	buf_init(&b, buf, sizeof(buf));
	buf_u8(&b, SSH_MSG_SERVICE_ACCEPT);
	buf_cstr(&b, name);
	if (b.overflow)
		return ANX_ENOMEM;
	return sshd_send_packet(s, b.data, b.len);
}

static int sshd_send_userauth_failure(struct sshd_state *s)
{
	uint8_t buf[64];
	struct ssh_buf b;

	buf_init(&b, buf, sizeof(buf));
	buf_u8(&b, SSH_MSG_USERAUTH_FAILURE);
	buf_cstr(&b, "publickey,password,none");
	buf_u8(&b, 0);	/* partial_success */
	if (b.overflow)
		return ANX_ENOMEM;
	return sshd_send_packet(s, b.data, b.len);
}

static int sshd_send_userauth_success(struct sshd_state *s)
{
	uint8_t msg = SSH_MSG_USERAUTH_SUCCESS;

	return sshd_send_packet(s, &msg, 1);
}

static int sshd_do_service_and_auth(struct sshd_state *s)
{
	uint8_t *payload;
	uint32_t payload_len;
	uint32_t off;
	const uint8_t *str;
	uint32_t str_len;
	int ret;

	/* --- First service request should be ssh-userauth --- */
	ret = sshd_recv_packet(s, &payload, &payload_len);
	if (ret != ANX_OK)
		return ret;
	if (payload_len < 1 || payload[0] != SSH_MSG_SERVICE_REQUEST) {
		anx_free(payload);
		return ANX_EIO;
	}
	off = 1;
	if (pkt_get_string(payload, payload_len, &off, &str, &str_len) != ANX_OK ||
	    !str_eq(str, str_len, "ssh-userauth")) {
		anx_free(payload);
		sshd_send_disconnect(s, SSH_DISCONNECT_SERVICE_NOT_AVAILABLE,
				     "bad service");
		return ANX_EIO;
	}
	anx_free(payload);

	ret = sshd_send_service_accept(s, "ssh-userauth");
	if (ret != ANX_OK)
		return ret;

	/* --- Authentication loop --- */
	for (;;) {
		const uint8_t *user, *svc, *method;
		uint32_t user_len, svc_len, method_len;

		ret = sshd_recv_packet(s, &payload, &payload_len);
		if (ret != ANX_OK)
			return ret;
		if (payload_len < 1 ||
		    payload[0] != SSH_MSG_USERAUTH_REQUEST) {
			anx_free(payload);
			return ANX_EIO;
		}
		off = 1;
		if (pkt_get_string(payload, payload_len, &off,
				    &user, &user_len) != ANX_OK ||
		    pkt_get_string(payload, payload_len, &off,
				    &svc, &svc_len) != ANX_OK ||
		    pkt_get_string(payload, payload_len, &off,
				    &method, &method_len) != ANX_OK) {
			anx_free(payload);
			return ANX_EIO;
		}

		if (str_eq(method, method_len, "none")) {
			/* 'none' auth always fails — its only purpose is to
			 * elicit the list of supported methods from us via
			 * SSH_MSG_USERAUTH_FAILURE. */
			anx_free(payload);
			ret = sshd_send_userauth_failure(s);
			if (ret != ANX_OK)
				return ret;
			continue;
		}

		if (str_eq(method, method_len, "password")) {
			const uint8_t *pw;
			uint32_t pw_len;

			/* bool change_password */
			if (off >= payload_len) {
				anx_free(payload);
				return ANX_EIO;
			}
			off++;
			if (pkt_get_string(payload, payload_len, &off,
					    &pw, &pw_len) != ANX_OK) {
				anx_free(payload);
				return ANX_EIO;
			}

			if (check_password(pw, pw_len)) {
				kprintf("sshd: password auth OK for '");
				{
					uint32_t i;

					for (i = 0; i < user_len && i < 64; i++)
						kputc((char)user[i]);
				}
				kprintf("'\n");
				anx_free(payload);
				return sshd_send_userauth_success(s);
			}

			kprintf("sshd: password auth failed\n");
			anx_free(payload);
			ret = sshd_send_userauth_failure(s);
			if (ret != ANX_OK)
				return ret;
			continue;
		}

		if (str_eq(method, method_len, "publickey")) {
			const uint8_t *algo;
			uint32_t algo_len;
			const uint8_t *pk_blob;
			uint32_t pk_blob_len;
			uint8_t has_sig;

			/* byte has_signature; string algo; string pk_blob */
			if (off >= payload_len) {
				anx_free(payload);
				return ANX_EIO;
			}
			has_sig = payload[off++];
			if (pkt_get_string(payload, payload_len, &off,
					    &algo, &algo_len) != ANX_OK ||
			    pkt_get_string(payload, payload_len, &off,
					    &pk_blob, &pk_blob_len) != ANX_OK) {
				anx_free(payload);
				return ANX_EIO;
			}

			/* We only support ssh-ed25519 for now */
			if (!str_eq(algo, algo_len, "ssh-ed25519")) {
				anx_free(payload);
				ret = sshd_send_userauth_failure(s);
				if (ret != ANX_OK)
					return ret;
				continue;
			}

			/* pk_blob is: string "ssh-ed25519"; string pubkey(32) */
			{
				uint32_t pbo = 0;
				const uint8_t *pk_algo;
				uint32_t pk_algo_len;
				const uint8_t *pk;
				uint32_t pk_len;

				if (pkt_get_string(pk_blob, pk_blob_len, &pbo,
						    &pk_algo, &pk_algo_len)
					    != ANX_OK ||
				    pkt_get_string(pk_blob, pk_blob_len, &pbo,
						    &pk, &pk_len) != ANX_OK ||
				    !str_eq(pk_algo, pk_algo_len, "ssh-ed25519") ||
				    pk_len != 32) {
					anx_free(payload);
					ret = sshd_send_userauth_failure(s);
					if (ret != ANX_OK)
						return ret;
					continue;
				}

				if (!has_sig) {
					/* Query: tell client we accept this key
					 * type.  Client will follow up with a
					 * signed request. */
					uint8_t ok[256];
					struct ssh_buf ob;

					buf_init(&ob, ok, sizeof(ok));
					buf_u8(&ob, SSH_MSG_USERAUTH_PK_OK);
					buf_string(&ob, algo, algo_len);
					buf_string(&ob, pk_blob, pk_blob_len);
					anx_free(payload);
					ret = sshd_send_packet(s, ob.data,
							       ob.len);
					if (ret != ANX_OK)
						return ret;
					continue;
				}

				/* Real auth request — verify signature.
				 *
				 * Data that was signed (RFC 4252 §7):
				 *   string session_id
				 *   byte   SSH_MSG_USERAUTH_REQUEST
				 *   string user
				 *   string "ssh-connection"
				 *   string "publickey"
				 *   bool   TRUE
				 *   string algo
				 *   string pk_blob
				 */
				{
					const uint8_t *sig_blob;
					uint32_t sig_blob_len;
					uint8_t *signed_data;
					uint32_t sd_cap;
					uint32_t sd_len = 0;
					uint8_t hdr[4];
					uint8_t ed_sig[64];
					uint32_t sbo = 0;
					const uint8_t *sig_algo;
					uint32_t sig_algo_len;
					const uint8_t *sig_bytes;
					uint32_t sig_bytes_len;
					int verify_ret;

					if (pkt_get_string(payload, payload_len,
							    &off, &sig_blob,
							    &sig_blob_len)
						    != ANX_OK) {
						anx_free(payload);
						return ANX_EIO;
					}

					/* Parse signature blob:
					 *   string "ssh-ed25519"
					 *   string sig(64)
					 */
					if (pkt_get_string(sig_blob, sig_blob_len,
							    &sbo, &sig_algo,
							    &sig_algo_len)
						    != ANX_OK ||
					    pkt_get_string(sig_blob, sig_blob_len,
							    &sbo, &sig_bytes,
							    &sig_bytes_len)
						    != ANX_OK ||
					    !str_eq(sig_algo, sig_algo_len,
						    "ssh-ed25519") ||
					    sig_bytes_len != 64) {
						anx_free(payload);
						ret = sshd_send_userauth_failure(s);
						if (ret != ANX_OK)
							return ret;
						continue;
					}
					anx_memcpy(ed_sig, sig_bytes, 64);

					/* Build signed_data */
					sd_cap = 4 + 32		/* session_id */
					       + 1		/* msg byte */
					       + 4 + user_len
					       + 4 + 14		/* ssh-connection */
					       + 4 + 9		/* publickey */
					       + 1		/* TRUE */
					       + 4 + algo_len
					       + 4 + pk_blob_len
					       + 64;
					signed_data = anx_alloc(sd_cap);
					if (!signed_data) {
						anx_free(payload);
						return ANX_ENOMEM;
					}

					put_u32(hdr, 32);
					anx_memcpy(signed_data + sd_len, hdr, 4);
					sd_len += 4;
					anx_memcpy(signed_data + sd_len,
						   s->session_id, 32);
					sd_len += 32;

					signed_data[sd_len++] =
						SSH_MSG_USERAUTH_REQUEST;

					put_u32(hdr, user_len);
					anx_memcpy(signed_data + sd_len, hdr, 4);
					sd_len += 4;
					anx_memcpy(signed_data + sd_len,
						   user, user_len);
					sd_len += user_len;

					put_u32(hdr, 14);
					anx_memcpy(signed_data + sd_len, hdr, 4);
					sd_len += 4;
					anx_memcpy(signed_data + sd_len,
						   "ssh-connection", 14);
					sd_len += 14;

					put_u32(hdr, 9);
					anx_memcpy(signed_data + sd_len, hdr, 4);
					sd_len += 4;
					anx_memcpy(signed_data + sd_len,
						   "publickey", 9);
					sd_len += 9;

					signed_data[sd_len++] = 1;	/* TRUE */

					put_u32(hdr, algo_len);
					anx_memcpy(signed_data + sd_len, hdr, 4);
					sd_len += 4;
					anx_memcpy(signed_data + sd_len,
						   algo, algo_len);
					sd_len += algo_len;

					put_u32(hdr, pk_blob_len);
					anx_memcpy(signed_data + sd_len, hdr, 4);
					sd_len += 4;
					anx_memcpy(signed_data + sd_len,
						   pk_blob, pk_blob_len);
					sd_len += pk_blob_len;

					verify_ret = anx_ed25519_verify(
						ed_sig, signed_data, sd_len, pk);
					anx_free(signed_data);

					if (verify_ret == 0) {
						/*
						 * Signature valid — now check
						 * authorized_keys credential.
						 * If no credential exists we
						 * warn and accept (dev mode).
						 * If it exists, key must match.
						 */
						uint8_t authkeys[ANX_AUTHORIZED_KEYS_MAX * 32];
						uint32_t ak_len = 0;
						uint32_t ki;
						bool authorized = false;

						if (anx_credential_exists("ssh-authorized-keys")) {
							anx_credential_read(
								"ssh-authorized-keys",
								authkeys,
								sizeof(authkeys),
								&ak_len);
							if (ak_len % 32 != 0)
								ak_len = 0;
							for (ki = 0; ki + 32 <= ak_len; ki += 32) {
								if (anx_memcmp(authkeys + ki, pk, 32) == 0) {
									authorized = true;
									break;
								}
							}
						} else {
							/* No authorized_keys — dev mode, accept with warning */
							kprintf("sshd: WARNING: no authorized_keys set, accepting any valid key\n");
							authorized = true;
						}

						if (!authorized) {
							kprintf("sshd: pubkey not in authorized_keys\n");
							anx_free(signed_data);
							anx_free(payload);
							ret = sshd_send_userauth_failure(s);
							if (ret != ANX_OK)
								return ret;
							continue;
						}

						{
							uint8_t fp[32];
							uint32_t i;

							anx_sha256(pk_blob, pk_blob_len, fp);
							kprintf("sshd: pubkey auth OK for '");
							for (i = 0; i < user_len && i < 64; i++)
								kputc((char)user[i]);
							kprintf("' fp=SHA256:");
							for (i = 0; i < 8; i++)
								kprintf("%02x", fp[i]);
							kprintf("...\n");
						}
						anx_free(payload);
						return sshd_send_userauth_success(s);
					}
					kprintf("sshd: pubkey signature verify failed\n");
				}
			}

			anx_free(payload);
			ret = sshd_send_userauth_failure(s);
			if (ret != ANX_OK)
				return ret;
			continue;
		}

		/* Unknown method — send failure and continue */
		(void)svc; (void)svc_len;
		anx_free(payload);
		ret = sshd_send_userauth_failure(s);
		if (ret != ANX_OK)
			return ret;
	}
}

/* --- Connection phase (channels) --- */

static int sshd_send_channel_data(struct sshd_state *s, const void *data,
				  uint32_t len)
{
	uint8_t *buf;
	struct ssh_buf b;
	uint32_t chunk;
	int ret;
	const uint8_t *p = (const uint8_t *)data;

	/* Split into chunks that fit within the client's max packet size. */
	while (len > 0) {
		chunk = len;
		if (chunk > s->client_max_packet - 64)
			chunk = s->client_max_packet - 64;
		if (chunk > SSHD_MAX_PAYLOAD - 64)
			chunk = SSHD_MAX_PAYLOAD - 64;
		if (chunk > s->client_window)
			chunk = s->client_window;
		if (chunk == 0)
			return ANX_EIO;

		buf = anx_alloc(chunk + 32);
		if (!buf)
			return ANX_ENOMEM;
		buf_init(&b, buf, chunk + 32);
		buf_u8(&b, SSH_MSG_CHANNEL_DATA);
		buf_u32(&b, s->client_channel);
		buf_string(&b, p, chunk);
		if (b.overflow) {
			anx_free(buf);
			return ANX_ENOMEM;
		}
		ret = sshd_send_packet(s, b.data, b.len);
		anx_free(buf);
		if (ret != ANX_OK)
			return ret;

		s->client_window -= chunk;
		p += chunk;
		len -= chunk;
	}
	return ANX_OK;
}

static int sshd_send_channel_eof(struct sshd_state *s)
{
	uint8_t buf[8];
	struct ssh_buf b;

	buf_init(&b, buf, sizeof(buf));
	buf_u8(&b, SSH_MSG_CHANNEL_EOF);
	buf_u32(&b, s->client_channel);
	return sshd_send_packet(s, b.data, b.len);
}

static int sshd_send_channel_close(struct sshd_state *s)
{
	uint8_t buf[8];
	struct ssh_buf b;

	buf_init(&b, buf, sizeof(buf));
	buf_u8(&b, SSH_MSG_CHANNEL_CLOSE);
	buf_u32(&b, s->client_channel);
	return sshd_send_packet(s, b.data, b.len);
}

static int sshd_send_exit_status(struct sshd_state *s, uint32_t status)
{
	uint8_t buf[64];
	struct ssh_buf b;

	buf_init(&b, buf, sizeof(buf));
	buf_u8(&b, SSH_MSG_CHANNEL_REQUEST);
	buf_u32(&b, s->client_channel);
	buf_cstr(&b, "exit-status");
	buf_u8(&b, 0);		/* want_reply */
	buf_u32(&b, status);
	return sshd_send_packet(s, b.data, b.len);
}

static int sshd_send_channel_success(struct sshd_state *s)
{
	uint8_t buf[8];
	struct ssh_buf b;

	buf_init(&b, buf, sizeof(buf));
	buf_u8(&b, SSH_MSG_CHANNEL_SUCCESS);
	buf_u32(&b, s->client_channel);
	return sshd_send_packet(s, b.data, b.len);
}

static int sshd_send_channel_failure(struct sshd_state *s)
{
	uint8_t buf[8];
	struct ssh_buf b;

	buf_init(&b, buf, sizeof(buf));
	buf_u8(&b, SSH_MSG_CHANNEL_FAILURE);
	buf_u32(&b, s->client_channel);
	return sshd_send_packet(s, b.data, b.len);
}

/* Run a single command line through the kernel shell and send the
 * captured kprintf output back as CHANNEL_DATA. */
static int sshd_exec_and_send(struct sshd_state *s, const char *cmd)
{
	char *capture;
	uint32_t captured;
	int ret;

	capture = anx_alloc(SSHD_CAPTURE_SIZE);
	if (!capture)
		return ANX_ENOMEM;

	anx_kprintf_capture_start(capture, SSHD_CAPTURE_SIZE);
	anx_shell_execute(cmd);
	captured = anx_kprintf_capture_stop();

	/* Translate lone \n into \r\n for terminal clients */
	{
		char *tx;
		uint32_t i, j = 0;

		tx = anx_alloc(captured * 2 + 1);
		if (!tx) {
			anx_free(capture);
			return ANX_ENOMEM;
		}
		for (i = 0; i < captured; i++) {
			if (capture[i] == '\n' &&
			    (i == 0 || capture[i - 1] != '\r')) {
				tx[j++] = '\r';
				tx[j++] = '\n';
			} else {
				tx[j++] = capture[i];
			}
		}
		ret = sshd_send_channel_data(s, tx, j);
		anx_free(tx);
	}

	anx_free(capture);
	return ret;
}

/* Interactive shell loop: read CHANNEL_DATA, assemble lines, execute. */
static int sshd_shell_loop(struct sshd_state *s)
{
	uint8_t *payload;
	uint32_t payload_len;
	int ret;

	/* Greet the user */
	{
		const char *greet =
			"Anunix kernel shell — type 'help' for commands.\r\nanx> ";

		ret = sshd_send_channel_data(s, greet,
					      (uint32_t)anx_strlen(greet));
		if (ret != ANX_OK)
			return ret;
	}

	while (s->channel_open) {
		ret = sshd_recv_packet(s, &payload, &payload_len);
		if (ret != ANX_OK)
			return ret;
		if (payload_len < 1) {
			anx_free(payload);
			continue;
		}

		switch (payload[0]) {
		case SSH_MSG_CHANNEL_DATA: {
			uint32_t off = 1;
			uint32_t ch;
			const uint8_t *data;
			uint32_t data_len;
			uint32_t i;

			if (off + 4 > payload_len) break;
			ch = get_u32(payload + off); off += 4;
			(void)ch;
			if (pkt_get_string(payload, payload_len, &off,
					    &data, &data_len) != ANX_OK)
				break;

			for (i = 0; i < data_len; i++) {
				char c = (char)data[i];

				if (c == '\r' || c == '\n') {
					/* Echo newline */
					sshd_send_channel_data(s, "\r\n", 2);
					s->line[s->line_len] = '\0';

					if (s->line_len > 0) {
						sshd_exec_and_send(s, s->line);
					}
					s->line_len = 0;
					sshd_send_channel_data(s, "anx> ", 5);
					continue;
				}
				if (c == 0x7F || c == 0x08) {
					if (s->line_len > 0) {
						s->line_len--;
						sshd_send_channel_data(s,
							"\b \b", 3);
					}
					continue;
				}
				if (c == 0x03) {
					/* Ctrl-C: discard line */
					sshd_send_channel_data(s, "^C\r\n", 4);
					s->line_len = 0;
					sshd_send_channel_data(s, "anx> ", 5);
					continue;
				}
				if (c == 0x04) {
					/* Ctrl-D: close */
					sshd_send_channel_data(s,
						"logout\r\n", 8);
					sshd_send_channel_eof(s);
					sshd_send_exit_status(s, 0);
					sshd_send_channel_close(s);
					s->channel_open = false;
					break;
				}
				if (s->line_len + 1 < SSHD_LINE_MAX &&
				    (uint8_t)c >= 0x20) {
					s->line[s->line_len++] = c;
					/* Echo character */
					sshd_send_channel_data(s, &c, 1);
				}
			}
			break;
		}

		case SSH_MSG_CHANNEL_WINDOW_ADJUST: {
			uint32_t off = 1;
			uint32_t ch, add;

			if (off + 8 > payload_len) break;
			ch = get_u32(payload + off); off += 4;
			add = get_u32(payload + off);
			(void)ch;
			s->client_window += add;
			break;
		}

		case SSH_MSG_CHANNEL_EOF:
		case SSH_MSG_CHANNEL_CLOSE:
			sshd_send_channel_close(s);
			s->channel_open = false;
			break;

		case SSH_MSG_GLOBAL_REQUEST: {
			/* Reply "request failure" to any global request */
			uint8_t msg = SSH_MSG_REQUEST_FAILURE;

			sshd_send_packet(s, &msg, 1);
			break;
		}

		default:
			/* Ignore unknown messages */
			break;
		}

		anx_free(payload);
	}
	return ANX_OK;
}

static int sshd_do_connection(struct sshd_state *s)
{
	uint8_t *payload;
	uint32_t payload_len;
	uint32_t off;
	const uint8_t *str;
	uint32_t str_len;
	int ret;

	/* --- Expect ssh-connection service request --- */
	ret = sshd_recv_packet(s, &payload, &payload_len);
	if (ret != ANX_OK)
		return ret;

	/* Some clients send channel open straight away without a second
	 * service request — handle either path. */
	if (payload_len >= 1 && payload[0] == SSH_MSG_SERVICE_REQUEST) {
		off = 1;
		if (pkt_get_string(payload, payload_len, &off,
				    &str, &str_len) == ANX_OK &&
		    str_eq(str, str_len, "ssh-connection")) {
			anx_free(payload);
			ret = sshd_send_service_accept(s, "ssh-connection");
			if (ret != ANX_OK)
				return ret;
			ret = sshd_recv_packet(s, &payload, &payload_len);
			if (ret != ANX_OK)
				return ret;
		} else {
			anx_free(payload);
			return ANX_EIO;
		}
	}

	/* Skip global requests that OpenSSH sends before CHANNEL_OPEN
	 * (e.g. "no-more-sessions@openssh.com"). */
	while (payload_len >= 1 &&
	       payload[0] == SSH_MSG_GLOBAL_REQUEST) {
		uint8_t want;
		anx_free(payload);
		payload = NULL;
		/* want_reply is byte at offset 5 (after type + 4-byte name len) */
		if (payload_len >= 6) {
			want = 0; /* can't read after free — reply failure anyway */
		}
		(void)want;
		/* send request-failure if want_reply (safe to always send) */
		{
			uint8_t msg = SSH_MSG_REQUEST_FAILURE;
			sshd_send_packet(s, &msg, 1);
		}
		ret = sshd_recv_packet(s, &payload, &payload_len);
		if (ret != ANX_OK)
			return ret;
	}

	/* --- Expect SSH_MSG_CHANNEL_OPEN --- */
	if (payload_len < 1 || payload[0] != SSH_MSG_CHANNEL_OPEN) {
		kprintf("sshd: expected CHANNEL_OPEN, got msg %u\n",
			payload_len >= 1 ? (unsigned)payload[0] : 0);
		anx_free(payload);
		return ANX_EIO;
	}

	off = 1;
	if (pkt_get_string(payload, payload_len, &off,
			    &str, &str_len) != ANX_OK) {
		anx_free(payload);
		return ANX_EIO;
	}

	if (!str_eq(str, str_len, "session")) {
		uint8_t buf[64];
		struct ssh_buf b;
		uint32_t sender_ch = 0;

		if (off + 4 <= payload_len)
			sender_ch = get_u32(payload + off);
		anx_free(payload);

		buf_init(&b, buf, sizeof(buf));
		buf_u8(&b, SSH_MSG_CHANNEL_OPEN_FAILURE);
		buf_u32(&b, sender_ch);
		buf_u32(&b, SSH_OPEN_UNKNOWN_CHANNEL_TYPE);
		buf_cstr(&b, "only session channels supported");
		buf_cstr(&b, "");
		sshd_send_packet(s, b.data, b.len);
		return ANX_EIO;
	}

	if (off + 12 > payload_len) {
		anx_free(payload);
		return ANX_EIO;
	}
	s->client_channel = get_u32(payload + off); off += 4;
	s->client_window = get_u32(payload + off); off += 4;
	s->client_max_packet = get_u32(payload + off); off += 4;
	s->server_channel = 0;
	s->channel_open = true;
	anx_free(payload);

	/* Cap the client's advertised max packet to our buffer size */
	if (s->client_max_packet > SSHD_MAX_PACKET)
		s->client_max_packet = SSHD_MAX_PACKET;
	if (s->client_max_packet < 256)
		s->client_max_packet = 256;

	/* --- Reply with CHANNEL_OPEN_CONFIRMATION --- */
	{
		uint8_t buf[64];
		struct ssh_buf b;

		buf_init(&b, buf, sizeof(buf));
		buf_u8(&b, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
		buf_u32(&b, s->client_channel);	/* recipient = client ch */
		buf_u32(&b, s->server_channel);	/* sender = server ch */
		buf_u32(&b, SSHD_WINDOW_SIZE);
		buf_u32(&b, SSHD_MAX_PACKET_SIZE);
		ret = sshd_send_packet(s, b.data, b.len);
		if (ret != ANX_OK)
			return ret;
	}

	/* --- Channel request loop --- */
	while (s->channel_open) {
		ret = sshd_recv_packet(s, &payload, &payload_len);
		if (ret != ANX_OK)
			return ret;
		if (payload_len < 1) {
			anx_free(payload);
			continue;
		}

		if (payload[0] == SSH_MSG_CHANNEL_REQUEST) {
			const uint8_t *rtype;
			uint32_t rtype_len;
			uint8_t want_reply;

			off = 1;
			if (off + 4 > payload_len) {
				anx_free(payload);
				return ANX_EIO;
			}
			/* recipient_channel */
			off += 4;
			if (pkt_get_string(payload, payload_len, &off,
					    &rtype, &rtype_len) != ANX_OK) {
				anx_free(payload);
				return ANX_EIO;
			}
			if (off >= payload_len) {
				anx_free(payload);
				return ANX_EIO;
			}
			want_reply = payload[off++];

			if (str_eq(rtype, rtype_len, "pty-req")) {
				if (want_reply)
					sshd_send_channel_success(s);
				anx_free(payload);
				continue;
			}

			if (str_eq(rtype, rtype_len, "env")) {
				/* Accept env assignments silently */
				if (want_reply)
					sshd_send_channel_success(s);
				anx_free(payload);
				continue;
			}

			if (str_eq(rtype, rtype_len, "shell")) {
				if (want_reply)
					sshd_send_channel_success(s);
				s->shell_mode = true;
				anx_free(payload);
				ret = sshd_shell_loop(s);
				return ret;
			}

			if (str_eq(rtype, rtype_len, "exec")) {
				const uint8_t *cmd;
				uint32_t cmd_len;
				char cmd_buf[SSHD_LINE_MAX];

				if (pkt_get_string(payload, payload_len,
						    &off, &cmd, &cmd_len) != ANX_OK) {
					anx_free(payload);
					return ANX_EIO;
				}
				if (want_reply)
					sshd_send_channel_success(s);

				if (cmd_len >= sizeof(cmd_buf))
					cmd_len = sizeof(cmd_buf) - 1;
				anx_memcpy(cmd_buf, cmd, cmd_len);
				cmd_buf[cmd_len] = '\0';
				anx_free(payload);

				sshd_exec_and_send(s, cmd_buf);
				sshd_send_channel_eof(s);
				sshd_send_exit_status(s, 0);
				sshd_send_channel_close(s);
				s->channel_open = false;
				continue;
			}

			if (str_eq(rtype, rtype_len, "subsystem")) {
				if (want_reply)
					sshd_send_channel_failure(s);
				anx_free(payload);
				continue;
			}

			/* Unknown request type */
			if (want_reply)
				sshd_send_channel_failure(s);
			anx_free(payload);
			continue;
		}

		if (payload[0] == SSH_MSG_CHANNEL_DATA ||
		    payload[0] == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
			/* Data before shell request — ignore */
			anx_free(payload);
			continue;
		}

		if (payload[0] == SSH_MSG_CHANNEL_EOF ||
		    payload[0] == SSH_MSG_CHANNEL_CLOSE) {
			anx_free(payload);
			sshd_send_channel_close(s);
			s->channel_open = false;
			break;
		}

		if (payload[0] == SSH_MSG_GLOBAL_REQUEST) {
			uint8_t msg = SSH_MSG_REQUEST_FAILURE;

			anx_free(payload);
			sshd_send_packet(s, &msg, 1);
			continue;
		}

		/* Ignore everything else */
		anx_free(payload);
	}
	return ANX_OK;
}

/* --- Session driver --- */

static void sshd_state_free(struct sshd_state *s)
{
	if (s->i_client) anx_free(s->i_client);
	if (s->i_server) anx_free(s->i_server);
	anx_memset(s->host_priv, 0, sizeof(s->host_priv));
	anx_memset(s->eph_priv, 0, sizeof(s->eph_priv));
	anx_memset(s->K, 0, sizeof(s->K));
	anx_memset(s->key_c2s, 0, sizeof(s->key_c2s));
	anx_memset(s->key_s2c, 0, sizeof(s->key_s2c));
	anx_free(s);
}

static void sshd_handle_session(struct anx_tcp_conn *conn)
{
	struct sshd_state *s;
	int ret;

	s = anx_zalloc(sizeof(*s));
	if (!s) {
		anx_tcp_srv_close(conn);
		return;
	}
	s->conn = conn;
	s->phase = SSHD_PHASE_VERSION;

	if (ensure_host_key(s->host_pub, s->host_priv) != ANX_OK) {
		kprintf("sshd: host key init failed\n");
		goto done;
	}

	ret = sshd_send_version(s);
	if (ret != ANX_OK) goto done;
	ret = sshd_recv_version(s);
	if (ret != ANX_OK) {
		kprintf("sshd: failed to read client version\n");
		goto done;
	}

	kprintf("sshd: client '%s'\n", s->v_client);

	s->phase = SSHD_PHASE_KEX;
	ret = sshd_send_kexinit(s);
	if (ret != ANX_OK) goto done;
	ret = sshd_recv_kexinit(s);
	if (ret != ANX_OK) {
		kprintf("sshd: kexinit failed\n");
		goto done;
	}
	ret = sshd_do_kex_ecdh(s);
	if (ret != ANX_OK) {
		kprintf("sshd: kex ecdh failed\n");
		goto done;
	}

	s->phase = SSHD_PHASE_AUTH;
	ret = sshd_do_service_and_auth(s);
	if (ret != ANX_OK) {
		kprintf("sshd: auth failed\n");
		goto done;
	}

	s->phase = SSHD_PHASE_CONNECTION;
	ret = sshd_do_connection(s);
	kprintf("sshd: do_connection ret=%d\n", ret);
	if (ret != ANX_OK) {
		kprintf("sshd: connection ended (%d)\n", ret);
		sshd_send_disconnect(s, SSH_DISCONNECT_BY_APPLICATION, "bye");
	}
	/*
	 * On clean exec/shell exit: skip SSH_MSG_DISCONNECT. The channel
	 * close sequence (CHANNEL_EOF + EXIT_STATUS + CHANNEL_CLOSE) is
	 * enough — the client drains all buffered output before seeing the
	 * TCP FIN. Sending DISCONNECT races the client reading CHANNEL_DATA
	 * and causes intermittent output loss.
	 */

done:
	anx_tcp_srv_close(conn);
	sshd_state_free(s);
}

/* --- TCP accept callback --- */

static void sshd_accept(struct anx_tcp_conn *conn, void *arg)
{
	(void)arg;
	g_pending_conn = conn;
}

int anx_sshd_init(uint16_t port)
{
	int ret;

	ret = anx_tcp_listen(port, sshd_accept, NULL);
	if (ret != ANX_OK) {
		kprintf("sshd: failed to listen on port %u (%d)\n",
			(uint32_t)port, ret);
		return ret;
	}

	g_sshd_ready = true;
	kprintf("sshd: SSH server listening on port %u\n", (uint32_t)port);
	return ANX_OK;
}

void anx_sshd_poll(void)
{
	if (!g_sshd_ready)
		return;

	if (g_pending_conn) {
		struct anx_tcp_conn *conn = g_pending_conn;

		g_pending_conn = NULL;
		sshd_handle_session(conn);
	}
}
