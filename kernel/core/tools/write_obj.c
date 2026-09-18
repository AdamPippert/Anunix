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
		g_uobj_table = anx_zalloc(sizeof(struct uobj_disk));
		if (g_uobj_table) {
			g_uobj_table->magic  = UOBJ_DISK_MAGIC;
			g_uobj_table->count  = 0;
			g_uobj_table->_pad[0] = 0;
			g_uobj_table->_pad[1] = 0;
		}
	}
	return g_uobj_table;
}

static int uobj_save(void)
{
	anx_oid_t oid = { UOBJ_DISK_OID_HI, UOBJ_DISK_OID_LO };
	struct uobj_disk *t = uobj_table();

	if (!t)
		return ANX_ENOMEM;
	/* The disk writer replaces an existing OID; do not delete it first. */
	return anx_disk_write_obj(&oid, UOBJ_DISK_TYPE, t, sizeof(*t));
}

int anx_uobj_record_checked(const char *ns, const char *path,
			    const void *payload, uint32_t payload_len)
{
	struct uobj_disk *t;
	struct uobj_entry previous, *e;
	uint32_t i, old_count;
	int rc;

	if (!ns || !path || (!payload && payload_len) || !*ns || !*path ||
	    anx_strlen(ns) >= sizeof(previous.ns) ||
	    anx_strlen(path) >= sizeof(previous.path) ||
	    payload_len > UOBJ_MAX_PAYLOAD)
		return ANX_EINVAL;
	/* Namespace resolution treats leading '/' as optional. */
	if (*path == '/') path++;
	if (!*path) return ANX_EINVAL;
	t = uobj_table();
	if (!t) return ANX_ENOMEM;
	old_count = t->count;
	for (i = 0; i < t->count; i++)
		if (!anx_strcmp(t->entries[i].ns, ns) &&
		    !anx_strcmp(t->entries[i].path, path))
			break;
	if (i == UOBJ_MAX_ENTRIES) return ANX_ENOMEM;
	e = &t->entries[i];
	previous = *e;
	anx_memset(e, 0, sizeof(*e));
	anx_strlcpy(e->ns, ns, sizeof(e->ns));
	anx_strlcpy(e->path, path, sizeof(e->path));
	e->payload_len = payload_len;
	if (payload_len) anx_memcpy(e->payload, payload, payload_len);
	if (i == t->count) t->count++;
	rc = uobj_save();
	if (rc != ANX_OK) {
		*e = previous;
		t->count = old_count;
	}
	return rc;
}

void uobj_record(const char *ns, const char *path,
		 const void *payload, uint32_t payload_len)
{
	int rc = anx_uobj_record_checked(ns, path, payload, payload_len);

	if (rc != ANX_OK)
		kprintf("write: object is live but was not saved (%d)\n", rc);
}

void uobj_remove(const char *ns, const char *path)
{
	struct uobj_disk *t = uobj_table();
	uint32_t i;

	if (!ns || !path) return;
	if (*path == '/') path++;

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
	if (rc != ANX_OK || actual != sizeof(*t) ||
	    obj_type != UOBJ_DISK_TYPE || t->magic != UOBJ_DISK_MAGIC ||
	    t->count > UOBJ_MAX_ENTRIES) {
		anx_memset(t, 0, sizeof(*t));
		t->magic = UOBJ_DISK_MAGIC;
		return;
	}
	/* Validate the entire journal before publishing any entry. */
	for (i = 0; i < t->count; i++) {
		struct uobj_entry *e = &t->entries[i];
		if (e->ns[sizeof(e->ns) - 1] != 0 ||
		    e->path[sizeof(e->path) - 1] != 0 ||
		    !e->ns[0] || !e->path[0] || e->payload_len > UOBJ_MAX_PAYLOAD) {
			anx_memset(t, 0, sizeof(*t));
			t->magic = UOBJ_DISK_MAGIC;
			kprintf("write: invalid object journal ignored\n");
			return;
		}
	}

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

int anx_uobj_write_at(const char *name, uint32_t offset,
		      const void *data, uint32_t len)
{
	char ns[64] = "default", path[192];
	const char *sep;
	anx_oid_t oid;
	struct anx_object_handle h;
	uint32_t n;
	int rc;

	if (!name || !data || !len) return ANX_EINVAL;
	sep = name;
	while (*sep && *sep != ':') sep++;
	if (!*sep) sep = NULL;
	if (sep) {
		n = (uint32_t)(sep - name);
		if (!n || n >= sizeof(ns)) return ANX_EINVAL;
		anx_memcpy(ns, name, n);
		ns[n] = 0;
		name = sep + 1;
	}
	if (*name == '/') name++;
	if (!*name || anx_strlen(name) >= sizeof(path)) return ANX_EINVAL;
	anx_strlcpy(path, name, sizeof(path));
	rc = anx_ns_resolve(ns, path, &oid);
	if (rc != ANX_OK) return rc;
	rc = anx_so_open(&oid, ANX_OPEN_READWRITE, &h);
	if (rc != ANX_OK) return rc;
	if (h.obj->staged) { rc = ANX_EBUSY; goto out; }
	/* Bounded overwrite preserves object length and identity. */
	if (h.obj->payload_size > UOBJ_MAX_PAYLOAD ||
	    offset > h.obj->payload_size || len > h.obj->payload_size - offset) {
		rc = ANX_EINVAL;
		goto out;
	}
	rc = anx_so_write_payload(&h, offset, data, len);
	if (rc >= 0) {
		rc = anx_uobj_record_checked(ns, path, h.obj->payload,
					     (uint32_t)h.obj->payload_size);
		if (rc != ANX_OK)
			kprintf("write: bytes changed in memory, save failed (%d)\n", rc);
	}
out:
	anx_so_close(&h);
	return rc;
}

static void write_at_command(int argc, char **argv)
{
	char content[1024];
	const char *p;
	uint32_t offset = 0, len = 0, n;
	int i, rc;

	if (argc < 5) goto usage;
	p = argv[2];
	if (!*p) goto usage;
	for (; *p; p++) {
		if (*p < '0' || *p > '9' || offset > (0xffffffffu - (*p - '0')) / 10)
			goto usage;
		offset = offset * 10 + (*p - '0');
	}
	for (i = 4; i < argc; i++) {
		n = (uint32_t)anx_strlen(argv[i]);
		if (len + n + 1 >= sizeof(content)) goto usage;
		if (i > 4) content[len++] = ' ';
		anx_memcpy(content + len, argv[i], n);
		len += n;
	}
	rc = anx_uobj_write_at(argv[3], offset, content, len);
	if (rc != ANX_OK)
		kprintf("write --at: failed (%d)\n", rc);
	else
		kprintf("write: updated %u bytes at %u in %s\n", len, offset, argv[3]);
	return;
usage:
	kprintf("usage: write --at <offset> <ns:path> <content...>\n");
}

void cmd_write_obj(int argc, char **argv)
{
	const char *ns_name = "default";
	const char *path = NULL;
	static char content[1024];
	enum anx_object_type obj_type = ANX_OBJ_BYTE_DATA;
	struct anx_so_create_params params;
	struct anx_state_object *obj;
	char oid_str[37];
	uint32_t content_len = 0;
	int i, ret;

	if (argc > 1 && !anx_strcmp(argv[1], "--at")) {
		write_at_command(argc, argv);
		return;
	}

	/* Parse arguments; everything after the path is the content */
	content[0] = '\0';
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
			uint32_t n = (uint32_t)anx_strlen(argv[i]);

			if (content_len + n + 2 > sizeof(content)) {
				kprintf("write: content longer than %u bytes\n",
					(uint32_t)sizeof(content) - 1);
				return;
			}
			if (content_len)
				content[content_len++] = ' ';
			anx_memcpy(content + content_len, argv[i], n);
			content_len += n;
			content[content_len] = '\0';
		}
	}

	if (!path || content_len == 0) {
		kprintf("usage: write [-t type] <ns:path> <content...>\n");
		return;
	}

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
		anx_objstore_release(obj);
		return;
	}

	uobj_record(ns_name, path, content, content_len);

	anx_uuid_to_string(&obj->oid, oid_str, sizeof(oid_str));
	kprintf("created %s (%u bytes) -> %s:%s\n",
		oid_str, content_len, ns_name, path);

	anx_objstore_release(obj);
}
