/*
 * safetensors.c — Safetensors format import (RFC-0013).
 *
 * Safetensors file format:
 *   [8 bytes] header_size (little-endian uint64)
 *   [header_size bytes] JSON header
 *   [remaining bytes] raw tensor data
 *
 * The JSON header maps tensor names to descriptors:
 *   { "layer_name": { "dtype": "F32", "shape": [4096, 4096],
 *                     "data_offsets": [start, end] }, ... }
 *
 * We parse the header, create tensor objects for each entry, and
 * copy the raw data from the appropriate offsets.
 */

#include <anx/types.h>
#include <anx/model.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/json.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Map safetensors dtype strings to our enum */
static int parse_st_dtype(const char *s, enum anx_tensor_dtype *out)
{
	if (!s)
		return ANX_EINVAL;

	if (anx_strcmp(s, "F32") == 0)
		*out = ANX_DTYPE_FLOAT32;
	else if (anx_strcmp(s, "F16") == 0)
		*out = ANX_DTYPE_FLOAT16;
	else if (anx_strcmp(s, "BF16") == 0)
		*out = ANX_DTYPE_BFLOAT16;
	else if (anx_strcmp(s, "F64") == 0)
		*out = ANX_DTYPE_FLOAT64;
	else if (anx_strcmp(s, "I8") == 0)
		*out = ANX_DTYPE_INT8;
	else if (anx_strcmp(s, "U8") == 0)
		*out = ANX_DTYPE_UINT8;
	else if (anx_strcmp(s, "I32") == 0)
		*out = ANX_DTYPE_INT32;
	else if (anx_strcmp(s, "BOOL") == 0)
		*out = ANX_DTYPE_BOOL;
	else
		return ANX_EINVAL;

	return ANX_OK;
}

int anx_model_import_safetensors(const char *model_name,
				  const void *data, uint64_t data_size,
				  uint32_t *tensor_count_out)
{
	const uint8_t *raw = (const uint8_t *)data;
	uint64_t header_size;
	const char *header_json;
	struct anx_json_value root;
	struct anx_model_manifest manifest;
	uint32_t count = 0;
	uint32_t i;
	const uint8_t *tensor_data;
	int ret;

	if (!model_name || !data || data_size < 8 || !tensor_count_out)
		return ANX_EINVAL;

	/* Read header size (8 bytes, little-endian) */
	header_size = (uint64_t)raw[0] |
		      ((uint64_t)raw[1] << 8) |
		      ((uint64_t)raw[2] << 16) |
		      ((uint64_t)raw[3] << 24) |
		      ((uint64_t)raw[4] << 32) |
		      ((uint64_t)raw[5] << 40) |
		      ((uint64_t)raw[6] << 48) |
		      ((uint64_t)raw[7] << 56);

	if (8 + header_size > data_size)
		return ANX_EINVAL;

	header_json = (const char *)(raw + 8);
	tensor_data = raw + 8 + header_size;

	/* Parse JSON header */
	ret = anx_json_parse(header_json, (uint32_t)header_size, &root);
	if (ret != ANX_OK)
		return ret;

	if (root.type != ANX_JSON_OBJECT) {
		anx_json_free(&root);
		return ANX_EINVAL;
	}

	/* Create model namespace with basic manifest */
	anx_memset(&manifest, 0, sizeof(manifest));
	anx_strlcpy(manifest.name, model_name, ANX_MODEL_NAME_MAX);
	anx_strlcpy(manifest.architecture, "imported", ANX_MODEL_ARCH_MAX);

	ret = anx_model_create(model_name, &manifest);
	if (ret != ANX_OK && ret != ANX_EEXIST) {
		anx_json_free(&root);
		return ret;
	}

	/* Iterate tensor entries */
	for (i = 0; i < root.v.object.count; i++) {
		struct anx_json_kv *kv = &root.v.object.pairs[i];
		struct anx_json_value *desc = &kv->value;
		struct anx_json_value *dtype_val, *shape_val, *offsets_val;
		struct anx_tensor_meta tmeta;
		struct anx_state_object *tobj;
		enum anx_tensor_dtype dtype;
		uint64_t data_start, data_end;
		uint32_t d;

		/* Skip __metadata__ key */
		if (kv->key[0] == '_')
			continue;

		if (desc->type != ANX_JSON_OBJECT)
			continue;

		/* Parse dtype */
		dtype_val = anx_json_get(desc, "dtype");
		if (!dtype_val)
			continue;
		if (parse_st_dtype(anx_json_string(dtype_val), &dtype) != ANX_OK)
			continue;

		/* Parse shape */
		shape_val = anx_json_get(desc, "shape");
		if (!shape_val || shape_val->type != ANX_JSON_ARRAY)
			continue;

		anx_memset(&tmeta, 0, sizeof(tmeta));
		tmeta.dtype = dtype;
		tmeta.ndim = anx_json_array_len(shape_val);
		if (tmeta.ndim == 0 || tmeta.ndim > ANX_TENSOR_MAX_DIMS)
			continue;

		for (d = 0; d < tmeta.ndim; d++) {
			struct anx_json_value *dim =
				anx_json_array_get(shape_val, d);
			if (dim)
				tmeta.shape[d] = (uint64_t)anx_json_number(dim);
		}

		/* Parse data_offsets [start, end] */
		offsets_val = anx_json_get(desc, "data_offsets");
		if (!offsets_val || offsets_val->type != ANX_JSON_ARRAY ||
		    anx_json_array_len(offsets_val) < 2)
			continue;

		data_start = (uint64_t)anx_json_number(
			anx_json_array_get(offsets_val, 0));
		data_end = (uint64_t)anx_json_number(
			anx_json_array_get(offsets_val, 1));

		if (data_end <= data_start)
			continue;

		/* Bounds check against actual data */
		if (tensor_data + data_end > raw + data_size)
			continue;

		/* Create tensor with the raw data */
		ret = anx_tensor_create(&tmeta, tensor_data + data_start,
					 data_end - data_start, &tobj);
		if (ret != ANX_OK)
			continue;

		/* Bind into model layer namespace */
		ret = anx_model_add_layer(model_name, kv->key, &tobj->oid);
		if (ret != ANX_OK) {
			anx_objstore_release(tobj);
			continue;
		}

		/* Seal to compute BRIN stats */
		anx_tensor_seal(&tobj->oid);

		anx_objstore_release(tobj);
		count++;
	}

	/* Update manifest with actual layer count */
	{
		char mpath[256];
		anx_oid_t moid;
		struct anx_state_object *mobj;
		uint32_t nlen = (uint32_t)anx_strlen(model_name);

		mpath[0] = '/';
		anx_memcpy(mpath + 1, model_name, nlen);
		anx_memcpy(mpath + 1 + nlen, "/manifest", 10);

		if (anx_ns_resolve("models", mpath, &moid) == ANX_OK) {
			mobj = anx_objstore_lookup(&moid);
			if (mobj && mobj->system_meta) {
				anx_meta_set_i64(mobj->system_meta,
						  "model.layers",
						  (int64_t)count);
				anx_objstore_release(mobj);
			}
		}
	}

	anx_json_free(&root);

	*tensor_count_out = count;
	return ANX_OK;
}
