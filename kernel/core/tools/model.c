/*
 * model.c — Model namespace shell tool (RFC-0013).
 *
 * USAGE
 *   model info <name>           Show model manifest
 *   model layers <name>         List layer tensors
 *   model diff <name-a> <name-b>  Compare two models
 *   model import <ns:path> <name> Import safetensors from object
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/model.h>
#include <anx/tensor.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>
#include <anx/string.h>

static void cmd_model_info_sub(int argc, char **argv)
{
	struct anx_model_manifest manifest;
	int ret;

	if (argc < 3) {
		kprintf("usage: model info <name>\n");
		return;
	}

	ret = anx_model_manifest_get(argv[2], &manifest);
	if (ret != ANX_OK) {
		kprintf("error: model '%s' not found (%d)\n", argv[2], ret);
		return;
	}

	kprintf("  name:       %s\n", manifest.name);
	kprintf("  arch:       %s\n", manifest.architecture);
	kprintf("  parameters: %u\n", (unsigned)manifest.parameters);
	kprintf("  layers:     %u\n", (unsigned)manifest.layers);
	if (manifest.hidden_dim)
		kprintf("  hidden_dim: %u\n", (unsigned)manifest.hidden_dim);
	if (manifest.vocab_size)
		kprintf("  vocab_size: %u\n", (unsigned)manifest.vocab_size);
	if (manifest.dtype)
		kprintf("  dtype:      %s\n",
			anx_tensor_dtype_name(
				(enum anx_tensor_dtype)manifest.dtype));
}

static void cmd_model_layers_sub(int argc, char **argv)
{
	struct anx_model_layer_info layers[64];
	uint32_t count = 0;
	uint32_t i;
	int ret;

	if (argc < 3) {
		kprintf("usage: model layers <name>\n");
		return;
	}

	ret = anx_model_list_layers(argv[2], layers, 64, &count);
	if (ret != ANX_OK) {
		kprintf("error: cannot list layers for '%s' (%d)\n",
			argv[2], ret);
		return;
	}

	if (count == 0) {
		kprintf("no layers found for model '%s'\n", argv[2]);
		return;
	}

	kprintf("  %-24s  %-16s  %-8s  %s\n",
		"LAYER", "SHAPE", "DTYPE", "SIZE");

	for (i = 0; i < count; i++) {
		char shape_buf[64];
		uint32_t pos = 0;
		uint32_t d;

		shape_buf[pos++] = '[';
		for (d = 0; d < layers[i].ndim; d++) {
			/* Simple int-to-str for shape dims */
			uint64_t v = layers[i].shape[d];
			char num[20];
			int npos = 0;

			if (v == 0) {
				num[npos++] = '0';
			} else {
				uint64_t tmp = v;
				char rev[20];
				int rpos = 0;

				while (tmp > 0) {
					rev[rpos++] = '0' + (char)(tmp % 10);
					tmp /= 10;
				}
				while (rpos > 0)
					num[npos++] = rev[--rpos];
			}

			if (pos + (uint32_t)npos + 2 < sizeof(shape_buf)) {
				anx_memcpy(shape_buf + pos, num, (uint32_t)npos);
				pos += (uint32_t)npos;
				if (d + 1 < layers[i].ndim)
					shape_buf[pos++] = ',';
			}
		}
		shape_buf[pos++] = ']';
		shape_buf[pos] = '\0';

		kprintf("  %-24s  %-16s  %-8s",
			layers[i].name, shape_buf,
			anx_tensor_dtype_name(
				(enum anx_tensor_dtype)layers[i].dtype));

		if (layers[i].byte_size >= 1024 * 1024)
			kprintf("  %u MiB",
				(unsigned)(layers[i].byte_size / (1024 * 1024)));
		else if (layers[i].byte_size >= 1024)
			kprintf("  %u KiB",
				(unsigned)(layers[i].byte_size / 1024));
		else
			kprintf("  %u B", (unsigned)layers[i].byte_size);
		kprintf("\n");
	}
}

static void cmd_model_diff_sub(int argc, char **argv)
{
	char diff_names[64][128];
	uint32_t count = 0;
	uint32_t i;
	int ret;

	if (argc < 4) {
		kprintf("usage: model diff <name-a> <name-b>\n");
		return;
	}

	ret = anx_model_diff(argv[2], argv[3], diff_names, 64, &count);
	if (ret != ANX_OK) {
		kprintf("error: diff failed (%d)\n", ret);
		return;
	}

	if (count == 0) {
		kprintf("models '%s' and '%s' are identical\n",
			argv[2], argv[3]);
		return;
	}

	kprintf("%u layer(s) differ:\n", (unsigned)count);
	for (i = 0; i < count; i++)
		kprintf("  %s\n", diff_names[i]);
}

static void cmd_model_import_sub(int argc, char **argv)
{
	const char *ns_name = "default";
	const char *path = NULL;
	const char *model_name;
	const char *colon;
	anx_oid_t oid;
	struct anx_state_object *obj;
	uint32_t tensor_count = 0;
	int ret;

	if (argc < 4) {
		kprintf("usage: model import <ns:path> <model-name>\n");
		kprintf("  imports safetensors data from a State Object\n");
		return;
	}

	/* Parse ns:path for the source data object */
	colon = argv[2];
	while (*colon && *colon != ':')
		colon++;
	if (*colon == ':') {
		static char ns_buf[64];
		uint32_t ns_len = (uint32_t)(colon - argv[2]);

		if (ns_len < sizeof(ns_buf)) {
			anx_memcpy(ns_buf, argv[2], ns_len);
			ns_buf[ns_len] = '\0';
			ns_name = ns_buf;
		}
		path = colon + 1;
	} else {
		path = argv[2];
	}

	model_name = argv[3];

	/* Resolve the source object containing safetensors data */
	ret = anx_ns_resolve(ns_name, path, &oid);
	if (ret != ANX_OK) {
		kprintf("error: '%s:%s' not found\n", ns_name, path);
		return;
	}

	obj = anx_objstore_lookup(&oid);
	if (!obj || !obj->payload || obj->payload_size == 0) {
		kprintf("error: object has no payload\n");
		if (obj) anx_objstore_release(obj);
		return;
	}

	ret = anx_model_import_safetensors(model_name, obj->payload,
					    obj->payload_size, &tensor_count);
	anx_objstore_release(obj);

	if (ret != ANX_OK) {
		kprintf("error: import failed (%d)\n", ret);
		return;
	}

	kprintf("imported %u tensors into models:/%s/\n",
		(unsigned)tensor_count, model_name);
}

void cmd_model(int argc, char **argv)
{
	if (argc < 2) {
		kprintf("usage: model <info|layers|diff|import> [args]\n");
		return;
	}

	if (anx_strcmp(argv[1], "info") == 0)
		cmd_model_info_sub(argc, argv);
	else if (anx_strcmp(argv[1], "layers") == 0)
		cmd_model_layers_sub(argc, argv);
	else if (anx_strcmp(argv[1], "diff") == 0)
		cmd_model_diff_sub(argc, argv);
	else if (anx_strcmp(argv[1], "import") == 0)
		cmd_model_import_sub(argc, argv);
	else
		kprintf("unknown model command: %s\n", argv[1]);
}
