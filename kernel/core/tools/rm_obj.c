/*
 * rm_obj.c — Delete a State Object.
 *
 * Transitions an object to DELETED state and removes its
 * namespace binding. Respects retention policies and refcount.
 *
 * USAGE
 *   rm <namespace:path>       Delete by namespace path
 *   rm -f <namespace:path>    Force delete (tombstone)
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>
#include <anx/string.h>

void cmd_rm_obj(int argc, char **argv)
{
	const char *ns_name = "default";
	const char *path = NULL;
	bool force = false;
	anx_oid_t oid;
	int i, ret;

	for (i = 1; i < argc; i++) {
		if (anx_strcmp(argv[i], "-f") == 0) {
			force = true;
		} else {
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
		}
	}

	if (!path) {
		kprintf("usage: rm [-f] <ns:path>\n");
		return;
	}

	/* Resolve path to OID */
	ret = anx_ns_resolve(ns_name, path, &oid);
	if (ret != ANX_OK) {
		kprintf("rm: '%s:%s' not found\n", ns_name, path);
		return;
	}

	/* Delete the object */
	ret = anx_so_delete(&oid, force);
	if (ret != ANX_OK) {
		kprintf("rm: delete failed (%d)\n", ret);
		return;
	}

	/* Remove namespace binding */
	anx_ns_unbind(ns_name, path);
	kprintf("deleted %s:%s\n", ns_name, path);
}
