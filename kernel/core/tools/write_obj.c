/*
 * write_obj.c — Create a State Object with text payload.
 *
 * Creates a new State Object and optionally binds it to a
 * namespace path. Records creator provenance.
 *
 * USAGE
 *   write <namespace:path> <content...>
 *   write default:/hello "Hello, world"
 *   write -t structured default:/data '{"key":"value"}'
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

void cmd_write_obj(int argc, char **argv)
{
	const char *ns_name = "default";
	const char *path = NULL;
	const char *content = NULL;
	enum anx_object_type obj_type = ANX_OBJ_BYTE_DATA;
	struct anx_so_create_params params;
	struct anx_state_object *obj;
	char oid_str[37];
	uint32_t content_len;
	int i, ret;

	/* Parse arguments */
	for (i = 1; i < argc; i++) {
		if (anx_strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
			i++;
			if (anx_strcmp(argv[i], "structured") == 0)
				obj_type = ANX_OBJ_STRUCTURED_DATA;
			else if (anx_strcmp(argv[i], "byte") == 0)
				obj_type = ANX_OBJ_BYTE_DATA;
		} else if (!path) {
			/* First non-flag arg is the path */
			const char *colon = argv[i];

			while (*colon && *colon != ':')
				colon++;
			if (*colon == ':') {
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
		} else {
			content = argv[i];
		}
	}

	if (!path || !content) {
		kprintf("usage: write [-t type] <ns:path> <content>\n");
		return;
	}

	/* Build content from remaining args (join with spaces) */
	/* For simplicity, use the single content arg */
	content_len = (uint32_t)anx_strlen(content);

	/* Create the State Object */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = obj_type;
	params.payload = content;
	params.payload_size = content_len;

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK) {
		kprintf("write: create failed (%d)\n", ret);
		return;
	}

	/* Bind to namespace */
	ret = anx_ns_bind(ns_name, path, &obj->oid);
	if (ret != ANX_OK) {
		kprintf("write: bind to %s:%s failed (%d)\n",
			ns_name, path, ret);
	}

	anx_uuid_to_string(&obj->oid, oid_str, sizeof(oid_str));
	kprintf("created %s (%u bytes) -> %s:%s\n",
		oid_str, content_len, ns_name, path);

	anx_objstore_release(obj);
}
