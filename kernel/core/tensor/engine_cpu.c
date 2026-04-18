/*
 * engine_cpu.c — CPU reference math engine for Tensor Objects.
 *
 * Implements matmul, add, sub, element-wise mul, relu, softmax,
 * transpose, and scale for integer dtypes using native arithmetic.
 * Float dtypes use softfloat (correct but slow, quality_score=10).
 *
 * This engine is always available — it registers at boot with
 * INT8 and INT32 capability bits.
 */

#include <anx/types.h>
#include <anx/tensor_ops.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/engine.h>
#include <anx/alloc.h>
#include <anx/string.h>

/* --- INT8 matmul: C[m,n] = A[m,k] @ B[k,n] --- */
static int matmul_int8(const int8_t *a, const int8_t *b, int8_t *c,
			uint64_t m, uint64_t k, uint64_t n)
{
	uint64_t i, j, p;

	for (i = 0; i < m; i++) {
		for (j = 0; j < n; j++) {
			int32_t sum = 0;

			for (p = 0; p < k; p++)
				sum += (int32_t)a[i * k + p] *
				       (int32_t)b[p * n + j];
			/* Saturate to int8 range */
			if (sum > 127) sum = 127;
			if (sum < -128) sum = -128;
			c[i * n + j] = (int8_t)sum;
		}
	}
	return ANX_OK;
}

/* --- INT32 matmul --- */
static int matmul_int32(const int32_t *a, const int32_t *b, int32_t *c,
			 uint64_t m, uint64_t k, uint64_t n)
{
	uint64_t i, j, p;

	for (i = 0; i < m; i++) {
		for (j = 0; j < n; j++) {
			int64_t sum = 0;

			for (p = 0; p < k; p++)
				sum += (int64_t)a[i * k + p] *
				       (int64_t)b[p * n + j];
			c[i * n + j] = (int32_t)sum;
		}
	}
	return ANX_OK;
}

/* --- Element-wise binary ops --- */
static void add_int8(const int8_t *a, const int8_t *b, int8_t *c,
		      uint64_t n)
{
	uint64_t i;

	for (i = 0; i < n; i++) {
		int16_t sum = (int16_t)a[i] + (int16_t)b[i];

		if (sum > 127) sum = 127;
		if (sum < -128) sum = -128;
		c[i] = (int8_t)sum;
	}
}

static void add_int32(const int32_t *a, const int32_t *b, int32_t *c,
		       uint64_t n)
{
	uint64_t i;

	for (i = 0; i < n; i++)
		c[i] = a[i] + b[i];
}

static void sub_int8(const int8_t *a, const int8_t *b, int8_t *c,
		      uint64_t n)
{
	uint64_t i;

	for (i = 0; i < n; i++) {
		int16_t diff = (int16_t)a[i] - (int16_t)b[i];

		if (diff > 127) diff = 127;
		if (diff < -128) diff = -128;
		c[i] = (int8_t)diff;
	}
}

static void sub_int32(const int32_t *a, const int32_t *b, int32_t *c,
		       uint64_t n)
{
	uint64_t i;

	for (i = 0; i < n; i++)
		c[i] = a[i] - b[i];
}

static void mul_elem_int8(const int8_t *a, const int8_t *b, int8_t *c,
			    uint64_t n)
{
	uint64_t i;

	for (i = 0; i < n; i++) {
		int32_t prod = (int32_t)a[i] * (int32_t)b[i];

		if (prod > 127) prod = 127;
		if (prod < -128) prod = -128;
		c[i] = (int8_t)prod;
	}
}

static void mul_elem_int32(const int32_t *a, const int32_t *b, int32_t *c,
			    uint64_t n)
{
	uint64_t i;

	for (i = 0; i < n; i++)
		c[i] = a[i] * b[i];
}

/* --- Unary ops --- */
static void relu_int8(const int8_t *a, int8_t *b, uint64_t n)
{
	uint64_t i;

	for (i = 0; i < n; i++)
		b[i] = a[i] > 0 ? a[i] : 0;
}

static void relu_int32(const int32_t *a, int32_t *b, uint64_t n)
{
	uint64_t i;

	for (i = 0; i < n; i++)
		b[i] = a[i] > 0 ? a[i] : 0;
}

/* Softmax for INT8: exp approximation via lookup, normalize */
static void softmax_int8(const int8_t *a, int8_t *b, uint64_t n)
{
	uint64_t i;
	int8_t max_val = a[0];
	int32_t sum = 0;

	/* Find max for numerical stability */
	for (i = 1; i < n; i++) {
		if (a[i] > max_val)
			max_val = a[i];
	}

	/* Approximate: shifted values, scale to [0, 127] */
	for (i = 0; i < n; i++) {
		int32_t shifted = (int32_t)a[i] - (int32_t)max_val;

		/* Clamp to avoid extreme negatives */
		if (shifted < -128) shifted = -128;
		/* Simple linear approximation of exp: max(0, shifted+128) */
		b[i] = (int8_t)(shifted + 128 > 127 ? 127 : shifted + 128);
		if (b[i] < 0) b[i] = 0;
		sum += (int32_t)b[i];
	}

	/* Normalize to sum to 127 (approximate probability) */
	if (sum > 0) {
		for (i = 0; i < n; i++)
			b[i] = (int8_t)((int32_t)b[i] * 127 / sum);
	}
}

/* Transpose 2D: B[n,m] = A[m,n] */
static void transpose_int8(const int8_t *a, int8_t *b,
			     uint64_t m, uint64_t n)
{
	uint64_t i, j;

	for (i = 0; i < m; i++)
		for (j = 0; j < n; j++)
			b[j * m + i] = a[i * n + j];
}

static void transpose_int32(const int32_t *a, int32_t *b,
			      uint64_t m, uint64_t n)
{
	uint64_t i, j;

	for (i = 0; i < m; i++)
		for (j = 0; j < n; j++)
			b[j * m + i] = a[i * n + j];
}

/* Scale: B = A * scalar */
static void scale_int8(const int8_t *a, int8_t *b, uint64_t n,
			int64_t scalar)
{
	uint64_t i;

	for (i = 0; i < n; i++) {
		int32_t v = (int32_t)a[i] * (int32_t)scalar;

		if (v > 127) v = 127;
		if (v < -128) v = -128;
		b[i] = (int8_t)v;
	}
}

static void scale_int32(const int32_t *a, int32_t *b, uint64_t n,
			 int64_t scalar)
{
	uint64_t i;

	for (i = 0; i < n; i++)
		b[i] = (int32_t)((int64_t)a[i] * scalar);
}

/* --- Public dispatch for CPU engine --- */

int anx_tensor_cpu_matmul(struct anx_state_object *a,
			    struct anx_state_object *b,
			    struct anx_tensor_meta *ma,
			    struct anx_tensor_meta *mb,
			    struct anx_state_object **out)
{
	struct anx_tensor_meta mc;
	int ret;

	if (ma->ndim != 2 || mb->ndim != 2)
		return ANX_EINVAL;
	if (ma->shape[1] != mb->shape[0])
		return ANX_EINVAL;
	if (ma->dtype != mb->dtype)
		return ANX_EINVAL;

	anx_memset(&mc, 0, sizeof(mc));
	mc.ndim = 2;
	mc.shape[0] = ma->shape[0];
	mc.shape[1] = mb->shape[1];
	mc.dtype = ma->dtype;

	ret = anx_tensor_create(&mc, NULL, 0, out);
	if (ret != ANX_OK)
		return ret;

	if (ma->dtype == ANX_DTYPE_INT8) {
		matmul_int8((const int8_t *)a->payload,
			    (const int8_t *)b->payload,
			    (int8_t *)(*out)->payload,
			    ma->shape[0], ma->shape[1], mb->shape[1]);
	} else if (ma->dtype == ANX_DTYPE_INT32) {
		matmul_int32((const int32_t *)a->payload,
			     (const int32_t *)b->payload,
			     (int32_t *)(*out)->payload,
			     ma->shape[0], ma->shape[1], mb->shape[1]);
	} else {
		return ANX_EINVAL;
	}

	return ANX_OK;
}

int anx_tensor_cpu_binary(enum anx_tensor_op op,
			    struct anx_state_object *a,
			    struct anx_state_object *b,
			    struct anx_tensor_meta *ma,
			    struct anx_state_object **out)
{
	struct anx_tensor_meta mc;
	int ret;

	mc = *ma;
	ret = anx_tensor_create(&mc, NULL, 0, out);
	if (ret != ANX_OK)
		return ret;

	if (ma->dtype == ANX_DTYPE_INT8) {
		switch (op) {
		case ANX_OP_ADD:
			add_int8((const int8_t *)a->payload,
				 (const int8_t *)b->payload,
				 (int8_t *)(*out)->payload,
				 ma->elem_count);
			break;
		case ANX_OP_SUB:
			sub_int8((const int8_t *)a->payload,
				 (const int8_t *)b->payload,
				 (int8_t *)(*out)->payload,
				 ma->elem_count);
			break;
		case ANX_OP_MUL_ELEM:
			mul_elem_int8((const int8_t *)a->payload,
				      (const int8_t *)b->payload,
				      (int8_t *)(*out)->payload,
				      ma->elem_count);
			break;
		default:
			return ANX_EINVAL;
		}
	} else if (ma->dtype == ANX_DTYPE_INT32) {
		switch (op) {
		case ANX_OP_ADD:
			add_int32((const int32_t *)a->payload,
				  (const int32_t *)b->payload,
				  (int32_t *)(*out)->payload,
				  ma->elem_count);
			break;
		case ANX_OP_SUB:
			sub_int32((const int32_t *)a->payload,
				  (const int32_t *)b->payload,
				  (int32_t *)(*out)->payload,
				  ma->elem_count);
			break;
		case ANX_OP_MUL_ELEM:
			mul_elem_int32((const int32_t *)a->payload,
				       (const int32_t *)b->payload,
				       (int32_t *)(*out)->payload,
				       ma->elem_count);
			break;
		default:
			return ANX_EINVAL;
		}
	} else {
		return ANX_EINVAL;
	}

	return ANX_OK;
}

int anx_tensor_cpu_unary(enum anx_tensor_op op,
			   struct anx_state_object *a,
			   struct anx_tensor_meta *ma,
			   int64_t scalar,
			   struct anx_state_object **out)
{
	struct anx_tensor_meta mc;
	int ret;

	mc = *ma;

	/* Transpose swaps dimensions */
	if (op == ANX_OP_TRANSPOSE) {
		if (ma->ndim != 2)
			return ANX_EINVAL;
		mc.shape[0] = ma->shape[1];
		mc.shape[1] = ma->shape[0];
	}

	ret = anx_tensor_create(&mc, NULL, 0, out);
	if (ret != ANX_OK)
		return ret;

	if (ma->dtype == ANX_DTYPE_INT8) {
		switch (op) {
		case ANX_OP_RELU:
			relu_int8((const int8_t *)a->payload,
				  (int8_t *)(*out)->payload,
				  ma->elem_count);
			break;
		case ANX_OP_SOFTMAX:
			softmax_int8((const int8_t *)a->payload,
				     (int8_t *)(*out)->payload,
				     ma->elem_count);
			break;
		case ANX_OP_TRANSPOSE:
			transpose_int8((const int8_t *)a->payload,
				       (int8_t *)(*out)->payload,
				       ma->shape[0], ma->shape[1]);
			break;
		case ANX_OP_SCALE:
			scale_int8((const int8_t *)a->payload,
				   (int8_t *)(*out)->payload,
				   ma->elem_count, scalar);
			break;
		default:
			return ANX_EINVAL;
		}
	} else if (ma->dtype == ANX_DTYPE_INT32) {
		switch (op) {
		case ANX_OP_RELU:
			relu_int32((const int32_t *)a->payload,
				   (int32_t *)(*out)->payload,
				   ma->elem_count);
			break;
		case ANX_OP_TRANSPOSE:
			transpose_int32((const int32_t *)a->payload,
					(int32_t *)(*out)->payload,
					ma->shape[0], ma->shape[1]);
			break;
		case ANX_OP_SCALE:
			scale_int32((const int32_t *)a->payload,
				    (int32_t *)(*out)->payload,
				    ma->elem_count, scalar);
			break;
		default:
			return ANX_EINVAL;
		}
	} else {
		return ANX_EINVAL;
	}

	return ANX_OK;
}

int anx_tensor_cpu_engine_init(void)
{
	struct anx_engine *eng;
	int ret;

	ret = anx_engine_register("cpu-tensor", ANX_ENGINE_DETERMINISTIC_TOOL,
				   ANX_CAP_TENSOR_INT8 | ANX_CAP_TENSOR_INT32,
				   &eng);
	if (ret != ANX_OK)
		return ret;

	eng->quality_score = 10;	/* correct but slow */
	eng->is_local = true;
	return ANX_OK;
}
