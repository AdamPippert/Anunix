/*
 * softfloat.c — Minimal IEEE 754 float32 arithmetic via integer registers.
 *
 * The kernel compiles with -mgeneral-regs-only, so we cannot use
 * FP instructions. These functions manipulate IEEE 754 single-precision
 * bit patterns stored as uint32_t. Only the operations needed for
 * BRIN computation are implemented.
 */

#include <anx/types.h>
#include <anx/tensor.h>

/* IEEE 754 float32 layout: [sign:1][exponent:8][mantissa:23] */
#define SF_SIGN_BIT	0x80000000U
#define SF_EXP_MASK	0x7F800000U
#define SF_MANT_MASK	0x007FFFFFU
#define SF_EXP_BIAS	127
#define SF_MANT_BITS	23

/* Special values */
#define SF_ZERO		0x00000000U
#define SF_NEG_ZERO	0x80000000U
#define SF_POS_INF	0x7F800000U
#define SF_NEG_INF	0xFF800000U

static uint32_t sf_sign(uint32_t x) { return x & SF_SIGN_BIT; }
static int32_t sf_exp(uint32_t x) { return (int32_t)((x & SF_EXP_MASK) >> SF_MANT_BITS) - SF_EXP_BIAS; }
static uint32_t sf_mant(uint32_t x) { return x & SF_MANT_MASK; }
static bool sf_is_zero(uint32_t x) { return (x & ~SF_SIGN_BIT) == 0; }

uint32_t anx_sf_zero(void)
{
	return SF_ZERO;
}

uint32_t anx_sf_abs(uint32_t a)
{
	return a & ~SF_SIGN_BIT;
}

/* Convert int64 to float32 bit pattern */
uint32_t anx_sf_from_int(int64_t val)
{
	uint32_t sign = 0;
	uint64_t abs_val;
	int32_t exp;
	uint32_t mant;
	int shift;

	if (val == 0)
		return SF_ZERO;

	if (val < 0) {
		sign = SF_SIGN_BIT;
		abs_val = (uint64_t)(-val);
	} else {
		abs_val = (uint64_t)val;
	}

	/* Find the highest set bit */
	exp = 0;
	{
		uint64_t tmp = abs_val;
		while (tmp > 1) {
			tmp >>= 1;
			exp++;
		}
	}

	/* Shift mantissa to fit in 23 bits */
	shift = exp - SF_MANT_BITS;
	if (shift > 0)
		mant = (uint32_t)(abs_val >> shift);
	else
		mant = (uint32_t)(abs_val << (-shift));

	/* Remove implicit leading 1 */
	mant &= SF_MANT_MASK;

	return sign | ((uint32_t)(exp + SF_EXP_BIAS) << SF_MANT_BITS) | mant;
}

/* Convert float32 bit pattern to int64 (truncate) */
int64_t anx_sf_to_int(uint32_t bits)
{
	int32_t exp;
	uint32_t mant;
	int64_t result;

	if (sf_is_zero(bits))
		return 0;

	exp = sf_exp(bits);
	if (exp < 0)
		return 0;	/* |value| < 1 */

	mant = sf_mant(bits) | (1U << SF_MANT_BITS);	/* add implicit 1 */

	if (exp >= SF_MANT_BITS)
		result = (int64_t)mant << (exp - SF_MANT_BITS);
	else
		result = (int64_t)(mant >> (SF_MANT_BITS - exp));

	if (sf_sign(bits))
		result = -result;

	return result;
}

/* float32 addition */
uint32_t anx_sf_add(uint32_t a, uint32_t b)
{
	int32_t exp_a, exp_b, exp_r;
	int64_t mant_a, mant_b, mant_r;
	uint32_t sign_r;
	int shift;

	if (sf_is_zero(a)) return b;
	if (sf_is_zero(b)) return a;

	exp_a = sf_exp(a);
	exp_b = sf_exp(b);
	mant_a = (int64_t)(sf_mant(a) | (1U << SF_MANT_BITS));
	mant_b = (int64_t)(sf_mant(b) | (1U << SF_MANT_BITS));

	if (sf_sign(a)) mant_a = -mant_a;
	if (sf_sign(b)) mant_b = -mant_b;

	/* Align exponents */
	if (exp_a > exp_b) {
		shift = exp_a - exp_b;
		if (shift < 40) mant_b >>= shift;
		else mant_b = 0;
		exp_r = exp_a;
	} else {
		shift = exp_b - exp_a;
		if (shift < 40) mant_a >>= shift;
		else mant_a = 0;
		exp_r = exp_b;
	}

	mant_r = mant_a + mant_b;

	if (mant_r == 0)
		return SF_ZERO;

	sign_r = 0;
	if (mant_r < 0) {
		sign_r = SF_SIGN_BIT;
		mant_r = -mant_r;
	}

	/* Normalize */
	while (mant_r >= (2LL << SF_MANT_BITS)) {
		mant_r >>= 1;
		exp_r++;
	}
	while (mant_r > 0 && mant_r < (1LL << SF_MANT_BITS)) {
		mant_r <<= 1;
		exp_r--;
	}

	if (exp_r + SF_EXP_BIAS <= 0)
		return SF_ZERO;
	if (exp_r + SF_EXP_BIAS >= 255)
		return sign_r | SF_POS_INF;

	return sign_r |
	       ((uint32_t)(exp_r + SF_EXP_BIAS) << SF_MANT_BITS) |
	       ((uint32_t)mant_r & SF_MANT_MASK);
}

/* float32 multiplication */
uint32_t anx_sf_mul(uint32_t a, uint32_t b)
{
	int32_t exp_r;
	uint64_t mant_a, mant_b, mant_r;
	uint32_t sign_r;

	if (sf_is_zero(a) || sf_is_zero(b))
		return SF_ZERO;

	sign_r = sf_sign(a) ^ sf_sign(b);
	exp_r = sf_exp(a) + sf_exp(b);

	mant_a = (uint64_t)(sf_mant(a) | (1U << SF_MANT_BITS));
	mant_b = (uint64_t)(sf_mant(b) | (1U << SF_MANT_BITS));
	mant_r = (mant_a * mant_b) >> SF_MANT_BITS;

	/* Normalize */
	while (mant_r >= (2ULL << SF_MANT_BITS)) {
		mant_r >>= 1;
		exp_r++;
	}

	if (exp_r + SF_EXP_BIAS <= 0)
		return SF_ZERO;
	if (exp_r + SF_EXP_BIAS >= 255)
		return sign_r | SF_POS_INF;

	return sign_r |
	       ((uint32_t)(exp_r + SF_EXP_BIAS) << SF_MANT_BITS) |
	       ((uint32_t)mant_r & SF_MANT_MASK);
}

/* float32 division */
uint32_t anx_sf_div(uint32_t a, uint32_t b)
{
	int32_t exp_r;
	uint64_t mant_a, mant_b, mant_r;
	uint32_t sign_r;

	if (sf_is_zero(b))
		return SF_POS_INF;
	if (sf_is_zero(a))
		return SF_ZERO;

	sign_r = sf_sign(a) ^ sf_sign(b);
	exp_r = sf_exp(a) - sf_exp(b);

	mant_a = (uint64_t)(sf_mant(a) | (1U << SF_MANT_BITS));
	mant_b = (uint64_t)(sf_mant(b) | (1U << SF_MANT_BITS));
	mant_r = (mant_a << SF_MANT_BITS) / mant_b;

	/* Normalize */
	while (mant_r >= (2ULL << SF_MANT_BITS)) {
		mant_r >>= 1;
		exp_r++;
	}
	while (mant_r > 0 && mant_r < (1ULL << SF_MANT_BITS)) {
		mant_r <<= 1;
		exp_r--;
	}

	if (exp_r + SF_EXP_BIAS <= 0)
		return SF_ZERO;
	if (exp_r + SF_EXP_BIAS >= 255)
		return sign_r | SF_POS_INF;

	return sign_r |
	       ((uint32_t)(exp_r + SF_EXP_BIAS) << SF_MANT_BITS) |
	       ((uint32_t)mant_r & SF_MANT_MASK);
}

/* float32 comparison: a < b */
bool anx_sf_lt(uint32_t a, uint32_t b)
{
	/* Handle zeros */
	if (sf_is_zero(a) && sf_is_zero(b))
		return false;

	/* Both positive: compare as integers */
	if (!sf_sign(a) && !sf_sign(b))
		return a < b;

	/* Both negative: reverse comparison */
	if (sf_sign(a) && sf_sign(b))
		return (a & ~SF_SIGN_BIT) > (b & ~SF_SIGN_BIT);

	/* Different signs: negative < positive */
	return sf_sign(a) != 0;
}

/* float32 comparison: a > b */
bool anx_sf_gt(uint32_t a, uint32_t b)
{
	return anx_sf_lt(b, a);
}
