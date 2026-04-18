/*
 * test_model.c — Tests for Model Namespace (RFC-0013).
 */

#include <anx/types.h>
#include <anx/model.h>
#include <anx/tensor.h>
#include <anx/state_object.h>
#include <anx/namespace.h>
#include <anx/string.h>

int test_model(void)
{
	struct anx_model_manifest manifest, manifest_out;
	struct anx_model_layer_info layers[16];
	uint32_t layer_count = 0;
	int ret;

	anx_objstore_init();
	anx_ns_init();

	/* --- Test 1: Create a model --- */
	anx_memset(&manifest, 0, sizeof(manifest));
	anx_strlcpy(manifest.name, "test-model", ANX_MODEL_NAME_MAX);
	anx_strlcpy(manifest.architecture, "transformer",
		     ANX_MODEL_ARCH_MAX);
	manifest.parameters = 1000;
	manifest.layers = 3;
	manifest.hidden_dim = 64;
	manifest.vocab_size = 256;
	manifest.dtype = ANX_DTYPE_INT8;

	ret = anx_model_create("test-model", &manifest);
	if (ret != ANX_OK)
		return -1;

	/* --- Test 2: Read manifest back --- */
	ret = anx_model_manifest_get("test-model", &manifest_out);
	if (ret != ANX_OK)
		return -2;

	if (anx_strcmp(manifest_out.name, "test-model") != 0)
		return -3;
	if (anx_strcmp(manifest_out.architecture, "transformer") != 0)
		return -4;
	if (manifest_out.parameters != 1000)
		return -5;
	if (manifest_out.layers != 3)
		return -6;
	if (manifest_out.hidden_dim != 64)
		return -7;

	/* --- Test 3: Add layers --- */
	{
		struct anx_tensor_meta tmeta;
		struct anx_state_object *t1, *t2, *t3;

		anx_memset(&tmeta, 0, sizeof(tmeta));
		tmeta.ndim = 2;
		tmeta.shape[0] = 256;
		tmeta.shape[1] = 64;
		tmeta.dtype = ANX_DTYPE_INT8;

		ret = anx_tensor_create(&tmeta, NULL, 0, &t1);
		if (ret != ANX_OK)
			return -10;
		ret = anx_model_add_layer("test-model", "embed_tokens",
					   &t1->oid);
		if (ret != ANX_OK)
			return -11;

		tmeta.shape[0] = 64;
		tmeta.shape[1] = 64;
		ret = anx_tensor_create(&tmeta, NULL, 0, &t2);
		if (ret != ANX_OK)
			return -12;
		ret = anx_model_add_layer("test-model", "q_proj", &t2->oid);
		if (ret != ANX_OK)
			return -13;

		tmeta.shape[0] = 64;
		tmeta.shape[1] = 256;
		ret = anx_tensor_create(&tmeta, NULL, 0, &t3);
		if (ret != ANX_OK)
			return -14;
		ret = anx_model_add_layer("test-model", "lm_head", &t3->oid);
		if (ret != ANX_OK)
			return -15;

		anx_objstore_release(t1);
		anx_objstore_release(t2);
		anx_objstore_release(t3);
	}

	/* --- Test 4: List layers --- */
	ret = anx_model_list_layers("test-model", layers, 16, &layer_count);
	if (ret != ANX_OK)
		return -20;
	if (layer_count != 3)
		return -21;

	/* --- Test 5: Model not found --- */
	ret = anx_model_manifest_get("nonexistent", &manifest_out);
	if (ret == ANX_OK)
		return -30;	/* should fail */

	/* --- Test 6: Safetensors import --- */
	{
		/*
		 * Build a minimal safetensors buffer:
		 *   8 bytes: header size
		 *   N bytes: JSON header
		 *   M bytes: raw tensor data
		 */
		static const char header[] =
			"{\"weight\": {\"dtype\": \"I8\", \"shape\": [2, 3],"
			" \"data_offsets\": [0, 6]}}";
		uint32_t hlen = (uint32_t)anx_strlen(header);
		uint8_t buf[256];
		uint32_t import_count = 0;

		/* Header size (little-endian) */
		buf[0] = (uint8_t)(hlen & 0xFF);
		buf[1] = (uint8_t)((hlen >> 8) & 0xFF);
		buf[2] = 0; buf[3] = 0;
		buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 0;

		/* JSON header */
		anx_memcpy(buf + 8, header, hlen);

		/* Tensor data: 6 bytes of [1, 2, 3, 4, 5, 6] */
		buf[8 + hlen + 0] = 1;
		buf[8 + hlen + 1] = 2;
		buf[8 + hlen + 2] = 3;
		buf[8 + hlen + 3] = 4;
		buf[8 + hlen + 4] = 5;
		buf[8 + hlen + 5] = 6;

		ret = anx_model_import_safetensors("imported-model",
						    buf, 8 + hlen + 6,
						    &import_count);
		if (ret != ANX_OK)
			return -40;
		if (import_count != 1)
			return -41;

		/* Verify the imported tensor */
		{
			struct anx_model_layer_info imp_layers[8];
			uint32_t imp_count = 0;

			ret = anx_model_list_layers("imported-model",
						     imp_layers, 8,
						     &imp_count);
			if (ret != ANX_OK)
				return -42;
			if (imp_count != 1)
				return -43;
			if (imp_layers[0].ndim != 2)
				return -44;
			if (imp_layers[0].shape[0] != 2 ||
			    imp_layers[0].shape[1] != 3)
				return -45;
		}
	}

	/* --- Test 7: Model diff (same model vs different) --- */
	{
		char diff_names[16][128];
		uint32_t diff_count = 0;

		/* Diff against itself — should find 0 differences */
		ret = anx_model_diff("test-model", "test-model",
				      diff_names, 16, &diff_count);
		if (ret != ANX_OK)
			return -50;
		if (diff_count != 0)
			return -51;

		/* Diff test-model vs imported-model — all layers differ */
		ret = anx_model_diff("test-model", "imported-model",
				      diff_names, 16, &diff_count);
		if (ret != ANX_OK)
			return -52;
		/* test-model has 3 layers not in imported, imported has 1
		 * not in test — total 4 differences */
		if (diff_count != 4)
			return -53;
	}

	return 0;
}
