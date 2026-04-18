/*
 * diff.c — Compute element-wise delta between two Tensor Objects.
 *
 * Both tensors must have the same shape and dtype. The result is a
 * new tensor containing (a[i] - b[i]) for each element. For integer
 * dtypes this is straightforward subtraction; float dtypes use
 * softfloat.
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>

/* Verify two tensors have identical shape and dtype */
static bool shapes_match(const struct anx_tensor_meta *a,
			   const struct anx_tensor_meta *b)
{
	uint32_t d;

	if (a->ndim != b->ndim || a->dtype != b->dtype)
		return false;
	for (d = 0; d < a->ndim; d++) {
		if (a->shape[d] != b->shape[d])
			return false;
	}
	return true;
}

int anx_tensor_diff(const anx_oid_t *oid_a, const anx_oid_t *oid_b,
		     struct anx_state_object **out)
{
	struct anx_state_object *obj_a, *obj_b, *result;
	struct anx_tensor_meta meta_a, meta_b, meta_out;
	int ret;

	if (!oid_a || !oid_b || !out)
		return ANX_EINVAL;

	obj_a = anx_objstore_lookup(oid_a);
	if (!obj_a)
		return ANX_ENOENT;

	obj_b = anx_objstore_lookup(oid_b);
	if (!obj_b) {
		anx_objstore_release(obj_a);
		return ANX_ENOENT;
	}

	if (obj_a->object_type != ANX_OBJ_TENSOR ||
	    obj_b->object_type != ANX_OBJ_TENSOR ||
	    !obj_a->payload || !obj_b->payload) {
		anx_objstore_release(obj_b);
		anx_objstore_release(obj_a);
		return ANX_EINVAL;
	}

	ret = anx_tensor_meta_get(oid_a, &meta_a);
	if (ret != ANX_OK)
		goto out_release;

	ret = anx_tensor_meta_get(oid_b, &meta_b);
	if (ret != ANX_OK)
		goto out_release;

	if (!shapes_match(&meta_a, &meta_b)) {
		ret = ANX_EINVAL;
		goto out_release;
	}

	/* Create output tensor (same shape/dtype) */
	meta_out = meta_a;
	meta_out.is_delta = true;
	meta_out.parent_tensor = *oid_a;

	ret = anx_tensor_create(&meta_out, NULL, 0, &result);
	if (ret != ANX_OK)
		goto out_release;

	/* Element-wise subtraction */
	if (meta_a.dtype == ANX_DTYPE_INT8) {
		const int8_t *pa = (const int8_t *)obj_a->payload;
		const int8_t *pb = (const int8_t *)obj_b->payload;
		int8_t *pr = (int8_t *)result->payload;
		uint64_t i;

		for (i = 0; i < meta_a.elem_count; i++)
			pr[i] = pa[i] - pb[i];

	} else if (meta_a.dtype == ANX_DTYPE_UINT8) {
		const uint8_t *pa = (const uint8_t *)obj_a->payload;
		const uint8_t *pb = (const uint8_t *)obj_b->payload;
		uint8_t *pr = (uint8_t *)result->payload;
		uint64_t i;

		for (i = 0; i < meta_a.elem_count; i++)
			pr[i] = pa[i] - pb[i];

	} else if (meta_a.dtype == ANX_DTYPE_INT32) {
		const int32_t *pa = (const int32_t *)obj_a->payload;
		const int32_t *pb = (const int32_t *)obj_b->payload;
		int32_t *pr = (int32_t *)result->payload;
		uint64_t i;

		for (i = 0; i < meta_a.elem_count; i++)
			pr[i] = pa[i] - pb[i];

	} else if (meta_a.dtype == ANX_DTYPE_FLOAT32) {
		const uint32_t *pa = (const uint32_t *)obj_a->payload;
		const uint32_t *pb = (const uint32_t *)obj_b->payload;
		uint32_t *pr = (uint32_t *)result->payload;
		uint64_t i;

		for (i = 0; i < meta_a.elem_count; i++)
			pr[i] = anx_sf_add(pa[i], pb[i] ^ 0x80000000U);

	} else {
		/* Unsupported dtype for diff — byte-level XOR as fallback */
		const uint8_t *pa = (const uint8_t *)obj_a->payload;
		const uint8_t *pb = (const uint8_t *)obj_b->payload;
		uint8_t *pr = (uint8_t *)result->payload;
		uint64_t i;

		for (i = 0; i < meta_a.byte_size; i++)
			pr[i] = pa[i] ^ pb[i];
	}

	*out = result;
	anx_objstore_release(obj_b);
	anx_objstore_release(obj_a);
	return ANX_OK;

out_release:
	anx_objstore_release(obj_b);
	anx_objstore_release(obj_a);
	return ret;
}
