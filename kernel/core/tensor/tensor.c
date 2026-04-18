/*
 * tensor.c — Tensor Object core implementation (RFC-0013).
 *
 * Creates, seals, and queries Tensor Objects. Wraps the State Object
 * API with tensor-specific metadata (shape, dtype, BRIN stats).
 */

#include <anx/types.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/meta.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* --- Dtype helpers --- */

uint32_t anx_tensor_dtype_size(enum anx_tensor_dtype dtype)
{
	switch (dtype) {
	case ANX_DTYPE_FLOAT16:		return 2;
	case ANX_DTYPE_BFLOAT16:	return 2;
	case ANX_DTYPE_FLOAT32:		return 4;
	case ANX_DTYPE_FLOAT64:		return 8;
	case ANX_DTYPE_INT8:		return 1;
	case ANX_DTYPE_UINT8:		return 1;
	case ANX_DTYPE_INT4:		return 0; /* special: 2 per byte */
	case ANX_DTYPE_INT32:		return 4;
	case ANX_DTYPE_BOOL:		return 0; /* special: 8 per byte */
	default:			return 0;
	}
}

const char *anx_tensor_dtype_name(enum anx_tensor_dtype dtype)
{
	switch (dtype) {
	case ANX_DTYPE_FLOAT16:		return "float16";
	case ANX_DTYPE_BFLOAT16:	return "bfloat16";
	case ANX_DTYPE_FLOAT32:		return "float32";
	case ANX_DTYPE_FLOAT64:		return "float64";
	case ANX_DTYPE_INT8:		return "int8";
	case ANX_DTYPE_UINT8:		return "uint8";
	case ANX_DTYPE_INT4:		return "int4";
	case ANX_DTYPE_INT32:		return "int32";
	case ANX_DTYPE_BOOL:		return "bool";
	default:			return "unknown";
	}
}

static uint64_t compute_byte_size(const struct anx_tensor_meta *meta)
{
	uint32_t elem_size = anx_tensor_dtype_size(meta->dtype);

	if (meta->dtype == ANX_DTYPE_INT4)
		return (meta->elem_count + 1) / 2;
	if (meta->dtype == ANX_DTYPE_BOOL)
		return (meta->elem_count + 7) / 8;
	return meta->elem_count * elem_size;
}

static uint64_t compute_elem_count(const struct anx_tensor_meta *meta)
{
	uint64_t count = 1;
	uint32_t i;

	for (i = 0; i < meta->ndim; i++)
		count *= meta->shape[i];
	return count;
}

/* Store tensor metadata in the object's system_meta */
static int store_tensor_meta(struct anx_state_object *obj,
			      const struct anx_tensor_meta *meta)
{
	if (!obj->system_meta)
		obj->system_meta = anx_meta_create();
	if (!obj->system_meta)
		return ANX_ENOMEM;

	anx_meta_set_i64(obj->system_meta, "tensor.ndim",
			  (int64_t)meta->ndim);
	anx_meta_set_i64(obj->system_meta, "tensor.dtype",
			  (int64_t)meta->dtype);
	anx_meta_set_i64(obj->system_meta, "tensor.elem_count",
			  (int64_t)meta->elem_count);
	anx_meta_set_i64(obj->system_meta, "tensor.byte_size",
			  (int64_t)meta->byte_size);

	/* Store shape as individual dimension entries */
	{
		uint32_t i;
		char key[32];

		for (i = 0; i < meta->ndim && i < ANX_TENSOR_MAX_DIMS; i++) {
			key[0] = 't'; key[1] = '.'; key[2] = 's';
			key[3] = '0' + (char)i; key[4] = '\0';
			anx_meta_set_i64(obj->system_meta, key,
					  (int64_t)meta->shape[i]);
		}
	}

	/* BRIN stats (stored as int64 encoding of uint32 bit patterns) */
	anx_meta_set_i64(obj->system_meta, "brin.mean",
			  (int64_t)meta->stat_mean_bits);
	anx_meta_set_i64(obj->system_meta, "brin.var",
			  (int64_t)meta->stat_variance_bits);
	anx_meta_set_i64(obj->system_meta, "brin.norm",
			  (int64_t)meta->stat_l2_norm_bits);
	anx_meta_set_i64(obj->system_meta, "brin.sparsity",
			  (int64_t)meta->stat_sparsity_bits);
	anx_meta_set_i64(obj->system_meta, "brin.min",
			  (int64_t)meta->stat_min_bits);
	anx_meta_set_i64(obj->system_meta, "brin.max",
			  (int64_t)meta->stat_max_bits);

	return ANX_OK;
}

/* Read tensor metadata from system_meta */
static int load_tensor_meta(struct anx_state_object *obj,
			     struct anx_tensor_meta *meta)
{
	const struct anx_meta_value *val;
	uint32_t i;
	char key[32];

	if (!obj->system_meta)
		return ANX_ENOENT;

	anx_memset(meta, 0, sizeof(*meta));

	val = anx_meta_get(obj->system_meta, "tensor.ndim");
	if (val) meta->ndim = (uint32_t)val->v.i64;

	val = anx_meta_get(obj->system_meta, "tensor.dtype");
	if (val) meta->dtype = (enum anx_tensor_dtype)val->v.i64;

	val = anx_meta_get(obj->system_meta, "tensor.elem_count");
	if (val) meta->elem_count = (uint64_t)val->v.i64;

	val = anx_meta_get(obj->system_meta, "tensor.byte_size");
	if (val) meta->byte_size = (uint64_t)val->v.i64;

	for (i = 0; i < meta->ndim && i < ANX_TENSOR_MAX_DIMS; i++) {
		key[0] = 't'; key[1] = '.'; key[2] = 's';
		key[3] = '0' + (char)i; key[4] = '\0';
		val = anx_meta_get(obj->system_meta, key);
		if (val) meta->shape[i] = (uint64_t)val->v.i64;
	}

	val = anx_meta_get(obj->system_meta, "brin.mean");
	if (val) meta->stat_mean_bits = (uint32_t)val->v.i64;
	val = anx_meta_get(obj->system_meta, "brin.var");
	if (val) meta->stat_variance_bits = (uint32_t)val->v.i64;
	val = anx_meta_get(obj->system_meta, "brin.norm");
	if (val) meta->stat_l2_norm_bits = (uint32_t)val->v.i64;
	val = anx_meta_get(obj->system_meta, "brin.sparsity");
	if (val) meta->stat_sparsity_bits = (uint32_t)val->v.i64;
	val = anx_meta_get(obj->system_meta, "brin.min");
	if (val) meta->stat_min_bits = (uint32_t)val->v.i64;
	val = anx_meta_get(obj->system_meta, "brin.max");
	if (val) meta->stat_max_bits = (uint32_t)val->v.i64;

	return ANX_OK;
}

/* --- Public API --- */

int anx_tensor_create(const struct anx_tensor_meta *meta,
		       const void *data, uint64_t data_size,
		       struct anx_state_object **out)
{
	struct anx_so_create_params params;
	struct anx_tensor_meta validated;
	int ret;

	if (!meta || meta->ndim == 0 || meta->ndim > ANX_TENSOR_MAX_DIMS)
		return ANX_EINVAL;
	if (meta->dtype >= ANX_DTYPE_COUNT)
		return ANX_EINVAL;

	/* Compute derived fields */
	validated = *meta;
	validated.elem_count = compute_elem_count(&validated);
	validated.byte_size = compute_byte_size(&validated);

	/* Validate data size if provided */
	if (data && data_size > 0 && data_size != validated.byte_size)
		return ANX_EINVAL;

	/* Create the underlying State Object */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_TENSOR;
	params.payload = data;
	params.payload_size = data ? validated.byte_size : 0;

	ret = anx_so_create(&params, out);
	if (ret != ANX_OK)
		return ret;

	/* Attach tensor metadata */
	ret = store_tensor_meta(*out, &validated);
	if (ret != ANX_OK) {
		anx_objstore_release(*out);
		return ret;
	}

	/* If no data provided, allocate zeroed payload */
	if (!data && validated.byte_size > 0) {
		void *payload = anx_zalloc(validated.byte_size);

		if (!payload) {
			anx_objstore_release(*out);
			return ANX_ENOMEM;
		}
		(*out)->payload = payload;
		(*out)->payload_size = validated.byte_size;
	}

	return ANX_OK;
}

int anx_tensor_seal(const anx_oid_t *oid)
{
	struct anx_state_object *obj;
	struct anx_tensor_meta meta;
	int ret;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;
	if (obj->object_type != ANX_OBJ_TENSOR) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}

	/* Compute BRIN stats */
	ret = load_tensor_meta(obj, &meta);
	if (ret == ANX_OK && obj->payload && obj->payload_size > 0) {
		anx_tensor_compute_brin(obj, &meta);
		store_tensor_meta(obj, &meta);
	}

	anx_objstore_release(obj);

	/* Seal the underlying object */
	return anx_so_seal(oid);
}

int anx_tensor_meta_get(const anx_oid_t *oid,
			 struct anx_tensor_meta *meta_out)
{
	struct anx_state_object *obj;
	int ret;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;
	if (obj->object_type != ANX_OBJ_TENSOR) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}

	ret = load_tensor_meta(obj, meta_out);
	anx_objstore_release(obj);
	return ret;
}

int anx_tensor_fill(const anx_oid_t *oid, const char *pattern)
{
	struct anx_state_object *obj;
	struct anx_tensor_meta meta;
	int ret;

	obj = anx_objstore_lookup(oid);
	if (!obj)
		return ANX_ENOENT;
	if (obj->object_type != ANX_OBJ_TENSOR || !obj->payload) {
		anx_objstore_release(obj);
		return ANX_EINVAL;
	}

	ret = load_tensor_meta(obj, &meta);
	if (ret != ANX_OK) {
		anx_objstore_release(obj);
		return ret;
	}

	if (anx_strcmp(pattern, "zeros") == 0) {
		anx_memset(obj->payload, 0, (uint32_t)obj->payload_size);
	} else if (anx_strcmp(pattern, "ones") == 0) {
		if (meta.dtype == ANX_DTYPE_INT8 ||
		    meta.dtype == ANX_DTYPE_UINT8) {
			anx_memset(obj->payload, 1,
				   (uint32_t)obj->payload_size);
		} else if (meta.dtype == ANX_DTYPE_INT32) {
			int32_t *p = (int32_t *)obj->payload;
			uint64_t i;

			for (i = 0; i < meta.elem_count; i++)
				p[i] = 1;
		}
	} else if (anx_strcmp(pattern, "range") == 0) {
		if (meta.dtype == ANX_DTYPE_INT8) {
			int8_t *p = (int8_t *)obj->payload;
			uint64_t i;

			for (i = 0; i < meta.elem_count; i++)
				p[i] = (int8_t)(i & 0x7F);
		} else if (meta.dtype == ANX_DTYPE_INT32) {
			int32_t *p = (int32_t *)obj->payload;
			uint64_t i;

			for (i = 0; i < meta.elem_count; i++)
				p[i] = (int32_t)i;
		}
	}

	anx_objstore_release(obj);
	return ANX_OK;
}
