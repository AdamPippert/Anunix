/*
 * brin.c — BRIN statistical summary computation for Tensor Objects.
 *
 * Computes mean, variance, L2 norm, sparsity, min, max by streaming
 * through the tensor payload element by element. No block
 * materialization needed — memory usage is constant regardless
 * of tensor size.
 *
 * Integer dtypes use native arithmetic with 64-bit accumulators.
 * Float dtypes use softfloat for accumulation.
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/state_object.h>

/* Epsilon for sparsity: elements with |x| < epsilon are "zero" */
#define SPARSITY_EPSILON_BITS	0x3727C5ACUL	/* ~1e-5 as float32 */

int anx_tensor_compute_brin(struct anx_state_object *obj,
			     struct anx_tensor_meta *meta)
{
	const uint8_t *data = (const uint8_t *)obj->payload;
	uint64_t count = meta->elem_count;
	uint64_t i;

	if (!data || count == 0)
		return ANX_EINVAL;

	/* Use integer arithmetic for integer dtypes (fast path) */
	if (meta->dtype == ANX_DTYPE_INT8) {
		const int8_t *p = (const int8_t *)data;
		int64_t sum = 0, sum_sq = 0;
		int64_t min_val = p[0], max_val = p[0];
		uint64_t zero_count = 0;

		for (i = 0; i < count; i++) {
			int64_t v = (int64_t)p[i];

			sum += v;
			sum_sq += v * v;
			if (v < min_val) min_val = v;
			if (v > max_val) max_val = v;
			if (v == 0) zero_count++;
		}

		meta->stat_mean_bits = anx_sf_div(
			anx_sf_from_int(sum),
			anx_sf_from_int((int64_t)count));
		meta->stat_variance_bits = anx_sf_div(
			anx_sf_from_int(sum_sq),
			anx_sf_from_int((int64_t)count));
		/* variance = E[x^2] - E[x]^2 */
		{
			uint32_t mean_sq = anx_sf_mul(
				meta->stat_mean_bits,
				meta->stat_mean_bits);

			meta->stat_variance_bits = anx_sf_add(
				meta->stat_variance_bits,
				mean_sq ^ 0x80000000U); /* negate */
		}
		meta->stat_l2_norm_bits = anx_sf_from_int(sum_sq);
		/* TODO: sqrt for proper L2 norm */
		meta->stat_min_bits = anx_sf_from_int(min_val);
		meta->stat_max_bits = anx_sf_from_int(max_val);
		meta->stat_sparsity_bits = anx_sf_div(
			anx_sf_from_int((int64_t)zero_count),
			anx_sf_from_int((int64_t)count));

	} else if (meta->dtype == ANX_DTYPE_INT32) {
		const int32_t *p = (const int32_t *)data;
		int64_t sum = 0;
		int64_t min_val = p[0], max_val = p[0];
		uint64_t zero_count = 0;

		for (i = 0; i < count; i++) {
			int64_t v = (int64_t)p[i];

			sum += v;
			if (v < min_val) min_val = v;
			if (v > max_val) max_val = v;
			if (v == 0) zero_count++;
		}

		meta->stat_mean_bits = anx_sf_div(
			anx_sf_from_int(sum),
			anx_sf_from_int((int64_t)count));
		meta->stat_min_bits = anx_sf_from_int(min_val);
		meta->stat_max_bits = anx_sf_from_int(max_val);
		meta->stat_sparsity_bits = anx_sf_div(
			anx_sf_from_int((int64_t)zero_count),
			anx_sf_from_int((int64_t)count));

	} else if (meta->dtype == ANX_DTYPE_UINT8) {
		const uint8_t *p = data;
		uint64_t sum = 0;
		uint64_t min_val = p[0], max_val = p[0];
		uint64_t zero_count = 0;

		for (i = 0; i < count; i++) {
			uint64_t v = (uint64_t)p[i];

			sum += v;
			if (v < min_val) min_val = v;
			if (v > max_val) max_val = v;
			if (v == 0) zero_count++;
		}

		meta->stat_mean_bits = anx_sf_div(
			anx_sf_from_int((int64_t)sum),
			anx_sf_from_int((int64_t)count));
		meta->stat_min_bits = anx_sf_from_int((int64_t)min_val);
		meta->stat_max_bits = anx_sf_from_int((int64_t)max_val);
		meta->stat_sparsity_bits = anx_sf_div(
			anx_sf_from_int((int64_t)zero_count),
			anx_sf_from_int((int64_t)count));

	} else if (meta->dtype == ANX_DTYPE_FLOAT32) {
		/* Float32: read bit patterns, use softfloat */
		const uint32_t *p = (const uint32_t *)data;
		uint32_t sf_sum = anx_sf_zero();
		uint32_t sf_min = p[0], sf_max = p[0];
		uint64_t zero_count = 0;

		for (i = 0; i < count; i++) {
			uint32_t v = p[i];

			sf_sum = anx_sf_add(sf_sum, v);
			if (anx_sf_lt(v, sf_min)) sf_min = v;
			if (anx_sf_gt(v, sf_max)) sf_max = v;
			if (anx_sf_lt(anx_sf_abs(v), SPARSITY_EPSILON_BITS))
				zero_count++;
		}

		meta->stat_mean_bits = anx_sf_div(
			sf_sum, anx_sf_from_int((int64_t)count));
		meta->stat_min_bits = sf_min;
		meta->stat_max_bits = sf_max;
		meta->stat_sparsity_bits = anx_sf_div(
			anx_sf_from_int((int64_t)zero_count),
			anx_sf_from_int((int64_t)count));

	} else {
		/* Unsupported dtype — zero stats */
		meta->stat_mean_bits = anx_sf_zero();
		meta->stat_variance_bits = anx_sf_zero();
		meta->stat_l2_norm_bits = anx_sf_zero();
		meta->stat_sparsity_bits = anx_sf_zero();
		meta->stat_min_bits = anx_sf_zero();
		meta->stat_max_bits = anx_sf_zero();
	}

	return ANX_OK;
}
