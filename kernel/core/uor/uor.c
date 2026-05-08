/*
 * uor.c — Universal Object Reference: topological coordinate layer.
 *
 * Projects State Objects into a deterministic, content-derived
 * coordinate space without replacing the OID/content_hash/provenance
 * authority model. See RFC-0002 §7 for the design and anx/uor.h for
 * the on-the-wire manifest layout.
 */

#include <anx/types.h>
#include <anx/uor.h>
#include <anx/state_object.h>
#include <anx/meta.h>
#include <anx/crypto.h>
#include <anx/string.h>
#include <anx/kprintf.h>

static uint64_t g_topology_epoch = 1;

/* --- little-endian writers ---------------------------------------- */

static int put_u16(uint8_t *buf, uint32_t cap, uint32_t *pos, uint16_t v)
{
	if (*pos + 2 > cap)
		return ANX_ENOMEM;
	buf[(*pos)++] = (uint8_t)(v & 0xff);
	buf[(*pos)++] = (uint8_t)((v >> 8) & 0xff);
	return ANX_OK;
}

static int put_u32(uint8_t *buf, uint32_t cap, uint32_t *pos, uint32_t v)
{
	if (*pos + 4 > cap)
		return ANX_ENOMEM;
	buf[(*pos)++] = (uint8_t)(v & 0xff);
	buf[(*pos)++] = (uint8_t)((v >> 8) & 0xff);
	buf[(*pos)++] = (uint8_t)((v >> 16) & 0xff);
	buf[(*pos)++] = (uint8_t)((v >> 24) & 0xff);
	return ANX_OK;
}

static int put_u64(uint8_t *buf, uint32_t cap, uint32_t *pos, uint64_t v)
{
	if (*pos + 8 > cap)
		return ANX_ENOMEM;
	for (uint32_t i = 0; i < 8; i++)
		buf[(*pos)++] = (uint8_t)((v >> (i * 8)) & 0xff);
	return ANX_OK;
}

static int put_bytes(uint8_t *buf, uint32_t cap, uint32_t *pos,
		     const void *src, uint32_t n)
{
	if (*pos + n > cap)
		return ANX_ENOMEM;
	anx_memcpy(buf + *pos, src, n);
	*pos += n;
	return ANX_OK;
}

static int put_oid(uint8_t *buf, uint32_t cap, uint32_t *pos,
		   const anx_oid_t *oid)
{
	int ret;

	ret = put_u64(buf, cap, pos, oid->hi);
	if (ret != ANX_OK)
		return ret;
	return put_u64(buf, cap, pos, oid->lo);
}

/* Cap a string at the manifest's per-string limit and emit
 * (uint16 length || bytes). NULL or empty becomes a zero-length entry.
 */
static int put_str(uint8_t *buf, uint32_t cap, uint32_t *pos, const char *s)
{
	uint32_t n = 0;
	int ret;

	if (s) {
		while (s[n] && n < 255)
			n++;
	}
	ret = put_u16(buf, cap, pos, (uint16_t)n);
	if (ret != ANX_OK)
		return ret;
	return put_bytes(buf, cap, pos, s, n);
}

/* --- canonical manifest ------------------------------------------- */

int anx_uor_build_manifest(const struct anx_state_object *obj,
			   struct anx_uor_manifest *out)
{
	uint32_t pos = 0;
	uint32_t parent_count;
	uint32_t i;
	int ret;

	if (!obj || !out)
		return ANX_EINVAL;

	anx_memset(out, 0, sizeof(*out));

	/* magic */
	ret = put_bytes(out->bytes, ANX_UOR_MANIFEST_MAX, &pos, "AUOR", 4);
	if (ret != ANX_OK)
		return ret;
	/* manifest_version */
	ret = put_u16(out->bytes, ANX_UOR_MANIFEST_MAX, &pos,
		      ANX_UOR_MANIFEST_VERSION);
	if (ret != ANX_OK)
		return ret;
	/* object_type — fits in u16 (current count well under 256) */
	ret = put_u16(out->bytes, ANX_UOR_MANIFEST_MAX, &pos,
		      (uint16_t)obj->object_type);
	if (ret != ANX_OK)
		return ret;
	/* oid */
	ret = put_oid(out->bytes, ANX_UOR_MANIFEST_MAX, &pos, &obj->oid);
	if (ret != ANX_OK)
		return ret;
	/* version */
	ret = put_u64(out->bytes, ANX_UOR_MANIFEST_MAX, &pos, obj->version);
	if (ret != ANX_OK)
		return ret;
	/* content_hash */
	ret = put_bytes(out->bytes, ANX_UOR_MANIFEST_MAX, &pos,
			obj->content_hash.bytes, 32);
	if (ret != ANX_OK)
		return ret;

	/* parents — bounded; surplus parents past MAX_PARENTS are
	 * intentionally elided so the manifest stays bounded. Their
	 * count is reflected as MAX_PARENTS so projection still tracks
	 * the truncation deterministically. */
	parent_count = obj->parent_count;
	if (parent_count > ANX_UOR_MANIFEST_MAX_PARENTS)
		parent_count = ANX_UOR_MANIFEST_MAX_PARENTS;
	ret = put_u32(out->bytes, ANX_UOR_MANIFEST_MAX, &pos, parent_count);
	if (ret != ANX_OK)
		return ret;
	for (i = 0; i < parent_count; i++) {
		ret = put_oid(out->bytes, ANX_UOR_MANIFEST_MAX, &pos,
			      &obj->parent_oids[i]);
		if (ret != ANX_OK)
			return ret;
	}

	/* schema fields */
	ret = put_str(out->bytes, ANX_UOR_MANIFEST_MAX, &pos, obj->schema_uri);
	if (ret != ANX_OK)
		return ret;
	ret = put_str(out->bytes, ANX_UOR_MANIFEST_MAX, &pos,
		      obj->schema_version);
	if (ret != ANX_OK)
		return ret;

	out->len = pos;
	return ANX_OK;
}

/* --- projection --------------------------------------------------- */

/* Big-endian load of n bytes (1..8) into a uint64. */
static uint64_t be_load(const uint8_t *p, uint32_t n)
{
	uint64_t v = 0;
	uint32_t i;

	for (i = 0; i < n; i++)
		v = (v << 8) | p[i];
	return v;
}

int anx_uor_project_manifest(const struct anx_uor_manifest *manifest,
			     uint16_t object_type,
			     struct anx_uor_ref *out)
{
	uint8_t hash[32];
	uint64_t content_bits;

	if (!manifest || !out || manifest->len == 0)
		return ANX_EINVAL;

	anx_memset(out, 0, sizeof(*out));

	anx_sha256(manifest->bytes, manifest->len, hash);
	anx_memcpy(out->address, hash, 32);

	/*
	 * boundary_key layout:
	 *   bits [63..56]  object_type (1 byte) — type-bucket clustering
	 *   bits [55..0]   address[1..7] big-endian
	 *
	 * Within a type bucket, content drives spread; sibling versions
	 * of the same object land at different keys because the manifest
	 * differs. The 16-bit region carves each type bucket into 256
	 * regions, suitable for memory-plane locality.
	 */
	content_bits = be_load(&hash[1], 7);
	out->boundary_key = ((uint64_t)(object_type & 0xff) << 56) | content_bits;

	out->region_lo = out->boundary_key & ~ANX_UOR_REGION_MASK;
	out->region_hi = out->region_lo | ANX_UOR_REGION_MASK;
	out->locality_metric = 0;
	out->manifest_version = ANX_UOR_MANIFEST_VERSION;
	out->flags = 0;

	return ANX_OK;
}

int anx_uor_project_object(const struct anx_state_object *obj,
			   struct anx_uor_ref *out)
{
	struct anx_uor_manifest m;
	int ret;

	if (!obj || !out)
		return ANX_EINVAL;

	ret = anx_uor_build_manifest(obj, &m);
	if (ret != ANX_OK)
		return ret;

	return anx_uor_project_manifest(&m, (uint16_t)obj->object_type, out);
}

uint64_t anx_uor_boundary_key(const struct anx_uor_ref *ref)
{
	return ref ? ref->boundary_key : 0;
}

/* --- metadata attachment ----------------------------------------- */

void anx_uor_format_address(const uint8_t addr[ANX_UOR_ADDR_BYTES],
			    char *out, uint32_t out_len)
{
	static const char hex[] = "0123456789abcdef";
	uint32_t i;

	if (!out || out_len == 0)
		return;
	if (out_len < ANX_UOR_ADDR_BYTES * 2 + 1) {
		out[0] = '\0';
		return;
	}
	for (i = 0; i < ANX_UOR_ADDR_BYTES; i++) {
		out[i * 2]     = hex[(addr[i] >> 4) & 0xf];
		out[i * 2 + 1] = hex[addr[i] & 0xf];
	}
	out[ANX_UOR_ADDR_BYTES * 2] = '\0';
}

int anx_uor_attach_metadata(struct anx_state_object *obj,
			    const struct anx_uor_ref *ref)
{
	char addr_hex[ANX_UOR_ADDR_BYTES * 2 + 1];

	if (!obj || !ref || !obj->system_meta)
		return ANX_EINVAL;

	anx_uor_format_address(ref->address, addr_hex, sizeof(addr_hex));

	anx_meta_set_str(obj->system_meta, ANX_META_UOR_ADDRESS, addr_hex);
	anx_meta_set_i64(obj->system_meta, ANX_META_UOR_BOUNDARY_KEY,
			 (int64_t)ref->boundary_key);
	anx_meta_set_i64(obj->system_meta, ANX_META_UOR_REGION_LO,
			 (int64_t)ref->region_lo);
	anx_meta_set_i64(obj->system_meta, ANX_META_UOR_REGION_HI,
			 (int64_t)ref->region_hi);
	anx_meta_set_i64(obj->system_meta, ANX_META_UOR_LOCALITY_METRIC,
			 (int64_t)ref->locality_metric);
	anx_meta_set_i64(obj->system_meta, ANX_META_UOR_MANIFEST_VERSION,
			 (int64_t)ref->manifest_version);
	anx_meta_set_i64(obj->system_meta, ANX_META_UOR_TOPOLOGY_EPOCH,
			 (int64_t)g_topology_epoch);
	return ANX_OK;
}

/* --- topology rebuild -------------------------------------------- */

struct rebuild_ctx {
	uint32_t projected;
};

static int rebuild_visit(struct anx_state_object *obj, void *arg)
{
	struct rebuild_ctx *ctx = arg;
	struct anx_uor_ref ref;

	if (!obj || !obj->system_meta)
		return 0;
	/* Skip objects that are not yet fully constructed. */
	if (obj->state == ANX_OBJ_DELETED || obj->state == ANX_OBJ_TOMBSTONE)
		return 0;

	if (anx_uor_project_object(obj, &ref) != ANX_OK)
		return 0;
	anx_uor_attach_metadata(obj, &ref);
	ctx->projected++;
	return 0;
}

int anx_uor_rebuild_topology_index(uint32_t *count_out)
{
	struct rebuild_ctx ctx = { .projected = 0 };
	int ret;

	g_topology_epoch++;
	ret = anx_objstore_iterate(rebuild_visit, &ctx);
	if (count_out)
		*count_out = ctx.projected;
	return ret;
}

uint64_t anx_uor_topology_epoch(void)
{
	return g_topology_epoch;
}

uint64_t anx_uor_topology_epoch_bump(void)
{
	return ++g_topology_epoch;
}
