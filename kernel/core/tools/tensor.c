/*
 * tensor.c — Tensor Object shell tool (RFC-0013).
 *
 * Creates, inspects, fills, and queries Tensor Objects from ansh.
 *
 * USAGE
 *   tensor create <ns:path> <shape> <dtype>
 *   tensor info <ns:path>
 *   tensor stats <ns:path>
 *   tensor fill <ns:path> <pattern>
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/tensor.h>
#include <anx/tensor_ops.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

/* Parse "namespace:/path" into separate ns and path strings.
 * Uses two alternating static buffers so two calls can coexist. */
static void parse_ns_path(const char *arg, const char **ns_out,
			    const char **path_out)
{
	static char ns_bufs[2][64];
	static uint32_t ns_idx;
	char *ns_buf = ns_bufs[ns_idx & 1];
	const char *colon = arg;

	ns_idx++;

	while (*colon && *colon != ':')
		colon++;

	if (*colon == ':') {
		uint32_t ns_len = (uint32_t)(colon - arg);

		if (ns_len < 64) {
			anx_memcpy(ns_buf, arg, ns_len);
			ns_buf[ns_len] = '\0';
			*ns_out = ns_buf;
		}
		*path_out = colon + 1;
	} else {
		*ns_out = "default";
		*path_out = arg;
	}
}

/* Parse a dtype name string to enum value */
static int parse_dtype(const char *name, enum anx_tensor_dtype *out)
{
	if (anx_strcmp(name, "float16") == 0)
		*out = ANX_DTYPE_FLOAT16;
	else if (anx_strcmp(name, "bfloat16") == 0)
		*out = ANX_DTYPE_BFLOAT16;
	else if (anx_strcmp(name, "float32") == 0)
		*out = ANX_DTYPE_FLOAT32;
	else if (anx_strcmp(name, "float64") == 0)
		*out = ANX_DTYPE_FLOAT64;
	else if (anx_strcmp(name, "int8") == 0)
		*out = ANX_DTYPE_INT8;
	else if (anx_strcmp(name, "uint8") == 0)
		*out = ANX_DTYPE_UINT8;
	else if (anx_strcmp(name, "int4") == 0)
		*out = ANX_DTYPE_INT4;
	else if (anx_strcmp(name, "int32") == 0)
		*out = ANX_DTYPE_INT32;
	else if (anx_strcmp(name, "bool") == 0)
		*out = ANX_DTYPE_BOOL;
	else
		return ANX_EINVAL;
	return ANX_OK;
}

/*
 * Parse comma-separated shape string "4096,4096" into dimensions.
 * Returns number of dimensions parsed, or 0 on error.
 */
static uint32_t parse_shape(const char *str, uint64_t *shape, uint32_t max_dims)
{
	uint32_t ndim = 0;
	const char *p = str;

	while (*p && ndim < max_dims) {
		uint64_t val = 0;

		if (*p < '0' || *p > '9')
			return 0;
		while (*p >= '0' && *p <= '9') {
			val = val * 10 + (uint64_t)(*p - '0');
			p++;
		}
		if (val == 0)
			return 0;
		shape[ndim++] = val;
		if (*p == ',')
			p++;
		else if (*p != '\0')
			return 0;
	}
	return ndim;
}

/* Print a softfloat value as fixed-point decimal */
static void print_sf(uint32_t bits)
{
	int64_t integer;
	uint32_t frac;
	uint32_t abs_bits = bits & 0x7FFFFFFFU;

	if (abs_bits == 0) {
		kprintf("0.000");
		return;
	}

	/* Sign */
	if (bits & 0x80000000U)
		kprintf("-");

	integer = anx_sf_to_int(abs_bits);
	if (integer < 0)
		integer = -integer;

	/* Compute fractional part: multiply remainder by 1000 */
	{
		uint32_t int_back = anx_sf_from_int(integer);
		uint32_t remainder = anx_sf_add(abs_bits,
					int_back ^ 0x80000000U);
		uint32_t sf_1000 = anx_sf_from_int(1000);
		uint32_t frac_sf = anx_sf_mul(remainder, sf_1000);

		frac = (uint32_t)anx_sf_to_int(frac_sf);
		if (frac > 999)
			frac = 999;
	}

	kprintf("%u.%03u", (unsigned)integer, (unsigned)frac);
}

static void cmd_tensor_create(int argc, char **argv)
{
	const char *ns_name, *path;
	struct anx_tensor_meta meta;
	struct anx_state_object *obj;
	char oid_str[37];
	int ret;

	if (argc < 5) {
		kprintf("usage: tensor create <ns:path> <shape> <dtype>\n");
		kprintf("  shape: comma-separated dims (e.g. 4096,4096)\n");
		kprintf("  dtype: int8 uint8 int32 float32 float16 bfloat16\n");
		return;
	}

	parse_ns_path(argv[2], &ns_name, &path);

	anx_memset(&meta, 0, sizeof(meta));

	meta.ndim = parse_shape(argv[3], meta.shape, ANX_TENSOR_MAX_DIMS);
	if (meta.ndim == 0) {
		kprintf("error: invalid shape '%s'\n", argv[3]);
		return;
	}

	if (parse_dtype(argv[4], &meta.dtype) != ANX_OK) {
		kprintf("error: unknown dtype '%s'\n", argv[4]);
		return;
	}

	ret = anx_tensor_create(&meta, NULL, 0, &obj);
	if (ret != ANX_OK) {
		kprintf("error: tensor create failed (%d)\n", ret);
		return;
	}

	/* Bind to namespace */
	ret = anx_ns_bind(ns_name, path, &obj->oid);
	if (ret != ANX_OK) {
		kprintf("warning: bind to %s:%s failed (%d)\n",
			ns_name, path, ret);
	}

	anx_uuid_to_string(&obj->oid, oid_str, sizeof(oid_str));

	/* Compute total size for display */
	{
		struct anx_tensor_meta final;
		uint64_t bytes;

		anx_tensor_meta_get(&obj->oid, &final);
		bytes = final.byte_size;

		kprintf("created tensor [");
		{
			uint32_t d;

			for (d = 0; d < final.ndim; d++) {
				kprintf("%u", (unsigned)final.shape[d]);
				if (d + 1 < final.ndim)
					kprintf(", ");
			}
		}
		kprintf("] %s", anx_tensor_dtype_name(final.dtype));

		if (bytes >= 1024 * 1024)
			kprintf(" (%u MiB)", (unsigned)(bytes / (1024 * 1024)));
		else if (bytes >= 1024)
			kprintf(" (%u KiB)", (unsigned)(bytes / 1024));
		else
			kprintf(" (%u B)", (unsigned)bytes);

		kprintf("\n");
	}

	anx_objstore_release(obj);
}

static void cmd_tensor_info(int argc, char **argv)
{
	const char *ns_name, *path;
	anx_oid_t oid;
	struct anx_tensor_meta meta;
	int ret;

	if (argc < 3) {
		kprintf("usage: tensor info <ns:path>\n");
		return;
	}

	parse_ns_path(argv[2], &ns_name, &path);

	ret = anx_ns_resolve(ns_name, path, &oid);
	if (ret != ANX_OK) {
		kprintf("error: '%s:%s' not found\n", ns_name, path);
		return;
	}

	ret = anx_tensor_meta_get(&oid, &meta);
	if (ret != ANX_OK) {
		kprintf("error: not a tensor object (%d)\n", ret);
		return;
	}

	kprintf("  shape: [");
	{
		uint32_t d;

		for (d = 0; d < meta.ndim; d++) {
			kprintf("%u", (unsigned)meta.shape[d]);
			if (d + 1 < meta.ndim)
				kprintf(", ");
		}
	}
	kprintf("]\n");
	kprintf("  dtype: %s\n", anx_tensor_dtype_name(meta.dtype));
	kprintf("  elements: %u\n", (unsigned)meta.elem_count);

	if (meta.byte_size >= 1024 * 1024)
		kprintf("  size: %u MiB\n",
			(unsigned)(meta.byte_size / (1024 * 1024)));
	else if (meta.byte_size >= 1024)
		kprintf("  size: %u KiB\n",
			(unsigned)(meta.byte_size / 1024));
	else
		kprintf("  size: %u B\n", (unsigned)meta.byte_size);
}

static void cmd_tensor_stats(int argc, char **argv)
{
	const char *ns_name, *path;
	anx_oid_t oid;
	struct anx_tensor_meta meta;
	int ret;

	if (argc < 3) {
		kprintf("usage: tensor stats <ns:path>\n");
		return;
	}

	parse_ns_path(argv[2], &ns_name, &path);

	ret = anx_ns_resolve(ns_name, path, &oid);
	if (ret != ANX_OK) {
		kprintf("error: '%s:%s' not found\n", ns_name, path);
		return;
	}

	/* Seal to compute BRIN stats, then read metadata */
	anx_tensor_seal(&oid);

	ret = anx_tensor_meta_get(&oid, &meta);
	if (ret != ANX_OK) {
		kprintf("error: not a tensor object (%d)\n", ret);
		return;
	}

	kprintf("  shape: [");
	{
		uint32_t d;

		for (d = 0; d < meta.ndim; d++) {
			kprintf("%u", (unsigned)meta.shape[d]);
			if (d + 1 < meta.ndim)
				kprintf(", ");
		}
	}
	kprintf("], dtype: %s\n", anx_tensor_dtype_name(meta.dtype));

	kprintf("  mean:     "); print_sf(meta.stat_mean_bits); kprintf("\n");
	kprintf("  variance: "); print_sf(meta.stat_variance_bits); kprintf("\n");
	kprintf("  l2_norm:  "); print_sf(meta.stat_l2_norm_bits); kprintf("\n");
	kprintf("  sparsity: "); print_sf(meta.stat_sparsity_bits); kprintf("\n");
	kprintf("  min:      "); print_sf(meta.stat_min_bits); kprintf("\n");
	kprintf("  max:      "); print_sf(meta.stat_max_bits); kprintf("\n");
}

static void cmd_tensor_fill(int argc, char **argv)
{
	const char *ns_name, *path;
	anx_oid_t oid;
	int ret;

	if (argc < 4) {
		kprintf("usage: tensor fill <ns:path> <pattern>\n");
		kprintf("  patterns: zeros, ones, range\n");
		return;
	}

	parse_ns_path(argv[2], &ns_name, &path);

	ret = anx_ns_resolve(ns_name, path, &oid);
	if (ret != ANX_OK) {
		kprintf("error: '%s:%s' not found\n", ns_name, path);
		return;
	}

	ret = anx_tensor_fill(&oid, argv[3]);
	if (ret != ANX_OK) {
		kprintf("error: fill failed (%d)\n", ret);
		return;
	}

	kprintf("filled %s:%s with '%s'\n", ns_name, path, argv[3]);
}

static void cmd_tensor_slice(int argc, char **argv)
{
	const char *ns_name, *path;
	anx_oid_t src_oid;
	struct anx_state_object *result;
	struct anx_tensor_meta meta;
	uint64_t start, end;
	int ret;

	if (argc < 5) {
		kprintf("usage: tensor slice <ns:path> <start> <end>\n");
		return;
	}

	parse_ns_path(argv[2], &ns_name, &path);

	ret = anx_ns_resolve(ns_name, path, &src_oid);
	if (ret != ANX_OK) {
		kprintf("error: '%s:%s' not found\n", ns_name, path);
		return;
	}

	/* Parse start/end indices */
	{
		const char *p;

		start = 0;
		p = argv[3];
		while (*p >= '0' && *p <= '9') {
			start = start * 10 + (uint64_t)(*p - '0');
			p++;
		}

		end = 0;
		p = argv[4];
		while (*p >= '0' && *p <= '9') {
			end = end * 10 + (uint64_t)(*p - '0');
			p++;
		}
	}

	ret = anx_tensor_slice(&src_oid, start, end, &result);
	if (ret != ANX_OK) {
		kprintf("error: slice failed (%d)\n", ret);
		return;
	}

	/* Bind result next to source with _slice suffix */
	{
		char dst_path[256];
		uint32_t plen = (uint32_t)anx_strlen(path);

		if (plen + 7 < sizeof(dst_path)) {
			anx_memcpy(dst_path, path, plen);
			anx_memcpy(dst_path + plen, "_slice", 7);
			anx_ns_bind(ns_name, dst_path, &result->oid);
		}
	}

	anx_tensor_meta_get(&result->oid, &meta);
	kprintf("sliced [%u:%u] -> [", (unsigned)start, (unsigned)end);
	{
		uint32_t d;

		for (d = 0; d < meta.ndim; d++) {
			kprintf("%u", (unsigned)meta.shape[d]);
			if (d + 1 < meta.ndim)
				kprintf(", ");
		}
	}
	kprintf("] %s (%u B)\n", anx_tensor_dtype_name(meta.dtype),
		(unsigned)meta.byte_size);

	anx_objstore_release(result);
}

static void cmd_tensor_diff(int argc, char **argv)
{
	const char *ns_a, *path_a, *ns_b, *path_b;
	anx_oid_t oid_a, oid_b;
	struct anx_state_object *result;
	struct anx_tensor_meta meta;
	int ret;

	if (argc < 4) {
		kprintf("usage: tensor diff <ns:path-a> <ns:path-b>\n");
		return;
	}

	parse_ns_path(argv[2], &ns_a, &path_a);

	ret = anx_ns_resolve(ns_a, path_a, &oid_a);
	if (ret != ANX_OK) {
		kprintf("error: '%s:%s' not found\n", ns_a, path_a);
		return;
	}

	parse_ns_path(argv[3], &ns_b, &path_b);

	ret = anx_ns_resolve(ns_b, path_b, &oid_b);
	if (ret != ANX_OK) {
		kprintf("error: '%s:%s' not found\n", ns_b, path_b);
		return;
	}

	ret = anx_tensor_diff(&oid_a, &oid_b, &result);
	if (ret != ANX_OK) {
		kprintf("error: diff failed (%d)\n", ret);
		return;
	}

	/* Bind result */
	{
		char dst_path[256];
		uint32_t plen = (uint32_t)anx_strlen(path_a);

		if (plen + 6 < sizeof(dst_path)) {
			anx_memcpy(dst_path, path_a, plen);
			anx_memcpy(dst_path + plen, "_diff", 6);
			anx_ns_bind(ns_a, dst_path, &result->oid);
		}
	}

	anx_tensor_meta_get(&result->oid, &meta);
	kprintf("diff -> [");
	{
		uint32_t d;

		for (d = 0; d < meta.ndim; d++) {
			kprintf("%u", (unsigned)meta.shape[d]);
			if (d + 1 < meta.ndim)
				kprintf(", ");
		}
	}
	kprintf("] %s (delta)\n", anx_tensor_dtype_name(meta.dtype));

	anx_objstore_release(result);
}

static void cmd_tensor_quantize(int argc, char **argv)
{
	const char *ns_name, *path;
	anx_oid_t src_oid;
	enum anx_tensor_dtype target_dtype;
	struct anx_state_object *result;
	struct anx_tensor_meta src_meta, dst_meta;
	int ret;

	if (argc < 4) {
		kprintf("usage: tensor quantize <ns:path> <target-dtype>\n");
		return;
	}

	parse_ns_path(argv[2], &ns_name, &path);

	ret = anx_ns_resolve(ns_name, path, &src_oid);
	if (ret != ANX_OK) {
		kprintf("error: '%s:%s' not found\n", ns_name, path);
		return;
	}

	if (parse_dtype(argv[3], &target_dtype) != ANX_OK) {
		kprintf("error: unknown dtype '%s'\n", argv[3]);
		return;
	}

	anx_tensor_meta_get(&src_oid, &src_meta);

	ret = anx_tensor_quantize(&src_oid, target_dtype, &result);
	if (ret != ANX_OK) {
		kprintf("error: quantize failed (%d)\n", ret);
		return;
	}

	/* Bind result */
	{
		char dst_path[256];
		uint32_t plen = (uint32_t)anx_strlen(path);

		if (plen + 5 < sizeof(dst_path)) {
			anx_memcpy(dst_path, path, plen);
			anx_memcpy(dst_path + plen, "_q", 3);
			anx_ns_bind(ns_name, dst_path, &result->oid);
		}
	}

	anx_tensor_meta_get(&result->oid, &dst_meta);
	kprintf("quantized %s -> %s (%u B -> %u B)\n",
		anx_tensor_dtype_name(src_meta.dtype),
		anx_tensor_dtype_name(dst_meta.dtype),
		(unsigned)src_meta.byte_size,
		(unsigned)dst_meta.byte_size);

	anx_objstore_release(result);
}

static void cmd_tensor_search(int argc, char **argv)
{
	anx_oid_t results[32];
	uint32_t count = 0;
	int ret;

	if (argc < 3) {
		kprintf("usage: tensor search <predicate>\n");
		kprintf("  e.g. tensor search sparsity>0.5\n");
		kprintf("  e.g. tensor search dtype==int8\n");
		return;
	}

	ret = anx_tensor_search(argv[2], results, 32, &count);
	if (ret != ANX_OK) {
		kprintf("error: search failed (%d)\n", ret);
		return;
	}

	if (count == 0) {
		kprintf("no matching tensors found\n");
		return;
	}

	kprintf("found %u tensor(s):\n", (unsigned)count);
	{
		uint32_t i;

		for (i = 0; i < count; i++) {
			struct anx_tensor_meta meta;
			char oid_str[37];

			anx_uuid_to_string(&results[i], oid_str,
					    sizeof(oid_str));

			if (anx_tensor_meta_get(&results[i], &meta) == ANX_OK) {
				kprintf("  %.8s  [", oid_str);
				{
					uint32_t d;

					for (d = 0; d < meta.ndim; d++) {
						kprintf("%u",
							(unsigned)meta.shape[d]);
						if (d + 1 < meta.ndim)
							kprintf(",");
					}
				}
				kprintf("] %s\n",
					anx_tensor_dtype_name(meta.dtype));
			} else {
				kprintf("  %.8s\n", oid_str);
			}
		}
	}
}

static void cmd_tensor_matmul(int argc, char **argv)
{
	const char *ns_a, *path_a, *ns_b, *path_b, *ns_c, *path_c;
	anx_oid_t oid_a, oid_b;
	struct anx_tensor_op_request req;
	struct anx_tensor_op_result result;
	struct anx_tensor_meta meta;
	int ret;

	if (argc < 5) {
		kprintf("usage: tensor matmul <a> <b> <out>\n");
		return;
	}

	parse_ns_path(argv[2], &ns_a, &path_a);
	ret = anx_ns_resolve(ns_a, path_a, &oid_a);
	if (ret != ANX_OK) {
		kprintf("error: '%s' not found\n", argv[2]);
		return;
	}

	parse_ns_path(argv[3], &ns_b, &path_b);
	ret = anx_ns_resolve(ns_b, path_b, &oid_b);
	if (ret != ANX_OK) {
		kprintf("error: '%s' not found\n", argv[3]);
		return;
	}

	anx_memset(&req, 0, sizeof(req));
	req.op = ANX_OP_MATMUL;
	req.inputs[0] = &oid_a;
	req.inputs[1] = &oid_b;
	req.input_count = 2;

	ret = anx_tensor_op_execute(&req, &result);
	if (ret != ANX_OK) {
		kprintf("error: matmul failed (%d)\n", ret);
		return;
	}

	/* Bind result */
	parse_ns_path(argv[4], &ns_c, &path_c);
	anx_ns_bind(ns_c, path_c, &result.output_oid);

	anx_tensor_meta_get(&result.output_oid, &meta);
	kprintf("matmul -> [");
	{
		uint32_t d;

		for (d = 0; d < meta.ndim; d++) {
			kprintf("%u", (unsigned)meta.shape[d]);
			if (d + 1 < meta.ndim)
				kprintf(",");
		}
	}
	kprintf("] %s (%u cycles)\n",
		anx_tensor_dtype_name(meta.dtype), (unsigned)result.cycles);
}

static void cmd_tensor_op(int argc, char **argv, enum anx_tensor_op op)
{
	const char *ns_a, *path_a;
	anx_oid_t oid_a;
	struct anx_tensor_op_request req;
	struct anx_tensor_op_result result;
	int ret;

	if (argc < 3) {
		kprintf("usage: tensor %s <input> [output]\n",
			anx_tensor_op_name(op));
		return;
	}

	parse_ns_path(argv[2], &ns_a, &path_a);
	ret = anx_ns_resolve(ns_a, path_a, &oid_a);
	if (ret != ANX_OK) {
		kprintf("error: '%s' not found\n", argv[2]);
		return;
	}

	anx_memset(&req, 0, sizeof(req));
	req.op = op;
	req.inputs[0] = &oid_a;
	req.input_count = 1;

	ret = anx_tensor_op_execute(&req, &result);
	if (ret != ANX_OK) {
		kprintf("error: %s failed (%d)\n",
			anx_tensor_op_name(op), ret);
		return;
	}

	/* Bind if output path given */
	if (argc >= 4) {
		const char *ns_o, *path_o;

		parse_ns_path(argv[3], &ns_o, &path_o);
		anx_ns_bind(ns_o, path_o, &result.output_oid);
	}

	kprintf("%s (%u cycles)\n", anx_tensor_op_name(op),
		(unsigned)result.cycles);
}

void cmd_tensor(int argc, char **argv)
{
	if (argc < 2) {
		kprintf("usage: tensor <create|info|stats|fill|slice|diff|"
			"quantize|search|matmul|relu|transpose> [args]\n");
		return;
	}

	if (anx_strcmp(argv[1], "create") == 0)
		cmd_tensor_create(argc, argv);
	else if (anx_strcmp(argv[1], "info") == 0)
		cmd_tensor_info(argc, argv);
	else if (anx_strcmp(argv[1], "stats") == 0)
		cmd_tensor_stats(argc, argv);
	else if (anx_strcmp(argv[1], "fill") == 0)
		cmd_tensor_fill(argc, argv);
	else if (anx_strcmp(argv[1], "slice") == 0)
		cmd_tensor_slice(argc, argv);
	else if (anx_strcmp(argv[1], "diff") == 0)
		cmd_tensor_diff(argc, argv);
	else if (anx_strcmp(argv[1], "quantize") == 0)
		cmd_tensor_quantize(argc, argv);
	else if (anx_strcmp(argv[1], "search") == 0)
		cmd_tensor_search(argc, argv);
	else if (anx_strcmp(argv[1], "matmul") == 0)
		cmd_tensor_matmul(argc, argv);
	else if (anx_strcmp(argv[1], "relu") == 0)
		cmd_tensor_op(argc, argv, ANX_OP_RELU);
	else if (anx_strcmp(argv[1], "softmax") == 0)
		cmd_tensor_op(argc, argv, ANX_OP_SOFTMAX);
	else if (anx_strcmp(argv[1], "transpose") == 0)
		cmd_tensor_op(argc, argv, ANX_OP_TRANSPOSE);
	else
		kprintf("unknown tensor command: %s\n", argv[1]);
}
