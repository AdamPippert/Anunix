/*
 * uor.c — ansh frontend for the UOR topological coordinate layer.
 *
 *   uor inspect <oid>             dump the uor.* metadata for an object
 *   uor scan <bk_lo> <bk_hi>      list OIDs whose boundary_key is in range
 *   uor rebuild                   recompute every projection, bump epoch
 *   uor project <oid>             reproject a single object now
 *
 * Boundary keys are accepted as decimal or 0x-prefixed hex. The scan
 * covers the full disk-store sorted index, so the BYTE_DATA bucket is
 * `uor scan 0 0xffffffffffffff` and the embedding bucket is
 * `uor scan 0x0200000000000000 0x02ffffffffffffff`, etc.
 */

#include <anx/types.h>
#include <anx/tools.h>
#include <anx/state_object.h>
#include <anx/uor.h>
#include <anx/objstore_disk.h>
#include <anx/meta.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>

static int parse_u64(const char *s, uint64_t *out)
{
	uint64_t v;

	if (!s || !out)
		return ANX_EINVAL;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		v = anx_strtoull(s + 2, NULL, 16);
	} else {
		v = anx_strtoull(s, NULL, 10);
	}
	*out = v;
	return ANX_OK;
}

static int parse_oid_hex(const char *s, anx_oid_t *out)
{
	uint8_t bytes[16];
	uint32_t i, j = 0;
	uint32_t n;

	if (!s || !out)
		return ANX_EINVAL;
	n = (uint32_t)anx_strlen(s);
	if (n != 32)
		return ANX_EINVAL;

	for (i = 0; i < 32; i += 2) {
		uint8_t hi, lo;
		char ch;

		ch = s[i];
		if (ch >= '0' && ch <= '9')      hi = (uint8_t)(ch - '0');
		else if (ch >= 'a' && ch <= 'f') hi = (uint8_t)(ch - 'a' + 10);
		else if (ch >= 'A' && ch <= 'F') hi = (uint8_t)(ch - 'A' + 10);
		else return ANX_EINVAL;

		ch = s[i + 1];
		if (ch >= '0' && ch <= '9')      lo = (uint8_t)(ch - '0');
		else if (ch >= 'a' && ch <= 'f') lo = (uint8_t)(ch - 'a' + 10);
		else if (ch >= 'A' && ch <= 'F') lo = (uint8_t)(ch - 'A' + 10);
		else return ANX_EINVAL;

		bytes[j++] = (uint8_t)((hi << 4) | lo);
	}

	out->hi = 0;
	for (i = 0; i < 8; i++)
		out->hi = (out->hi << 8) | bytes[i];
	out->lo = 0;
	for (i = 0; i < 8; i++)
		out->lo = (out->lo << 8) | bytes[8 + i];
	return ANX_OK;
}

static void print_meta_value(struct anx_meta_store *store, const char *key)
{
	const struct anx_meta_value *v = anx_meta_get(store, key);

	if (!v) {
		kprintf("%-26s (unset)\n", key);
		return;
	}
	switch (v->type) {
	case ANX_META_STRING:
		kprintf("%-26s %s\n", key, v->v.str.data);
		break;
	case ANX_META_INT64: {
		uint64_t u = (uint64_t)v->v.i64;

		kprintf("%-26s 0x%016llx (%llu)\n", key,
			(unsigned long long)u, (unsigned long long)u);
		break;
	}
	default:
		kprintf("%-26s (unsupported type)\n", key);
		break;
	}
}

static void cmd_uor_inspect(int argc, char **argv)
{
	anx_oid_t oid;
	struct anx_state_object *obj;

	if (argc < 3) {
		kprintf("usage: uor inspect <oid>\n");
		return;
	}
	if (parse_oid_hex(argv[2], &oid) != ANX_OK) {
		kprintf("uor: bad oid '%s' (need 32 hex chars)\n", argv[2]);
		return;
	}
	obj = anx_objstore_lookup(&oid);
	if (!obj) {
		kprintf("uor: object not found\n");
		return;
	}

	kprintf("oid                        %016llx%016llx\n",
		(unsigned long long)oid.hi, (unsigned long long)oid.lo);
	kprintf("object_type                %u\n", (unsigned)obj->object_type);
	kprintf("version                    %llu\n",
		(unsigned long long)obj->version);

	print_meta_value(obj->system_meta, ANX_META_UOR_ADDRESS);
	print_meta_value(obj->system_meta, ANX_META_UOR_BOUNDARY_KEY);
	print_meta_value(obj->system_meta, ANX_META_UOR_REGION_LO);
	print_meta_value(obj->system_meta, ANX_META_UOR_REGION_HI);
	print_meta_value(obj->system_meta, ANX_META_UOR_LOCALITY_METRIC);
	print_meta_value(obj->system_meta, ANX_META_UOR_MANIFEST_VERSION);
	print_meta_value(obj->system_meta, ANX_META_UOR_TOPOLOGY_EPOCH);

	anx_objstore_release(obj);
}

struct scan_print_ctx {
	uint32_t hits;
	uint32_t cap;
};

static int scan_print_cb(const anx_oid_t *oid, uint64_t bk,
			  uint32_t obj_type, void *arg)
{
	struct scan_print_ctx *ctx = arg;

	(void)obj_type;
	if (ctx->hits >= ctx->cap)
		return 1;
	kprintf("0x%016llx  %016llx%016llx  type=%u\n",
		(unsigned long long)bk,
		(unsigned long long)oid->hi,
		(unsigned long long)oid->lo,
		(unsigned)obj_type);
	ctx->hits++;
	return 0;
}

static void cmd_uor_scan(int argc, char **argv)
{
	uint64_t bk_lo = 0, bk_hi = (uint64_t)-1;
	struct scan_print_ctx ctx = { .hits = 0, .cap = 256 };

	if (argc >= 3 && parse_u64(argv[2], &bk_lo) != ANX_OK) {
		kprintf("uor: bad bk_lo\n");
		return;
	}
	if (argc >= 4 && parse_u64(argv[3], &bk_hi) != ANX_OK) {
		kprintf("uor: bad bk_hi\n");
		return;
	}
	kprintf("# range [0x%016llx, 0x%016llx]\n",
		(unsigned long long)bk_lo, (unsigned long long)bk_hi);
	if (anx_disk_range_scan(bk_lo, bk_hi, scan_print_cb, &ctx) != ANX_OK)
		kprintf("uor: range scan failed\n");
	else
		kprintf("# %u object(s)\n", (unsigned)ctx.hits);
}

static void cmd_uor_rebuild(void)
{
	uint32_t projected = 0;

	if (anx_uor_rebuild_topology_index(&projected) != ANX_OK) {
		kprintf("uor: rebuild failed\n");
		return;
	}
	kprintf("uor: reprojected %u object(s) (epoch=%llu)\n",
		(unsigned)projected,
		(unsigned long long)anx_uor_topology_epoch());
}

static void cmd_uor_project(int argc, char **argv)
{
	anx_oid_t oid;
	struct anx_state_object *obj;
	struct anx_uor_ref ref;

	if (argc < 3) {
		kprintf("usage: uor project <oid>\n");
		return;
	}
	if (parse_oid_hex(argv[2], &oid) != ANX_OK) {
		kprintf("uor: bad oid\n");
		return;
	}
	obj = anx_objstore_lookup(&oid);
	if (!obj) {
		kprintf("uor: object not found\n");
		return;
	}
	if (anx_uor_project_object(obj, &ref) != ANX_OK) {
		kprintf("uor: projection failed\n");
		anx_objstore_release(obj);
		return;
	}
	if (anx_uor_attach_metadata(obj, &ref) != ANX_OK)
		kprintf("uor: metadata attach failed (continuing)\n");

	{
		char hex[ANX_UOR_ADDR_BYTES * 2 + 1];

		anx_uor_format_address(ref.address, hex, sizeof(hex));
		kprintf("address      %s\n", hex);
		kprintf("boundary_key 0x%016llx\n",
			(unsigned long long)ref.boundary_key);
		kprintf("region       0x%016llx..0x%016llx\n",
			(unsigned long long)ref.region_lo,
			(unsigned long long)ref.region_hi);
	}
	anx_objstore_release(obj);
}

void cmd_uor(int argc, char **argv)
{
	if (argc < 2) {
		kprintf("usage: uor <inspect|scan|rebuild|project> [args]\n");
		return;
	}
	if (anx_strcmp(argv[1], "inspect") == 0) {
		cmd_uor_inspect(argc, argv);
	} else if (anx_strcmp(argv[1], "scan") == 0) {
		cmd_uor_scan(argc, argv);
	} else if (anx_strcmp(argv[1], "rebuild") == 0) {
		cmd_uor_rebuild();
	} else if (anx_strcmp(argv[1], "project") == 0) {
		cmd_uor_project(argc, argv);
	} else {
		kprintf("uor: unknown subcommand '%s'\n", argv[1]);
	}
}
