/*
 * ls.c — List State Objects in a namespace.
 *
 * Lists namespace bindings, showing OID, object type, lifecycle
 * state, version, and payload size. Operates on State Objects
 * rather than file system entries.
 *
 * USAGE
 *   ls [namespace:]path
 *   ls                    List root of 'default' namespace
 *   ls posix:/home        List entries under /home in posix namespace
 *   ls -l                 Long format with details
 *   ls -a                 List all namespaces
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

static const char *obj_type_str(enum anx_object_type t)
{
	switch (t) {
	case ANX_OBJ_BYTE_DATA:		return "byte";
	case ANX_OBJ_STRUCTURED_DATA:	return "struct";
	case ANX_OBJ_EMBEDDING:		return "embed";
	case ANX_OBJ_GRAPH_NODE:	return "graph";
	case ANX_OBJ_MODEL_OUTPUT:	return "model";
	case ANX_OBJ_EXECUTION_TRACE:	return "trace";
	case ANX_OBJ_CAPABILITY:	return "cap";
	case ANX_OBJ_CREDENTIAL:	return "cred";
	default:			return "?";
	}
}

static const char *obj_state_str(enum anx_object_state s)
{
	switch (s) {
	case ANX_OBJ_CREATING:	return "creating";
	case ANX_OBJ_ACTIVE:	return "active";
	case ANX_OBJ_SEALED:	return "sealed";
	case ANX_OBJ_EXPIRED:	return "expired";
	case ANX_OBJ_DELETED:	return "deleted";
	case ANX_OBJ_TOMBSTONE:	return "tomb";
	default:		return "?";
	}
}

void cmd_ls(int argc, char **argv)
{
	const char *ns_name = "default";
	const char *path = NULL;
	bool long_format = false;
	bool list_ns = false;
	struct anx_ns_list_entry entries[16];
	uint32_t count = 0;
	uint32_t i;
	int ret;

	/* Parse flags */
	for (i = 1; i < (uint32_t)argc; i++) {
		if (anx_strcmp(argv[i], "-l") == 0) {
			long_format = true;
		} else if (anx_strcmp(argv[i], "-a") == 0) {
			list_ns = true;
		} else {
			/* Parse namespace:path */
			const char *colon = argv[i];

			while (*colon && *colon != ':')
				colon++;
			if (*colon == ':') {
				/* Has namespace prefix */
				uint32_t ns_len = (uint32_t)(colon - argv[i]);
				static char ns_buf[64];

				if (ns_len < sizeof(ns_buf)) {
					anx_memcpy(ns_buf, argv[i], ns_len);
					ns_buf[ns_len] = '\0';
					ns_name = ns_buf;
				}
				path = colon + 1;
			} else {
				path = argv[i];
			}
		}
	}

	/* List all namespaces */
	if (list_ns) {
		char names[16][ANX_NS_NAME_MAX];
		uint32_t ns_count = 0;

		anx_ns_list_namespaces(names, 16, &ns_count);
		kprintf("namespaces (%u):\n", ns_count);
		for (i = 0; i < ns_count; i++)
			kprintf("  %s\n", names[i]);
		return;
	}

	/* List entries */
	ret = anx_ns_list(ns_name, path, entries, 16, &count);
	if (ret != ANX_OK && ret != ANX_ENOENT) {
		kprintf("ls: error (%d)\n", ret);
		return;
	}

	if (count == 0) {
		kprintf("(empty)\n");
		return;
	}

	for (i = 0; i < count; i++) {
		if (long_format) {
			char oid_str[37];
			struct anx_state_object *obj;

			anx_uuid_to_string(&entries[i].oid, oid_str,
					   sizeof(oid_str));
			obj = anx_objstore_lookup(&entries[i].oid);

			if (obj) {
				kprintf("  %s  %s  %s  v%u  %u bytes  %s\n",
					oid_str,
					obj_type_str(obj->object_type),
					obj_state_str(obj->state),
					(uint32_t)obj->version,
					(uint32_t)obj->payload_size,
					entries[i].name);
				anx_objstore_release(obj);
			} else {
				kprintf("  %s  %s%s\n",
					oid_str,
					entries[i].is_directory ? "[dir] " : "",
					entries[i].name);
			}
		} else {
			kprintf("  %s%s\n",
				entries[i].is_directory ? "[dir] " : "",
				entries[i].name);
		}
	}
}
