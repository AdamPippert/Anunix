/*
 * test_tensor.c — Tensor Object tests (RFC-0013 Phase 1).
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

static bool near_eq(float a, float b, float eps)
{
	float d = a - b;

	if (d < 0)
		d = -d;
	return d <= eps;
}

static int test_dtype_helpers(void)
{
	if (anx_tensor_dtype_bytes(ANX_DTYPE_FLOAT32) != 4)
		return -1;
	if (anx_tensor_dtype_bytes(ANX_DTYPE_BFLOAT16) != 2)
		return -2;
	if (anx_tensor_dtype_bytes(ANX_DTYPE_INT8) != 1)
		return -3;
	if (anx_tensor_dtype_bytes(ANX_DTYPE_INT4) != 0)
		return -4;
	if (anx_tensor_dtype_bytes(ANX_DTYPE_BOOL) != 0)
		return -5;

	{
		uint64_t shape3[3] = { 2, 3, 4 };

		if (anx_tensor_shape_elems(shape3, 3) != 24)
			return -6;
	}
	{
		uint64_t shape1[1] = { 0 };

		if (anx_tensor_shape_elems(shape1, 1) != 0)
			return -7;
	}

	if (anx_tensor_compute_byte_size(ANX_DTYPE_FLOAT32, 10) != 40)
		return -8;
	if (anx_tensor_compute_byte_size(ANX_DTYPE_INT4, 7) != 4)
		return -9;
	if (anx_tensor_compute_byte_size(ANX_DTYPE_INT4, 8) != 4)
		return -10;
	if (anx_tensor_compute_byte_size(ANX_DTYPE_BOOL, 9) != 2)
		return -11;

	if (anx_strcmp(anx_tensor_dtype_name(ANX_DTYPE_BFLOAT16),
		       "bfloat16") != 0)
		return -12;

	return 0;
}

static int test_create_and_readback_fp32(void)
{
	struct anx_tensor_meta meta;
	struct anx_state_object *obj;
	float src[6] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f };
	float dst[6];
	int ret;

	anx_objstore_init();

	anx_memset(&meta, 0, sizeof(meta));
	meta.ndim = 2;
	meta.shape[0] = 2;
	meta.shape[1] = 3;
	meta.dtype = ANX_DTYPE_FLOAT32;

	ret = anx_tensor_create(&meta, src, sizeof(src), &obj);
	if (ret != ANX_OK)
		return -1;
	if (obj->object_type != ANX_OBJ_TENSOR)
		return -2;

	{
		struct anx_tensor_meta out;

		ret = anx_tensor_get_meta(&obj->oid, &out);
		if (ret != ANX_OK)
			return -3;
		if (out.ndim != 2)
			return -4;
		if (out.shape[0] != 2 || out.shape[1] != 3)
			return -5;
		if (out.dtype != ANX_DTYPE_FLOAT32)
			return -6;
		if (out.elem_count != 6)
			return -7;
		if (out.byte_size != 24)
			return -8;
		if (out.brin_valid)
			return -9;	/* not yet sealed */
	}

	ret = anx_tensor_read_data(&obj->oid, 0, dst, sizeof(dst));
	if (ret != (int)sizeof(dst))
		return -10;
	{
		int i;

		for (i = 0; i < 6; i++) {
			if (!near_eq(dst[i], src[i], 1.0e-6f))
				return -11;
		}
	}

	anx_objstore_release(obj);
	return 0;
}

static int test_seal_computes_brin(void)
{
	struct anx_tensor_meta meta;
	struct anx_state_object *obj;
	float src[5] = { -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };
	int ret;

	anx_objstore_init();

	anx_memset(&meta, 0, sizeof(meta));
	meta.ndim = 1;
	meta.shape[0] = 5;
	meta.dtype = ANX_DTYPE_FLOAT32;

	ret = anx_tensor_create(&meta, src, sizeof(src), &obj);
	if (ret != ANX_OK)
		return -1;

	ret = anx_tensor_seal(&obj->oid);
	if (ret != ANX_OK)
		return -2;

	if (obj->state != ANX_OBJ_SEALED)
		return -3;

	{
		struct anx_tensor_meta out;

		ret = anx_tensor_get_meta(&obj->oid, &out);
		if (ret != ANX_OK)
			return -4;
		if (!out.brin_valid)
			return -5;

		/* mean(−2,−1,0,1,2) = 0 */
		if (!near_eq(out.stat_mean, 0.0f, 1.0e-5f))
			return -6;

		/* var = (4+1+0+1+4)/5 = 2 */
		if (!near_eq(out.stat_variance, 2.0f, 1.0e-4f))
			return -7;

		/* L2 = sqrt(10) ≈ 3.1623 */
		if (!near_eq(out.stat_l2_norm, 3.1622777f, 1.0e-3f))
			return -8;

		if (!near_eq(out.stat_min, -2.0f, 1.0e-6f))
			return -9;
		if (!near_eq(out.stat_max, 2.0f, 1.0e-6f))
			return -10;

		/* one of five elements is zero → sparsity 0.2 */
		if (!near_eq(out.stat_sparsity, 0.2f, 1.0e-5f))
			return -11;
	}

	/* Sealing again is idempotent. */
	ret = anx_tensor_seal(&obj->oid);
	if (ret != ANX_OK)
		return -12;

	anx_objstore_release(obj);
	return 0;
}

static int test_sparsity_search(void)
{
	struct anx_tensor_meta meta;
	struct anx_state_object *dense;
	struct anx_state_object *sparse;
	float dense_data[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
	float sparse_data[4] = { 0.0f, 0.0f, 0.0f, 0.7f };
	anx_oid_t results[4];
	uint32_t count = 0;
	int ret;

	anx_objstore_init();

	anx_memset(&meta, 0, sizeof(meta));
	meta.ndim = 1;
	meta.shape[0] = 4;
	meta.dtype = ANX_DTYPE_FLOAT32;

	ret = anx_tensor_create(&meta, dense_data, sizeof(dense_data), &dense);
	if (ret != ANX_OK)
		return -1;
	ret = anx_tensor_create(&meta, sparse_data, sizeof(sparse_data),
				&sparse);
	if (ret != ANX_OK)
		return -2;

	anx_tensor_seal(&dense->oid);
	anx_tensor_seal(&sparse->oid);

	/* Only sparse one has sparsity in [0.5, 1.0]. */
	ret = anx_tensor_search(0.5f, 1.0f, ANX_DTYPE_COUNT,
				results, 4, &count);
	if (ret != ANX_OK)
		return -3;
	if (count != 1)
		return -4;
	if (anx_uuid_compare(&results[0], &sparse->oid) != 0)
		return -5;

	/* Dense tensor has sparsity 0; should match a [0, 0.1] query. */
	count = 0;
	ret = anx_tensor_search(0.0f, 0.1f, ANX_DTYPE_FLOAT32,
				results, 4, &count);
	if (ret != ANX_OK)
		return -6;
	if (count != 1)
		return -7;
	if (anx_uuid_compare(&results[0], &dense->oid) != 0)
		return -8;

	/* Dtype filter excludes both. */
	count = 0;
	ret = anx_tensor_search(0.0f, 1.0f, ANX_DTYPE_INT8,
				results, 4, &count);
	if (ret != ANX_OK)
		return -9;
	if (count != 0)
		return -10;

	anx_objstore_release(dense);
	anx_objstore_release(sparse);
	return 0;
}

static int test_bfloat16_brin(void)
{
	struct anx_tensor_meta meta;
	struct anx_state_object *obj;
	/* bf16 bit patterns for values { 1.0, 2.0, -1.0, 0.0 }.
	 * bf16 = top 16 bits of fp32: fp32(1.0)=0x3F800000 → 0x3F80. */
	uint16_t src[4] = { 0x3F80, 0x4000, 0xBF80, 0x0000 };
	int ret;

	anx_objstore_init();

	anx_memset(&meta, 0, sizeof(meta));
	meta.ndim = 1;
	meta.shape[0] = 4;
	meta.dtype = ANX_DTYPE_BFLOAT16;

	ret = anx_tensor_create(&meta, src, sizeof(src), &obj);
	if (ret != ANX_OK)
		return -1;
	ret = anx_tensor_seal(&obj->oid);
	if (ret != ANX_OK)
		return -2;

	{
		struct anx_tensor_meta out;

		anx_tensor_get_meta(&obj->oid, &out);
		if (!out.brin_valid)
			return -3;

		/* mean(1, 2, -1, 0) = 0.5 */
		if (!near_eq(out.stat_mean, 0.5f, 1.0e-4f))
			return -4;
		/* one zero out of four */
		if (!near_eq(out.stat_sparsity, 0.25f, 1.0e-5f))
			return -5;
		if (!near_eq(out.stat_min, -1.0f, 1.0e-4f))
			return -6;
		if (!near_eq(out.stat_max, 2.0f, 1.0e-4f))
			return -7;
	}

	anx_objstore_release(obj);
	return 0;
}

static int test_int8_brin(void)
{
	struct anx_tensor_meta meta;
	struct anx_state_object *obj;
	int8_t src[4] = { -128, -64, 0, 127 };
	int ret;

	anx_objstore_init();

	anx_memset(&meta, 0, sizeof(meta));
	meta.ndim = 1;
	meta.shape[0] = 4;
	meta.dtype = ANX_DTYPE_INT8;

	ret = anx_tensor_create(&meta, src, sizeof(src), &obj);
	if (ret != ANX_OK)
		return -1;
	ret = anx_tensor_seal(&obj->oid);
	if (ret != ANX_OK)
		return -2;

	{
		struct anx_tensor_meta out;

		anx_tensor_get_meta(&obj->oid, &out);
		if (out.stat_min > -127.5f)
			return -3;
		if (!near_eq(out.stat_max, 127.0f, 1.0f))
			return -4;
		if (!near_eq(out.stat_sparsity, 0.25f, 1.0e-5f))
			return -5;
	}

	anx_objstore_release(obj);
	return 0;
}

static int test_invalid_inputs(void)
{
	struct anx_tensor_meta meta;
	struct anx_state_object *obj;
	int ret;

	anx_objstore_init();

	/* ndim = 0 rejected */
	anx_memset(&meta, 0, sizeof(meta));
	meta.dtype = ANX_DTYPE_FLOAT32;
	ret = anx_tensor_create(&meta, NULL, 0, &obj);
	if (ret != ANX_EINVAL)
		return -1;

	/* ndim too big rejected */
	meta.ndim = ANX_TENSOR_MAX_DIMS + 1;
	ret = anx_tensor_create(&meta, NULL, 0, &obj);
	if (ret != ANX_EINVAL)
		return -2;

	/* Bad dtype rejected */
	anx_memset(&meta, 0, sizeof(meta));
	meta.ndim = 1;
	meta.shape[0] = 4;
	meta.dtype = ANX_DTYPE_COUNT;
	ret = anx_tensor_create(&meta, NULL, 0, &obj);
	if (ret != ANX_EINVAL)
		return -3;

	/* Mismatched data size rejected */
	anx_memset(&meta, 0, sizeof(meta));
	meta.ndim = 1;
	meta.shape[0] = 4;
	meta.dtype = ANX_DTYPE_FLOAT32;
	{
		float bogus[2] = { 0 };

		ret = anx_tensor_create(&meta, bogus, sizeof(bogus), &obj);
		if (ret != ANX_EINVAL)
			return -4;
	}

	/* Missing oid rejected */
	{
		struct anx_tensor_meta out;

		ret = anx_tensor_get_meta(NULL, &out);
		if (ret != ANX_EINVAL)
			return -5;
	}

	return 0;
}

int test_tensor(void)
{
	int ret;

	ret = test_dtype_helpers();
	if (ret != 0)
		return ret;

	ret = test_create_and_readback_fp32();
	if (ret != 0)
		return ret - 20;

	ret = test_seal_computes_brin();
	if (ret != 0)
		return ret - 40;

	ret = test_sparsity_search();
	if (ret != 0)
		return ret - 60;

	ret = test_bfloat16_brin();
	if (ret != 0)
		return ret - 80;

	ret = test_int8_brin();
	if (ret != 0)
		return ret - 100;

	ret = test_invalid_inputs();
	if (ret != 0)
		return ret - 120;

	return 0;
}
