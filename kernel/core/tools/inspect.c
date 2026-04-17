/*
 * inspect.c — Full State Object inspector.
 *
 * Displays complete object internals: OID, content hash, version,
 * type, state, payload size, refcount. Hex dump of payload with
 * ASCII sidebar.
 *
 * USAGE
 *   inspect <oid-or-path>     Full inspection
 *   inspect -x <oid-or-path>  Hex dump only
 *   inspect -m <oid-or-path>  Metadata only
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/namespace.h>
#include <anx/state_object.h>
#include <anx/kprintf.h>
#include <anx/string.h>
#include <anx/uuid.h>

static const char *type_name(enum anx_object_type t)
{
	switch (t) {
	case ANX_OBJ_BYTE_DATA:		return "byte_data";
	case ANX_OBJ_STRUCTURED_DATA:	return "structured_data";
	case ANX_OBJ_EMBEDDING:		return "embedding";
	case ANX_OBJ_GRAPH_NODE:	return "graph_node";
	case ANX_OBJ_MODEL_OUTPUT:	return "model_output";
	case ANX_OBJ_EXECUTION_TRACE:	return "execution_trace";
	case ANX_OBJ_CAPABILITY:	return "capability";
	case ANX_OBJ_CREDENTIAL:	return "credential";
	default:			return "unknown";
	}
}

static const char *state_name(enum anx_object_state s)
{
	switch (s) {
	case ANX_OBJ_CREATING:	return "creating";
	case ANX_OBJ_ACTIVE:	return "active";
	case ANX_OBJ_SEALED:	return "sealed";
	case ANX_OBJ_EXPIRED:	return "expired";
	case ANX_OBJ_DELETED:	return "deleted";
	case ANX_OBJ_TOMBSTONE:	return "tombstone";
	default:		return "unknown";
	}
}

static int resolve_arg(const char *arg, anx_oid_t *oid)
{
	const char *colon = arg;
	int ret;

	while (*colon && *colon != ':')
		colon++;
	if (*colon == ':') {
		char ns_buf[64];
		uint32_t ns_len = (uint32_t)(colon - arg);

		if (ns_len < sizeof(ns_buf)) {
			anx_memcpy(ns_buf, arg, ns_len);
			ns_buf[ns_len] = '\0';
			return anx_ns_resolve(ns_buf, colon + 1, oid);
		}
	}
	ret = anx_ns_resolve("default", arg, oid);
	if (ret == ANX_OK)
		return ANX_OK;
	return anx_ns_resolve("posix", arg, oid);
}

void cmd_inspect(int argc, char **argv)
{
	const char *target = NULL;
	bool hex_only = false;
	bool meta_only = false;
	anx_oid_t oid;
	struct anx_state_object *obj;
	char oid_str[37];
	int i, ret;

	for (i = 1; i < argc; i++) {
		if (anx_strcmp(argv[i], "-x") == 0)
			hex_only = true;
		else if (anx_strcmp(argv[i], "-m") == 0)
			meta_only = true;
		else
			target = argv[i];
	}

	if (!target) {
		kprintf("usage: inspect [-x|-m] <oid-or-path>\n");
		return;
	}

	ret = resolve_arg(target, &oid);
	if (ret != ANX_OK) {
		kprintf("inspect: '%s' not found\n", target);
		return;
	}

	obj = anx_objstore_lookup(&oid);
	if (!obj) {
		kprintf("inspect: object not in store\n");
		return;
	}

	anx_uuid_to_string(&obj->oid, oid_str, sizeof(oid_str));

	if (!hex_only) {
		kprintf("\n=== State Object Inspection ===\n\n");
		kprintf("  OID:       %s\n", oid_str);
		kprintf("  Type:      %s\n", type_name(obj->object_type));
		kprintf("  State:     %s\n", state_name(obj->state));
		kprintf("  Version:   %u\n", (uint32_t)obj->version);
		kprintf("  Payload:   %u bytes\n", (uint32_t)obj->payload_size);
		kprintf("  Refcount:  %u\n", obj->refcount);
		kprintf("  Parents:   %u\n", obj->parent_count);
	}

	if (!meta_only && obj->payload && obj->payload_size > 0) {
		const uint8_t *data = (const uint8_t *)obj->payload;
		uint32_t len = (uint32_t)obj->payload_size;
		uint32_t show = len > 256 ? 256 : len;
		uint32_t off, j;

		kprintf("\n  Hex dump (%u of %u bytes):\n", show, len);
		for (off = 0; off < show; off += 16) {
			kprintf("  %x: ", off);
			for (j = 0; j < 16 && off + j < show; j++)
				kprintf("%x ", (uint32_t)data[off + j]);
			for (; j < 16; j++)
				kprintf("   ");
			kprintf(" ");
			for (j = 0; j < 16 && off + j < show; j++) {
				char c = (char)data[off + j];
				kprintf("%c",
					(c >= 0x20 && c < 0x7F) ? c : '.');
			}
			kprintf("\n");
		}
		if (len > 256)
			kprintf("  ... (%u more bytes)\n", len - 256);
	}

	kprintf("\n");
	anx_objstore_release(obj);
}
