/*
 * model.c — Model Namespace management (RFC-0013).
 *
 * Creates model namespaces under "models", stores manifests as
 * structured data State Objects, and provides layer listing.
 */

#include <anx/types.h>
#include <anx/model.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/meta.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/kprintf.h>

/* Ensure the "models" namespace exists */
static int ensure_models_ns(void)
{
	/* Try creating; ignore EEXIST */
	int ret = anx_ns_create("models");

	if (ret != ANX_OK && ret != ANX_EEXIST)
		return ret;
	return ANX_OK;
}

int anx_model_create(const char *name,
		      const struct anx_model_manifest *manifest)
{
	struct anx_so_create_params params;
	struct anx_state_object *manifest_obj;
	char path[256];
	int ret;

	if (!name || !manifest)
		return ANX_EINVAL;

	ret = ensure_models_ns();
	if (ret != ANX_OK)
		return ret;

	/* Create the manifest as a structured data object */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_STRUCTURED_DATA;
	params.schema_uri = "anx://model/manifest/v1";

	ret = anx_so_create(&params, &manifest_obj);
	if (ret != ANX_OK)
		return ret;

	/* Store manifest fields in system_meta */
	if (!manifest_obj->system_meta)
		manifest_obj->system_meta = anx_meta_create();
	if (!manifest_obj->system_meta) {
		anx_objstore_release(manifest_obj);
		return ANX_ENOMEM;
	}

	anx_meta_set_str(manifest_obj->system_meta, "model.name",
			  manifest->name);
	anx_meta_set_str(manifest_obj->system_meta, "model.arch",
			  manifest->architecture);
	anx_meta_set_i64(manifest_obj->system_meta, "model.params",
			  (int64_t)manifest->parameters);
	anx_meta_set_i64(manifest_obj->system_meta, "model.layers",
			  (int64_t)manifest->layers);
	anx_meta_set_i64(manifest_obj->system_meta, "model.hidden_dim",
			  (int64_t)manifest->hidden_dim);
	anx_meta_set_i64(manifest_obj->system_meta, "model.vocab_size",
			  (int64_t)manifest->vocab_size);
	anx_meta_set_i64(manifest_obj->system_meta, "model.dtype",
			  (int64_t)manifest->dtype);

	/* Bind manifest to models:/<name>/manifest */
	{
		uint32_t nlen = (uint32_t)anx_strlen(name);

		path[0] = '/';
		anx_memcpy(path + 1, name, nlen);
		anx_memcpy(path + 1 + nlen, "/manifest", 10);
	}

	ret = anx_ns_bind("models", path, &manifest_obj->oid);
	if (ret != ANX_OK) {
		anx_objstore_release(manifest_obj);
		return ret;
	}

	anx_objstore_release(manifest_obj);
	return ANX_OK;
}

int anx_model_manifest_get(const char *name,
			     struct anx_model_manifest *out)
{
	char path[256];
	anx_oid_t oid;
	struct anx_state_object *obj;
	const struct anx_meta_value *val;
	int ret;

	if (!name || !out)
		return ANX_EINVAL;

	{
		uint32_t nlen = (uint32_t)anx_strlen(name);

		path[0] = '/';
		anx_memcpy(path + 1, name, nlen);
		anx_memcpy(path + 1 + nlen, "/manifest", 10);
	}

	ret = anx_ns_resolve("models", path, &oid);
	if (ret != ANX_OK)
		return ret;

	obj = anx_objstore_lookup(&oid);
	if (!obj)
		return ANX_ENOENT;

	anx_memset(out, 0, sizeof(*out));

	if (obj->system_meta) {
		val = anx_meta_get(obj->system_meta, "model.name");
		if (val && val->type == ANX_META_STRING)
			anx_strlcpy(out->name, val->v.str.data,
				     ANX_MODEL_NAME_MAX);

		val = anx_meta_get(obj->system_meta, "model.arch");
		if (val && val->type == ANX_META_STRING)
			anx_strlcpy(out->architecture, val->v.str.data,
				     ANX_MODEL_ARCH_MAX);

		val = anx_meta_get(obj->system_meta, "model.params");
		if (val) out->parameters = (uint64_t)val->v.i64;

		val = anx_meta_get(obj->system_meta, "model.layers");
		if (val) out->layers = (uint32_t)val->v.i64;

		val = anx_meta_get(obj->system_meta, "model.hidden_dim");
		if (val) out->hidden_dim = (uint32_t)val->v.i64;

		val = anx_meta_get(obj->system_meta, "model.vocab_size");
		if (val) out->vocab_size = (uint32_t)val->v.i64;

		val = anx_meta_get(obj->system_meta, "model.dtype");
		if (val) out->dtype = (uint32_t)val->v.i64;
	}

	anx_objstore_release(obj);
	return ANX_OK;
}

int anx_model_add_layer(const char *model_name, const char *layer_path,
			  const anx_oid_t *tensor_oid)
{
	char full_path[256];
	uint32_t nlen, llen;
	int ret;

	if (!model_name || !layer_path || !tensor_oid)
		return ANX_EINVAL;

	ret = ensure_models_ns();
	if (ret != ANX_OK)
		return ret;

	nlen = (uint32_t)anx_strlen(model_name);
	llen = (uint32_t)anx_strlen(layer_path);

	if (1 + nlen + 8 + llen + 1 > sizeof(full_path))
		return ANX_EINVAL;

	/* Build path: /<model>/layers/<layer_path> */
	full_path[0] = '/';
	anx_memcpy(full_path + 1, model_name, nlen);
	anx_memcpy(full_path + 1 + nlen, "/layers/", 8);
	anx_memcpy(full_path + 1 + nlen + 8, layer_path, llen + 1);

	return anx_ns_bind("models", full_path, tensor_oid);
}

int anx_model_list_layers(const char *name,
			    struct anx_model_layer_info *out,
			    uint32_t max_layers, uint32_t *count)
{
	char path[256];
	struct anx_ns_list_entry entries[64];
	uint32_t entry_count = 0;
	uint32_t found = 0;
	uint32_t i;
	int ret;

	if (!name || !out || !count)
		return ANX_EINVAL;

	{
		uint32_t nlen = (uint32_t)anx_strlen(name);

		path[0] = '/';
		anx_memcpy(path + 1, name, nlen);
		anx_memcpy(path + 1 + nlen, "/layers", 8);
	}

	ret = anx_ns_list("models", path, entries, 64, &entry_count);
	if (ret != ANX_OK) {
		*count = 0;
		return ret;
	}

	for (i = 0; i < entry_count && found < max_layers; i++) {
		struct anx_tensor_meta tmeta;

		if (entries[i].is_directory)
			continue;

		ret = anx_tensor_meta_get(&entries[i].oid, &tmeta);
		if (ret != ANX_OK)
			continue;

		anx_strlcpy(out[found].name, entries[i].name, 128);
		out[found].ndim = tmeta.ndim;
		anx_memcpy(out[found].shape, tmeta.shape,
			   sizeof(tmeta.shape));
		out[found].dtype = (uint32_t)tmeta.dtype;
		out[found].byte_size = tmeta.byte_size;
		found++;
	}

	*count = found;
	return ANX_OK;
}

int anx_model_diff(const char *name_a, const char *name_b,
		    char diff_names[][128], uint32_t max_diffs,
		    uint32_t *count)
{
	struct anx_model_layer_info layers_a[64], layers_b[64];
	uint32_t count_a = 0, count_b = 0;
	uint32_t found = 0;
	uint32_t i, j;
	int ret;

	if (!name_a || !name_b || !diff_names || !count)
		return ANX_EINVAL;

	ret = anx_model_list_layers(name_a, layers_a, 64, &count_a);
	if (ret != ANX_OK)
		return ret;

	ret = anx_model_list_layers(name_b, layers_b, 64, &count_b);
	if (ret != ANX_OK)
		return ret;

	/* Find layers in A that differ from B (different shape, dtype, or size) */
	for (i = 0; i < count_a && found < max_diffs; i++) {
		bool matched = false;

		for (j = 0; j < count_b; j++) {
			if (anx_strcmp(layers_a[i].name,
				       layers_b[j].name) != 0)
				continue;

			/* Same name — check if they differ */
			if (layers_a[i].byte_size != layers_b[j].byte_size ||
			    layers_a[i].dtype != layers_b[j].dtype) {
				anx_strlcpy(diff_names[found],
					     layers_a[i].name, 128);
				found++;
			}
			matched = true;
			break;
		}

		/* Layer in A but not in B */
		if (!matched && found < max_diffs) {
			anx_strlcpy(diff_names[found],
				     layers_a[i].name, 128);
			found++;
		}
	}

	/* Layers in B but not in A */
	for (j = 0; j < count_b && found < max_diffs; j++) {
		bool in_a = false;

		for (i = 0; i < count_a; i++) {
			if (anx_strcmp(layers_b[j].name,
				       layers_a[i].name) == 0) {
				in_a = true;
				break;
			}
		}
		if (!in_a) {
			anx_strlcpy(diff_names[found],
				     layers_b[j].name, 128);
			found++;
		}
	}

	*count = found;
	return ANX_OK;
}
