/*
 * random.c — Cryptographic random number generation.
 *
 * Uses RDRAND on x86_64, CNTPCT_EL0 + SHA-256 mixing on ARM64.
 * The pool is seeded from hardware sources and stirred with each
 * request to provide forward secrecy.
 */

#include <anx/types.h>
#include <anx/string.h>
#include <anx/crypto.h>
#include <anx/arch.h>

/*
 * Internal entropy pool: 32 bytes of state mixed with hardware entropy
 * on every call. SHA-256 acts as a whitening function.
 */
static uint8_t entropy_pool[32];
static uint64_t pool_counter;

#if defined(__x86_64__)

/* Check if RDRAND is supported via CPUID leaf 1, ECX bit 30 */
static int have_rdrand(void)
{
	static int cached = -1;
	uint32_t eax, ebx, ecx, edx;

	if (cached >= 0)
		return cached;

	__asm__ __volatile__(
		"cpuid"
		: "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
		: "a"(1), "c"(0)
	);

	cached = (ecx & (1u << 30)) ? 1 : 0;
	return cached;
}

/* Read a 64-bit value from RDRAND; retry up to 10 times on failure.
 * Returns -1 if RDRAND is unsupported or keeps failing. */
static int rdrand64(uint64_t *val)
{
	uint64_t v;
	uint32_t ok;
	int tries = 10;

	if (!have_rdrand())
		return -1;

	while (tries-- > 0) {
		__asm__ __volatile__(
			".byte 0x48, 0x0f, 0xc7, 0xf0\n\t"  /* rdrand %rax */
			"setc %b1\n\t"
			: "=a"(v), "=r"(ok)
			:
			: "cc"
		);
		if (ok & 1) {
			*val = v;
			return 0;
		}
	}
	return -1;
}

static void gather_entropy(uint8_t seed[32])
{
	uint64_t r0 = 0, r1 = 0, r2 = 0, r3 = 0;

	/* Try RDRAND first */
	if (rdrand64(&r0) == 0 && rdrand64(&r1) == 0 &&
	    rdrand64(&r2) == 0 && rdrand64(&r3) == 0) {
		anx_memcpy(seed, &r0, 8);
		anx_memcpy(seed + 8, &r1, 8);
		anx_memcpy(seed + 16, &r2, 8);
		anx_memcpy(seed + 24, &r3, 8);
		return;
	}

	/* Fallback: use TSC with multiple samples for timing jitter.
	 * This is not ideal but provides some entropy on VMs that lack
	 * RDRAND (QEMU TCG with Bochs BIOS is one such environment). */
	r0 = arch_timer_ticks();
	r1 = arch_timer_ticks();
	r2 = arch_timer_ticks();
	r3 = arch_timer_ticks();
	anx_memcpy(seed, &r0, 8);
	anx_memcpy(seed + 8, &r1, 8);
	anx_memcpy(seed + 16, &r2, 8);
	anx_memcpy(seed + 24, &r3, 8);
}

#elif defined(__aarch64__)

static void gather_entropy(uint8_t seed[32])
{
	uint64_t t0, t1, t2, t3;

	/*
	 * ARM64 doesn't universally have TRNG. Use CNTPCT_EL0
	 * (physical timer counter) as an entropy source, sampled
	 * multiple times with timing jitter providing entropy.
	 */
	t0 = arch_timer_ticks();
	t1 = arch_timer_ticks();
	t2 = arch_timer_ticks();
	t3 = arch_timer_ticks();

	anx_memcpy(seed, &t0, 8);
	anx_memcpy(seed + 8, &t1, 8);
	anx_memcpy(seed + 16, &t2, 8);
	anx_memcpy(seed + 24, &t3, 8);
}

#else
#error "Unsupported architecture for random number generation"
#endif

/*
 * Mix new entropy into the pool and extract output.
 * Hash(pool || entropy || counter) -> new pool, output derived from it.
 */
static void pool_stir_and_extract(uint8_t *out, uint32_t len)
{
	struct anx_sha256_ctx ctx;
	uint8_t hw_seed[32];
	uint8_t hash[32];

	while (len > 0) {
		uint32_t chunk = len < 32 ? len : 32;

		gather_entropy(hw_seed);
		pool_counter++;

		anx_sha256_init(&ctx);
		anx_sha256_update(&ctx, entropy_pool, 32);
		anx_sha256_update(&ctx, hw_seed, 32);
		anx_sha256_update(&ctx, (const uint8_t *)&pool_counter, 8);
		anx_sha256_final(&ctx, hash);

		/* Update pool for forward secrecy */
		anx_memcpy(entropy_pool, hash, 32);

		/* Extract output (re-hash to separate pool from output) */
		anx_sha256_init(&ctx);
		anx_sha256_update(&ctx, hash, 32);
		anx_sha256_update(&ctx, (const uint8_t *)&pool_counter, 8);
		anx_sha256_final(&ctx, hash);

		anx_memcpy(out, hash, chunk);
		out += chunk;
		len -= chunk;
	}

	anx_memset(hw_seed, 0, sizeof(hw_seed));
	anx_memset(hash, 0, sizeof(hash));
}

void anx_random_bytes(void *buf, uint32_t len)
{
	pool_stir_and_extract((uint8_t *)buf, len);
}

uint32_t anx_random_u32(void)
{
	uint32_t val;

	anx_random_bytes(&val, sizeof(val));
	return val;
}
