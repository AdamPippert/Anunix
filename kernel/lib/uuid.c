/*
 * uuid.c — UUIDv7 generation.
 *
 * Uses arch_time_now() for the timestamp and a simple xorshift PRNG
 * for random bits. A proper CSPRNG can replace the PRNG later.
 */

#include <anx/types.h>
#include <anx/uuid.h>
#include <anx/arch.h>
#include <anx/hashtable.h>

/* Simple xorshift64 PRNG — not cryptographic, sufficient for uniqueness */
static uint64_t prng_state;

static uint64_t xorshift64(void)
{
	uint64_t x = prng_state;

	if (x == 0)
		x = arch_time_now() | 1;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	prng_state = x;
	return x;
}

void anx_uuid_generate(struct anx_uuid *out)
{
	/* UUIDv7 layout:
	 *   hi[63:16] = 48-bit Unix timestamp in milliseconds
	 *   hi[15:12] = version (0x7)
	 *   hi[11:0]  = random
	 *   lo[63:62] = variant (0b10)
	 *   lo[61:0]  = random
	 */
	uint64_t ts_ns = arch_time_now();
	uint64_t ts_ms = ts_ns / 1000000;	/* nanoseconds to milliseconds */
	uint64_t rand1 = xorshift64();
	uint64_t rand2 = xorshift64();

	out->hi = (ts_ms << 16)		/* 48-bit timestamp */
		| (0x7ULL << 12)	/* version 7 */
		| (rand1 & 0xFFF);	/* 12 random bits */

	out->lo = (0x2ULL << 62)	/* variant 10 */
		| (rand2 & 0x3FFFFFFFFFFFFFFFULL); /* 62 random bits */
}

int anx_uuid_compare(const struct anx_uuid *a, const struct anx_uuid *b)
{
	if (a->hi != b->hi)
		return a->hi < b->hi ? -1 : 1;
	if (a->lo != b->lo)
		return a->lo < b->lo ? -1 : 1;
	return 0;
}

static const char hex[] = "0123456789abcdef";

static void put_hex(char *buf, uint64_t val, int nibbles)
{
	for (int i = nibbles - 1; i >= 0; i--) {
		buf[i] = hex[val & 0xf];
		val >>= 4;
	}
}

void anx_uuid_to_string(const struct anx_uuid *u, char *buf, size_t len)
{
	if (len < 37)
		return;

	/* Format: 8-4-4-4-12 */
	put_hex(buf, u->hi >> 32, 8);
	buf[8] = '-';
	put_hex(buf + 9, u->hi >> 16, 4);
	buf[13] = '-';
	put_hex(buf + 14, u->hi, 4);
	buf[18] = '-';
	put_hex(buf + 19, u->lo >> 48, 4);
	buf[23] = '-';
	put_hex(buf + 24, u->lo, 12);
	buf[36] = '\0';
}

uint64_t anx_uuid_hash(const struct anx_uuid *u)
{
	return anx_hash_u64(u->hi ^ u->lo);
}
