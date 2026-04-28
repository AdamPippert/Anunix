/*
 * cp.c — Copy a State Object.
 *
 * Creates a new State Object with a new OID, copies the payload,
 * and records derivation provenance (parent_oids links to source).
 *
 * USAGE
 *   cp <src-path> <dst-path>
 *   cp default:/hello default:/hello-copy
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

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

void cmd_cp(int argc, char **argv)
{
	const char *src_ns, *src_path, *dst_ns, *dst_path;
	anx_oid_t src_oid;
	struct anx_state_object *src_obj;
	struct anx_so_create_params params;
	struct anx_state_object *new_obj;
	char oid_str[37];
	int ret;

	if (argc < 3) {
		kprintf("usage: cp <src-path> <dst-path>\n");
		return;
	}

	parse_ns_path(argv[1], &src_ns, &src_path);
	parse_ns_path(argv[2], &dst_ns, &dst_path);

	/* Resolve source */
	ret = anx_ns_resolve(src_ns, src_path, &src_oid);
	if (ret != ANX_OK) {
		kprintf("cp: source '%s' not found\n", argv[1]);
		return;
	}

	src_obj = anx_objstore_lookup(&src_oid);
	if (!src_obj) {
		kprintf("cp: source object not in store\n");
		return;
	}

	/* Create new object with same type and payload */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = src_obj->object_type;
	params.payload = src_obj->payload;
	params.payload_size = src_obj->payload_size;
	params.parent_oids = &src_oid;
	params.parent_count = 1;

	ret = anx_so_create(&params, &new_obj);
	anx_objstore_release(src_obj);

	if (ret != ANX_OK) {
		kprintf("cp: create failed (%d)\n", ret);
		return;
	}

	/* Bind to destination path and persist */
	ret = anx_ns_bind(dst_ns, dst_path, &new_obj->oid);
	if (ret != ANX_OK)
		kprintf("cp: bind failed (%d)\n", ret);
	else
		uobj_record(dst_ns, dst_path,
			    new_obj->payload, new_obj->payload_size);

	anx_uuid_to_string(&new_obj->oid, oid_str, sizeof(oid_str));
	kprintf("copied -> %s (%s:%s)\n", oid_str, dst_ns, dst_path);
	anx_objstore_release(new_obj);
}
