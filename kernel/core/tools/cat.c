/*
 * cat.c — Read State Object payload.
 *
 * Reads and displays the payload of a State Object, addressed
 * by OID prefix or namespace path.
 *
 * USAGE
 *   cat <oid-prefix>          Read by OID prefix
 *   cat <namespace:path>      Read by namespace path
 *   cat -p <oid>              Show provenance chain
 *   cat -x <oid>              Hex dump
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

static void hex_dump(const uint8_t *data, uint32_t len)
{
	uint32_t i, j;

	for (i = 0; i < len; i += 16) {
		kprintf("  %x: ", i);
		for (j = 0; j < 16 && i + j < len; j++)
			kprintf("%x ", (uint32_t)data[i + j]);
		for (; j < 16; j++)
			kprintf("   ");
		kprintf(" ");
		for (j = 0; j < 16 && i + j < len; j++) {
			char c = (char)data[i + j];

			kprintf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
		}
		kprintf("\n");
	}
}

void cmd_cat(int argc, char **argv)
{
	bool show_hex = false;
	bool show_prov = false;
	const char *target = NULL;
	anx_oid_t oid;
	struct anx_state_object *obj;
	int i, ret;

	for (i = 1; i < argc; i++) {
		if (anx_strcmp(argv[i], "-x") == 0)
			show_hex = true;
		else if (anx_strcmp(argv[i], "-p") == 0)
			show_prov = true;
		else
			target = argv[i];
	}

	if (!target) {
		kprintf("usage: cat [-x] [-p] <oid-or-path>\n");
		return;
	}

	ret = anx_so_resolve(target, &oid);
	if (ret != ANX_OK) {
		kprintf("cat: '%s' not found (%d)\n", target, ret);
		return;
	}

	obj = anx_objstore_lookup(&oid);
	if (!obj) {
		kprintf("cat: object not in store\n");
		return;
	}

	if (show_prov) {
		char oid_str[37];

		anx_uuid_to_string(&obj->oid, oid_str, sizeof(oid_str));
		kprintf("oid:     %s\n", oid_str);
		kprintf("version: %u\n", (uint32_t)obj->version);
		kprintf("size:    %u bytes\n", (uint32_t)obj->payload_size);
		kprintf("parents: %u\n", obj->parent_count);
	}

	if (obj->payload && obj->payload_size > 0) {
		if (show_hex) {
			hex_dump((const uint8_t *)obj->payload,
				 (uint32_t)obj->payload_size);
		} else {
			/* Print as text, null-terminate if needed */
			const char *p = (const char *)obj->payload;
			uint32_t len = (uint32_t)obj->payload_size;
			uint32_t j;

			for (j = 0; j < len; j++)
				kprintf("%c", p[j]);
			kprintf("\n");
		}
	} else {
		kprintf("(empty payload)\n");
	}

	anx_objstore_release(obj);
}
