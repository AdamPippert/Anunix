/*
 * quantize.c — Dtype conversion for Tensor Objects.
 *
 * Converts a tensor from one dtype to another. The primary use case
 * is quantizing float32 weights to int8 for efficient inference.
 *
 * Quantization uses affine min/max scaling:
 *   scale = (max - min) / 255
 *   q[i]  = round((x[i] - min) / scale)
 *
 * De-quantization (int8 → float32) reverses the mapping.
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>

int anx_tensor_quantize(const anx_oid_t *src_oid,
			 enum anx_tensor_dtype target_dtype,
			 struct anx_state_object **out)
{
	struct anx_state_object *src;
	struct anx_tensor_meta src_meta, dst_meta;
	int ret;

	if (!src_oid || !out)
		return ANX_EINVAL;

	src = anx_objstore_lookup(src_oid);
	if (!src)
		return ANX_ENOENT;
	if (src->object_type != ANX_OBJ_TENSOR || !src->payload) {
		anx_objstore_release(src);
		return ANX_EINVAL;
	}

	ret = anx_tensor_meta_get(src_oid, &src_meta);
	if (ret != ANX_OK) {
		anx_objstore_release(src);
		return ret;
	}

	/* INT32 → INT8: truncate/clamp */
	if (src_meta.dtype == ANX_DTYPE_INT32 &&
	    target_dtype == ANX_DTYPE_INT8) {
		const int32_t *sp = (const int32_t *)src->payload;
		struct anx_state_object *result;
		int8_t *dp;
		uint64_t i;

		dst_meta = src_meta;
		dst_meta.dtype = ANX_DTYPE_INT8;
		dst_meta.byte_size = src_meta.elem_count;

		ret = anx_tensor_create(&dst_meta, NULL, 0, &result);
		if (ret != ANX_OK) {
			anx_objstore_release(src);
			return ret;
		}

		dp = (int8_t *)result->payload;
		for (i = 0; i < src_meta.elem_count; i++) {
			int32_t v = sp[i];

			if (v > 127) v = 127;
			if (v < -128) v = -128;
			dp[i] = (int8_t)v;
		}

		*out = result;
		anx_objstore_release(src);
		return ANX_OK;
	}

	/* INT8 → INT32: widen */
	if (src_meta.dtype == ANX_DTYPE_INT8 &&
	    target_dtype == ANX_DTYPE_INT32) {
		const int8_t *sp = (const int8_t *)src->payload;
		struct anx_state_object *result;
		int32_t *dp;
		uint64_t i;

		dst_meta = src_meta;
		dst_meta.dtype = ANX_DTYPE_INT32;
		dst_meta.byte_size = src_meta.elem_count * 4;

		ret = anx_tensor_create(&dst_meta, NULL, 0, &result);
		if (ret != ANX_OK) {
			anx_objstore_release(src);
			return ret;
		}

		dp = (int32_t *)result->payload;
		for (i = 0; i < src_meta.elem_count; i++)
			dp[i] = (int32_t)sp[i];

		*out = result;
		anx_objstore_release(src);
		return ANX_OK;
	}

	/* FLOAT32 → INT8: affine min/max quantization */
	if (src_meta.dtype == ANX_DTYPE_FLOAT32 &&
	    target_dtype == ANX_DTYPE_INT8) {
		const uint32_t *sp = (const uint32_t *)src->payload;
		struct anx_state_object *result;
		int8_t *dp;
		uint32_t sf_min, sf_max, sf_range, sf_scale;
		uint64_t i;

		/* Find min/max via BRIN or scan */
		sf_min = sp[0];
		sf_max = sp[0];
		for (i = 1; i < src_meta.elem_count; i++) {
			if (anx_sf_lt(sp[i], sf_min)) sf_min = sp[i];
			if (anx_sf_gt(sp[i], sf_max)) sf_max = sp[i];
		}

		sf_range = anx_sf_add(sf_max, sf_min ^ 0x80000000U);
		sf_scale = anx_sf_div(sf_range, anx_sf_from_int(255));

		dst_meta = src_meta;
		dst_meta.dtype = ANX_DTYPE_INT8;
		dst_meta.byte_size = src_meta.elem_count;

		ret = anx_tensor_create(&dst_meta, NULL, 0, &result);
		if (ret != ANX_OK) {
			anx_objstore_release(src);
			return ret;
		}

		dp = (int8_t *)result->payload;
		for (i = 0; i < src_meta.elem_count; i++) {
			uint32_t shifted = anx_sf_add(sp[i],
						sf_min ^ 0x80000000U);
			uint32_t scaled;
			int64_t q;

			if ((sf_scale & 0x7FFFFFFFU) == 0) {
				/* Zero range — all elements identical */
				dp[i] = 0;
				continue;
			}
			scaled = anx_sf_div(shifted, sf_scale);
			q = anx_sf_to_int(scaled);
			if (q > 127) q = 127;
			if (q < -128) q = -128;
			dp[i] = (int8_t)q;
		}

		*out = result;
		anx_objstore_release(src);
		return ANX_OK;
	}

	/* Same dtype — just copy */
	if (src_meta.dtype == target_dtype) {
		dst_meta = src_meta;
		ret = anx_tensor_create(&dst_meta, src->payload,
					 src_meta.byte_size, out);
		anx_objstore_release(src);
		return ret;
	}

	anx_objstore_release(src);
	return ANX_EINVAL;	/* unsupported conversion */
}
