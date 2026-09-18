/*
 * meta.c — State Object metadata editor.
 *
 * Read and write user metadata on State Objects.
 *
 * USAGE
 *   meta set <oid-or-path> <key> <val> Set user metadata
 *   meta get <oid-or-path> <key>       Get a metadata value
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/meta.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

static void show_entry(const struct anx_meta_entry *e, void *arg)
{
	uint32_t *n = arg;

	(*n)++;
	switch (e->value.type) {
	case ANX_META_STRING:
		kprintf("  %s = %s\n", e->key, e->value.v.str.data);
		break;
	case ANX_META_INT64:
		kprintf("  %s = %lld\n", e->key, (long long)e->value.v.i64);
		break;
	case ANX_META_BOOL:
		kprintf("  %s = %s\n", e->key,
			e->value.v.boolean ? "true" : "false");
		break;
	case ANX_META_BYTES:
		kprintf("  %s = <%u bytes>\n", e->key, e->value.v.bytes.len);
		break;
	default:
		kprintf("  %s = <float>\n", e->key);
		break;
	}
}

void cmd_meta(int argc, char **argv)
{
	anx_oid_t oid;
	struct anx_state_object *obj;
	int ret;

	if (argc < 3) {
		kprintf("usage: meta <show|set|get> <path> [key] [value]\n");
		return;
	}

	ret = anx_so_resolve(argv[2], &oid);
	if (ret != ANX_OK) {
		kprintf("meta: '%s' not found\n", argv[2]);
		return;
	}

	obj = anx_objstore_lookup(&oid);
	if (!obj) {
		kprintf("meta: object not in store\n");
		return;
	}

	if (anx_strcmp(argv[1], "show") == 0) {
		uint32_t n = 0;

		anx_meta_iterate(obj->user_meta, show_entry, &n);
		if (n == 0)
			kprintf("(no metadata)\n");
	} else if (anx_strcmp(argv[1], "set") == 0) {
		if (argc < 5) {
			kprintf("usage: meta set <path> <key> <value>\n");
			anx_objstore_release(obj);
			return;
		}
		if (!obj->user_meta)
			obj->user_meta = anx_meta_create();
		if (obj->user_meta) {
			ret = anx_meta_set_str(obj->user_meta,
					       argv[3], argv[4]);
			if (ret == ANX_OK)
				kprintf("set %s = %s\n", argv[3], argv[4]);
			else
				kprintf("meta: set failed (%d)\n", ret);
		}
	} else if (anx_strcmp(argv[1], "get") == 0) {
		if (argc < 4) {
			kprintf("usage: meta get <path> <key>\n");
			anx_objstore_release(obj);
			return;
		}
		if (obj->user_meta) {
			const struct anx_meta_value *val;

			val = anx_meta_get(obj->user_meta, argv[3]);
			if (val && val->type == ANX_META_STRING)
				kprintf("%s\n", val->v.str.data);
			else
				kprintf("(not set)\n");
		} else {
			kprintf("(no metadata)\n");
		}
	} else {
		kprintf("usage: meta <show|set|get> <path> [key] [value]\n");
	}

	anx_objstore_release(obj);
}
