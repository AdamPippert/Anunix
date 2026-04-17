/*
 * mv.c — Move/rename a namespace binding.
 *
 * Rebinds a State Object from one namespace path to another.
 * The object itself doesn't move — only the name changes.
 * The OID remains stable.
 *
 * USAGE
 *   mv <src-path> <dst-path>
 *   mv default:/old-name default:/new-name
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/namespace.h>
#include <anx/kprintf.h>
#include <anx/string.h>

static int parse_ns_path(const char *arg, const char **ns_out,
			  const char **path_out)
{
	static char ns_buf[64];
	const char *colon = arg;

	while (*colon && *colon != ':')
		colon++;
	if (*colon == ':') {
		uint32_t ns_len = (uint32_t)(colon - arg);

		if (ns_len < sizeof(ns_buf)) {
			anx_memcpy(ns_buf, arg, ns_len);
			ns_buf[ns_len] = '\0';
			*ns_out = ns_buf;
			*path_out = colon + 1;
			return ANX_OK;
		}
	}
	*ns_out = "default";
	*path_out = arg;
	return ANX_OK;
}

void cmd_mv(int argc, char **argv)
{
	const char *src_ns, *src_path, *dst_ns, *dst_path;
	anx_oid_t oid;
	int ret;

	if (argc < 3) {
		kprintf("usage: mv <src-path> <dst-path>\n");
		return;
	}

	parse_ns_path(argv[1], &src_ns, &src_path);
	parse_ns_path(argv[2], &dst_ns, &dst_path);

	/* Resolve source OID */
	ret = anx_ns_resolve(src_ns, src_path, &oid);
	if (ret != ANX_OK) {
		kprintf("mv: source '%s' not found\n", argv[1]);
		return;
	}

	/* Bind to new path */
	ret = anx_ns_bind(dst_ns, dst_path, &oid);
	if (ret != ANX_OK) {
		kprintf("mv: bind to '%s' failed (%d)\n", argv[2], ret);
		return;
	}

	/* Remove old binding */
	anx_ns_unbind(src_ns, src_path);
	kprintf("moved %s:%s -> %s:%s\n", src_ns, src_path, dst_ns, dst_path);
}
