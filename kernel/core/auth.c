/*
 * auth.c — Multi-key authentication system.
 *
 * Manages user accounts with password and SSH key authentication.
 * Passwords are stored as SHA-256 hashes (computed via a simple
 * software implementation). Sessions track scopes for access control.
 */

#include <anx/types.h>
#include <anx/auth.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/spinlock.h>
#include <anx/kprintf.h>

static struct anx_user users[ANX_MAX_USERS];
static struct anx_session current_session;
static struct anx_spinlock auth_lock = ANX_SPINLOCK_INIT;

/* --- Simple SHA-256 for password hashing --- */

static const uint32_t sha256_k[64] = {
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
#define SIG0(x)		(ROR32(x, 2) ^ ROR32(x, 13) ^ ROR32(x, 22))
#define SIG1(x)		(ROR32(x, 6) ^ ROR32(x, 11) ^ ROR32(x, 25))
#define SIG_S0(x)	(ROR32(x, 7) ^ ROR32(x, 18) ^ ((x) >> 3))
#define SIG_S1(x)	(ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

static void sha256_hash(const void *data, uint32_t len, uint8_t out[32])
{
	uint32_t h[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};
	uint8_t block[64];
	uint32_t w[64];
	uint64_t total_bits = (uint64_t)len * 8;
	const uint8_t *p = (const uint8_t *)data;
	uint32_t remaining = len;
	uint32_t i;
	bool padded = false;

	while (remaining > 0 || !padded) {
		uint32_t chunk = remaining;
		uint32_t a, b, c, d, e, f, g, hh, t1, t2;

		if (chunk > 64)
			chunk = 64;

		anx_memset(block, 0, 64);
		if (chunk > 0)
			anx_memcpy(block, p, chunk);

		if (chunk < 64) {
			block[chunk] = 0x80;
			padded = true;
			if (chunk < 56) {
				/* Room for length */
				for (i = 0; i < 8; i++)
					block[56 + i] = (uint8_t)
						(total_bits >> (56 - i * 8));
			} else {
				/* Process this block, then one more for length */
			}
		}

		/* Parse block into 16 words */
		for (i = 0; i < 16; i++)
			w[i] = ((uint32_t)block[i*4] << 24) |
			       ((uint32_t)block[i*4+1] << 16) |
			       ((uint32_t)block[i*4+2] << 8) |
			       (uint32_t)block[i*4+3];

		/* Extend to 64 words */
		for (i = 16; i < 64; i++)
			w[i] = SIG_S1(w[i-2]) + w[i-7] +
			       SIG_S0(w[i-15]) + w[i-16];

		/* Compress */
		a = h[0]; b = h[1]; c = h[2]; d = h[3];
		e = h[4]; f = h[5]; g = h[6]; hh = h[7];

		for (i = 0; i < 64; i++) {
			t1 = hh + SIG1(e) + CH(e, f, g) + sha256_k[i] + w[i];
			t2 = SIG0(a) + MAJ(a, b, c);
			hh = g; g = f; f = e; e = d + t1;
			d = c; c = b; b = a; a = t1 + t2;
		}

		h[0] += a; h[1] += b; h[2] += c; h[3] += d;
		h[4] += e; h[5] += f; h[6] += g; h[7] += hh;

		p += chunk;
		remaining -= chunk;

		/* If we filled a 64-byte block without padding, continue */
		if (chunk == 64 && remaining == 0 && !padded) {
			/* Need one more block for padding + length */
			anx_memset(block, 0, 64);
			block[0] = 0x80;
			for (i = 0; i < 8; i++)
				block[56 + i] = (uint8_t)
					(total_bits >> (56 - i * 8));
			padded = true;

			for (i = 0; i < 16; i++)
				w[i] = ((uint32_t)block[i*4] << 24) |
				       ((uint32_t)block[i*4+1] << 16) |
				       ((uint32_t)block[i*4+2] << 8) |
				       (uint32_t)block[i*4+3];
			for (i = 16; i < 64; i++)
				w[i] = SIG_S1(w[i-2]) + w[i-7] +
				       SIG_S0(w[i-15]) + w[i-16];

			a = h[0]; b = h[1]; c = h[2]; d = h[3];
			e = h[4]; f = h[5]; g = h[6]; hh = h[7];
			for (i = 0; i < 64; i++) {
				t1 = hh + SIG1(e) + CH(e,f,g) +
				     sha256_k[i] + w[i];
				t2 = SIG0(a) + MAJ(a,b,c);
				hh=g; g=f; f=e; e=d+t1;
				d=c; c=b; b=a; a=t1+t2;
			}
			h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
			h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
		}
	}

	/* Output hash */
	for (i = 0; i < 8; i++) {
		out[i*4+0] = (uint8_t)(h[i] >> 24);
		out[i*4+1] = (uint8_t)(h[i] >> 16);
		out[i*4+2] = (uint8_t)(h[i] >> 8);
		out[i*4+3] = (uint8_t)(h[i]);
	}
}

/* Convert hash to hex string */
static void hash_to_hex(const uint8_t hash[32], char *hex)
{
	static const char digits[] = "0123456789abcdef";
	int i;

	for (i = 0; i < 32; i++) {
		hex[i*2] = digits[(hash[i] >> 4) & 0xF];
		hex[i*2+1] = digits[hash[i] & 0xF];
	}
	hex[64] = '\0';
}

/* Constant-time comparison */
static bool secure_compare(const char *a, const char *b, uint32_t len)
{
	uint8_t diff = 0;
	uint32_t i;

	for (i = 0; i < len; i++)
		diff |= (uint8_t)(a[i] ^ b[i]);
	return diff == 0;
}

/* --- Public API --- */

void anx_auth_init(void)
{
	anx_memset(users, 0, sizeof(users));
	anx_memset(&current_session, 0, sizeof(current_session));
	anx_spin_init(&auth_lock);
}

static struct anx_user *find_user(const char *username)
{
	int i;

	for (i = 0; i < ANX_MAX_USERS; i++) {
		if (users[i].active &&
		    anx_strcmp(users[i].username, username) == 0)
			return &users[i];
	}
	return NULL;
}

int anx_auth_create_user(const char *username)
{
	int i;

	if (!username || anx_strlen(username) == 0)
		return ANX_EINVAL;
	if (find_user(username))
		return ANX_EEXIST;

	for (i = 0; i < ANX_MAX_USERS; i++) {
		if (!users[i].active) {
			anx_memset(&users[i], 0, sizeof(users[i]));
			anx_strlcpy(users[i].username, username,
				     sizeof(users[i].username));
			users[i].active = true;
			return ANX_OK;
		}
	}
	return ANX_ENOMEM;
}

int anx_auth_add_password(const char *username, const char *password,
			   enum anx_key_scope_type scope)
{
	struct anx_user *user;
	struct anx_auth_key *key;
	uint8_t hash[32];
	char hex[65];

	user = find_user(username);
	if (!user)
		return ANX_ENOENT;
	if (user->key_count >= ANX_MAX_KEYS_PER_USER)
		return ANX_ENOMEM;

	sha256_hash(password, (uint32_t)anx_strlen(password), hash);
	hash_to_hex(hash, hex);

	key = &user->keys[user->key_count];
	anx_memset(key, 0, sizeof(*key));
	key->key_type = ANX_AUTH_PASSWORD;
	anx_strlcpy(key->key_data, hex, sizeof(key->key_data));
	key->key_len = 64;
	key->scopes[0].type = scope;
	key->scope_count = 1;
	key->active = true;
	user->key_count++;

	return ANX_OK;
}

int anx_auth_add_ssh_key(const char *username, const char *pubkey,
			  enum anx_key_scope_type scope)
{
	struct anx_user *user;
	struct anx_auth_key *key;

	user = find_user(username);
	if (!user)
		return ANX_ENOENT;
	if (user->key_count >= ANX_MAX_KEYS_PER_USER)
		return ANX_ENOMEM;

	key = &user->keys[user->key_count];
	anx_memset(key, 0, sizeof(*key));
	key->key_type = ANX_AUTH_SSH_KEY;
	anx_strlcpy(key->key_data, pubkey, sizeof(key->key_data));
	key->key_len = (uint32_t)anx_strlen(pubkey);
	key->scopes[0].type = scope;
	key->scope_count = 1;
	key->active = true;
	user->key_count++;

	return ANX_OK;
}

int anx_auth_login_password(const char *username, const char *password,
			     struct anx_session *session)
{
	struct anx_user *user;
	uint8_t hash[32];
	char hex[65];
	uint32_t i;

	user = find_user(username);
	if (!user)
		return ANX_EPERM;

	sha256_hash(password, (uint32_t)anx_strlen(password), hash);
	hash_to_hex(hash, hex);

	for (i = 0; i < user->key_count; i++) {
		struct anx_auth_key *key = &user->keys[i];

		if (!key->active || key->key_type != ANX_AUTH_PASSWORD)
			continue;
		if (secure_compare(key->key_data, hex, 64)) {
			/* Match — create session with this key's scopes */
			anx_memset(session, 0, sizeof(*session));
			anx_strlcpy(session->username, username,
				     sizeof(session->username));
			anx_memcpy(session->scopes, key->scopes,
				   key->scope_count * sizeof(struct anx_key_scope));
			session->scope_count = key->scope_count;
			session->max_scope = key->scopes[0].type;
			session->active = true;

			/* Set as current session */
			current_session = *session;
			return ANX_OK;
		}
	}

	return ANX_EPERM;
}

bool anx_auth_session_has_scope(const struct anx_session *session,
				 enum anx_key_scope_type scope)
{
	uint32_t i;

	if (!session || !session->active)
		return false;

	/* Admin scope grants everything */
	for (i = 0; i < session->scope_count; i++) {
		if (session->scopes[i].type == ANX_SCOPE_ADMIN)
			return true;
		if (session->scopes[i].type == scope)
			return true;
	}
	return false;
}

struct anx_session *anx_auth_current_session(void)
{
	if (!current_session.active)
		return NULL;
	return &current_session;
}

void anx_auth_logout(void)
{
	/* Secure-zero the session */
	volatile uint8_t *p = (volatile uint8_t *)&current_session;
	uint32_t i;

	for (i = 0; i < sizeof(current_session); i++)
		p[i] = 0;
}

bool anx_auth_has_users(void)
{
	int i;

	for (i = 0; i < ANX_MAX_USERS; i++) {
		if (users[i].active)
			return true;
	}
	return false;
}
