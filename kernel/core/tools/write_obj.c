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
#include <anx/objstore_disk.h>
#include <anx/alloc.h>

/* --- User namespace journal (persists write objects across reboots) --- */

#define UOBJ_DISK_OID_HI  0x554F424A4E535331ULL  /* "UOBJNSS1" */
#define UOBJ_DISK_OID_LO  0x0000000000000001ULL
#define UOBJ_DISK_MAGIC   0x554F424AU             /* "UOBJ"     */
#define UOBJ_DISK_TYPE    0xFE4D5E00U
#define UOBJ_MAX_ENTRIES  64
#define UOBJ_MAX_PAYLOAD  2048

struct uobj_entry {
	char     ns[64];
	char     path[192];
	uint32_t payload_len;
	uint8_t  payload[UOBJ_MAX_PAYLOAD];
};

struct uobj_disk {
	uint32_t magic;
	uint32_t count;
	uint32_t _pad[2];
	struct uobj_entry entries[UOBJ_MAX_ENTRIES];
};

/* In-memory table (heap-allocated on first use) */
static struct uobj_disk *g_uobj_table;

static struct uobj_disk *uobj_table(void)
{
	if (!g_uobj_table) {
		g_uobj_table = anx_alloc(sizeof(struct uobj_disk));
		if (g_uobj_table) {
			g_uobj_table->magic  = UOBJ_DISK_MAGIC;
			g_uobj_table->count  = 0;
			g_uobj_table->_pad[0] = 0;
			g_uobj_table->_pad[1] = 0;
		}
	}
	return g_uobj_table;
}

static void uobj_save(void)
{
	anx_oid_t oid;
	struct uobj_disk *t = uobj_table();

	if (!t)
		return;

	oid.hi = UOBJ_DISK_OID_HI;
	oid.lo = UOBJ_DISK_OID_LO;
	anx_disk_delete_obj(&oid);
	anx_disk_write_obj(&oid, UOBJ_DISK_TYPE, t, sizeof(*t));
}

void uobj_record(const char *ns, const char *path,
		 const void *payload, uint32_t payload_len)
{
	struct uobj_disk *t = uobj_table();
	struct uobj_entry *e;
	uint32_t i;

	if (!t)
		return;

	/* Replace existing entry for the same path */
	for (i = 0; i < t->count; i++) {
		if (anx_strcmp(t->entries[i].ns, ns) == 0 &&
		    anx_strcmp(t->entries[i].path, path) == 0) {
			e = &t->entries[i];
			goto fill;
		}
	}

	/* New entry */
	if (t->count >= UOBJ_MAX_ENTRIES)
		return;
	e = &t->entries[t->count++];

fill:
	anx_strlcpy(e->ns, ns, sizeof(e->ns));
	anx_strlcpy(e->path, path, sizeof(e->path));
	e->payload_len = payload_len < UOBJ_MAX_PAYLOAD
			 ? payload_len : UOBJ_MAX_PAYLOAD;
	anx_memcpy(e->payload, payload, e->payload_len);
	uobj_save();
}

void uobj_remove(const char *ns, const char *path)
{
	struct uobj_disk *t = uobj_table();
	uint32_t i;

	if (!t)
		return;

	for (i = 0; i < t->count; i++) {
		if (anx_strcmp(t->entries[i].ns, ns) == 0 &&
		    anx_strcmp(t->entries[i].path, path) == 0) {
			/* Swap with last and shrink */
			t->entries[i] = t->entries[--t->count];
			uobj_save();
			return;
		}
	}
}

void anx_uobj_load(void)
{
	struct uobj_disk *t = uobj_table();
	anx_oid_t oid;
	uint32_t actual, obj_type;
	uint32_t i;
	int rc;

	if (!t)
		return;

	oid.hi = UOBJ_DISK_OID_HI;
	oid.lo = UOBJ_DISK_OID_LO;
	rc = anx_disk_read_obj(&oid, t, sizeof(*t), &actual, &obj_type);
	if (rc != ANX_OK || t->magic != UOBJ_DISK_MAGIC)
		return;

	for (i = 0; i < t->count; i++) {
		struct uobj_entry *e = &t->entries[i];
		struct anx_so_create_params params;
		struct anx_state_object *obj;

		anx_memset(&params, 0, sizeof(params));
		params.object_type = ANX_OBJ_BYTE_DATA;
		params.payload     = e->payload;
		params.payload_size = e->payload_len;

		if (anx_so_create(&params, &obj) == ANX_OK) {
			anx_ns_bind(e->ns, e->path, &obj->oid);
			anx_objstore_release(obj);
		}
	}

	kprintf("write: restored %u object(s) from disk\n", t->count);
}

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

	uobj_record(ns_name, path, content, content_len);

	anx_uuid_to_string(&obj->oid, oid_str, sizeof(oid_str));
	kprintf("created %s (%u bytes) -> %s:%s\n",
		oid_str, content_len, ns_name, path);

	anx_objstore_release(obj);
}
