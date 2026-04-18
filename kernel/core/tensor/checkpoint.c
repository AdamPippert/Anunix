/*
 * checkpoint.c — Model checkpoint and verification (RFC-0013).
 *
 * Seals all unsealed tensors in a model namespace (computing BRIN
 * stats for each), providing a consistent snapshot. Verification
 * checks that all tensors are sealed.
 */

#include <anx/types.h>
#include <anx/tensor_ops.h>
#include <anx/tensor.h>
#include <anx/model.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/string.h>

int anx_model_checkpoint(const char *model_name, uint32_t *sealed_count)
{
	struct anx_model_layer_info layers[ANX_MODEL_LAYERS_MAX];
	uint32_t count = 0;
	uint32_t sealed = 0;
	uint32_t i;
	char layer_path[256];
	int ret;

	if (!model_name || !sealed_count)
		return ANX_EINVAL;

	ret = anx_model_list_layers(model_name, layers,
				     ANX_MODEL_LAYERS_MAX, &count);
	if (ret != ANX_OK)
		return ret;

	for (i = 0; i < count; i++) {
		uint32_t nlen = (uint32_t)anx_strlen(model_name);
		uint32_t llen = (uint32_t)anx_strlen(layers[i].name);
		anx_oid_t oid;

		/* Build full path: /<model>/layers/<layer> */
		layer_path[0] = '/';
		anx_memcpy(layer_path + 1, model_name, nlen);
		anx_memcpy(layer_path + 1 + nlen, "/layers/", 8);
		anx_memcpy(layer_path + 1 + nlen + 8, layers[i].name,
			   llen + 1);

		ret = anx_ns_resolve("models", layer_path, &oid);
		if (ret != ANX_OK)
			continue;

		/* Try to seal — already-sealed tensors return an error
		 * which we ignore */
		ret = anx_tensor_seal(&oid);
		if (ret == ANX_OK)
			sealed++;
	}

	*sealed_count = sealed;
	return ANX_OK;
}

int anx_model_verify(const char *model_name, bool *all_sealed)
{
	struct anx_model_layer_info layers[ANX_MODEL_LAYERS_MAX];
	uint32_t count = 0;
	uint32_t i;
	char layer_path[256];
	int ret;

	if (!model_name || !all_sealed)
		return ANX_EINVAL;

	*all_sealed = true;

	ret = anx_model_list_layers(model_name, layers,
				     ANX_MODEL_LAYERS_MAX, &count);
	if (ret != ANX_OK)
		return ret;

	for (i = 0; i < count; i++) {
		uint32_t nlen = (uint32_t)anx_strlen(model_name);
		uint32_t llen = (uint32_t)anx_strlen(layers[i].name);
		anx_oid_t oid;
		struct anx_state_object *obj;

		layer_path[0] = '/';
		anx_memcpy(layer_path + 1, model_name, nlen);
		anx_memcpy(layer_path + 1 + nlen, "/layers/", 8);
		anx_memcpy(layer_path + 1 + nlen + 8, layers[i].name,
			   llen + 1);

		ret = anx_ns_resolve("models", layer_path, &oid);
		if (ret != ANX_OK)
			continue;

		obj = anx_objstore_lookup(&oid);
		if (!obj)
			continue;

		if (obj->state != ANX_OBJ_SEALED)
			*all_sealed = false;

		anx_objstore_release(obj);

		if (!*all_sealed)
			return ANX_OK;	/* early exit */
	}

	return ANX_OK;
}
