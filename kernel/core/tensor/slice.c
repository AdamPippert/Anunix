/*
 * slice.c — Row-slice a Tensor Object along the first dimension.
 *
 * Copies a contiguous range of rows [start, end) from a source tensor
 * into a new tensor. The new tensor has shape[0] = end - start, with
 * all other dimensions preserved. This is a copy — the source is not
 * modified.
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>

int anx_tensor_slice(const anx_oid_t *src_oid, uint64_t start, uint64_t end,
		      struct anx_state_object **out)
{
	struct anx_state_object *src;
	struct anx_tensor_meta src_meta, dst_meta;
	uint64_t row_size;
	uint64_t slice_rows, slice_bytes;
	const uint8_t *src_data;
	int ret;

	if (!src_oid || !out || start >= end)
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

	if (src_meta.ndim < 1 || end > src_meta.shape[0]) {
		anx_objstore_release(src);
		return ANX_EINVAL;
	}

	/* Compute row size: product of all dims except the first */
	{
		uint32_t elem_size = anx_tensor_dtype_size(src_meta.dtype);
		uint64_t elems_per_row = 1;
		uint32_t d;

		for (d = 1; d < src_meta.ndim; d++)
			elems_per_row *= src_meta.shape[d];

		if (src_meta.dtype == ANX_DTYPE_INT4)
			row_size = (elems_per_row + 1) / 2;
		else if (src_meta.dtype == ANX_DTYPE_BOOL)
			row_size = (elems_per_row + 7) / 8;
		else
			row_size = elems_per_row * elem_size;
	}

	slice_rows = end - start;
	slice_bytes = slice_rows * row_size;

	/* Build destination metadata */
	dst_meta = src_meta;
	dst_meta.shape[0] = slice_rows;
	dst_meta.elem_count = slice_rows;
	{
		uint32_t d;

		for (d = 1; d < dst_meta.ndim; d++)
			dst_meta.elem_count *= dst_meta.shape[d];
	}

	/* Create with the sliced data */
	src_data = (const uint8_t *)src->payload + (start * row_size);
	ret = anx_tensor_create(&dst_meta, src_data, slice_bytes, out);

	anx_objstore_release(src);
	return ret;
}
