/*
 * test_uor.c — UOR projection and topology rebuild (RFC-0002 §7).
 *
 * Exercises:
 *   - canonical manifest determinism
 *   - projection sensitivity to content_hash and version
 *   - boundary_key layout (type byte || content prefix)
 *   - region bounds
 *   - metadata attachment on create and seal
 *   - topology rebuild walks the full object store
 *   - boundary-key-driven disk range scans cluster by region
 */

#include <anx/types.h>
#include <anx/state_object.h>
#include <anx/uor.h>
#include <anx/meta.h>
#include <anx/string.h>
#include <anx/objstore_disk.h>
#include <anx/mock_blk.h>

#define COLLECT_MAX	8

struct collect_acc {
	anx_oid_t hits[COLLECT_MAX];
	uint32_t  count;
};

static int collect_cb(const anx_oid_t *oid, uint64_t bk,
		      uint32_t obj_type, void *arg)
{
	struct collect_acc *a = arg;

	(void)bk;
	(void)obj_type;
	if (a->count < COLLECT_MAX)
		a->hits[a->count++] = *oid;
	return 0;
}

int test_uor(void)
{
	struct anx_state_object *obj_a, *obj_b;
	struct anx_so_create_params p;
	struct anx_uor_manifest m1, m2;
	struct anx_uor_ref r1, r2;
	int ret;

	anx_objstore_init();

	/* ----- 1. Manifest determinism ----- */
	anx_memset(&p, 0, sizeof(p));
	p.object_type = ANX_OBJ_BYTE_DATA;
	p.payload = "hello";
	p.payload_size = 5;
	ret = anx_so_create(&p, &obj_a);
	if (ret != ANX_OK)
		return -1;

	if (anx_uor_build_manifest(obj_a, &m1) != ANX_OK)
		return -2;
	if (anx_uor_build_manifest(obj_a, &m2) != ANX_OK)
		return -3;
	if (m1.len != m2.len ||
	    anx_memcmp(m1.bytes, m2.bytes, m1.len) != 0)
		return -4;

	/* ----- 2. Projection determinism ----- */
	if (anx_uor_project_object(obj_a, &r1) != ANX_OK)
		return -5;
	if (anx_uor_project_object(obj_a, &r2) != ANX_OK)
		return -6;
	if (anx_memcmp(r1.address, r2.address, ANX_UOR_ADDR_BYTES) != 0)
		return -7;
	if (r1.boundary_key != r2.boundary_key)
		return -8;
	if (r1.region_lo != r2.region_lo || r1.region_hi != r2.region_hi)
		return -9;

	/* ----- 3. Boundary-key layout ----- */
	if ((r1.boundary_key >> 56) != (uint64_t)ANX_OBJ_BYTE_DATA)
		return -10;
	if ((r1.region_hi - r1.region_lo + 1) != ANX_UOR_REGION_WIDTH)
		return -11;
	if ((r1.boundary_key & ~ANX_UOR_REGION_MASK) != r1.region_lo)
		return -12;

	/* ----- 4. Content sensitivity ----- */
	anx_memset(&p, 0, sizeof(p));
	p.object_type = ANX_OBJ_BYTE_DATA;
	p.payload = "world";
	p.payload_size = 5;
	ret = anx_so_create(&p, &obj_b);
	if (ret != ANX_OK)
		return -13;

	struct anx_uor_ref rb;
	if (anx_uor_project_object(obj_b, &rb) != ANX_OK)
		return -14;
	if (anx_memcmp(r1.address, rb.address, ANX_UOR_ADDR_BYTES) == 0)
		return -15;
	if ((r1.boundary_key >> 56) != (rb.boundary_key >> 56))
		return -16;

	/* ----- 5. Type bucketing ----- */
	{
		struct anx_state_object *obj_emb;
		struct anx_uor_ref re;

		anx_memset(&p, 0, sizeof(p));
		p.object_type = ANX_OBJ_EMBEDDING;
		p.payload = "hello";
		p.payload_size = 5;
		ret = anx_so_create(&p, &obj_emb);
		if (ret != ANX_OK)
			return -17;
		if (anx_uor_project_object(obj_emb, &re) != ANX_OK)
			return -18;
		if ((re.boundary_key >> 56) != (uint64_t)ANX_OBJ_EMBEDDING)
			return -19;
		if ((re.boundary_key >> 56) == (r1.boundary_key >> 56))
			return -20;
	}

	/* ----- 6. Metadata attachment ----- */
	{
		const struct anx_meta_value *v;

		v = anx_meta_get(obj_a->system_meta, ANX_META_UOR_ADDRESS);
		if (!v || v->type != ANX_META_STRING)
			return -21;
		if (v->v.str.len != ANX_UOR_ADDR_BYTES * 2)
			return -22;

		v = anx_meta_get(obj_a->system_meta, ANX_META_UOR_BOUNDARY_KEY);
		if (!v || v->type != ANX_META_INT64)
			return -23;
		if ((uint64_t)v->v.i64 != r1.boundary_key)
			return -24;

		v = anx_meta_get(obj_a->system_meta, ANX_META_UOR_REGION_LO);
		if (!v || (uint64_t)v->v.i64 != r1.region_lo)
			return -25;
	}

	/* ----- 7. Reprojection on seal ----- */
	{
		struct anx_object_handle h;
		const struct anx_meta_value *v;
		uint64_t bk_before, bk_after;

		v = anx_meta_get(obj_a->system_meta, ANX_META_UOR_BOUNDARY_KEY);
		bk_before = (uint64_t)v->v.i64;

		ret = anx_so_open(&obj_a->oid, ANX_OPEN_WRITE, &h);
		if (ret != ANX_OK)
			return -26;
		ret = anx_so_replace_payload(&h, "HELLO!", 6);
		if (ret != ANX_OK)
			return -27;
		anx_so_close(&h);

		ret = anx_so_seal(&obj_a->oid);
		if (ret != ANX_OK)
			return -28;

		v = anx_meta_get(obj_a->system_meta, ANX_META_UOR_BOUNDARY_KEY);
		if (!v)
			return -29;
		bk_after = (uint64_t)v->v.i64;
		if (bk_before == bk_after)
			return -30;
		if ((bk_after >> 56) != (uint64_t)ANX_OBJ_BYTE_DATA)
			return -31;
	}

	/* ----- 8. Topology rebuild ----- */
	{
		uint32_t projected = 0;
		uint64_t epoch_before, epoch_after;

		epoch_before = anx_uor_topology_epoch();
		ret = anx_uor_rebuild_topology_index(&projected);
		if (ret != ANX_OK)
			return -32;
		if (projected < 3)
			return -33;
		epoch_after = anx_uor_topology_epoch();
		if (epoch_after <= epoch_before)
			return -34;
	}

	/* ----- 9. Disk-store range scan via UOR-derived boundary keys ----- */
	{
		struct anx_state_object *o[3];
		anx_oid_t oids[3];
		uint64_t bks[3];
		struct anx_uor_ref r;
		struct collect_acc acc;
		uint64_t region_lo, region_hi;
		uint32_t i;

		test_mock_blk_init(2048);
		if (anx_disk_format("uor") != ANX_OK)
			return -35;

		for (i = 0; i < 3; i++) {
			static const char *blobs[3] = { "alpha", "beta", "gamma" };
			static const uint32_t lens[3] = { 5, 4, 5 };

			anx_memset(&p, 0, sizeof(p));
			p.object_type = ANX_OBJ_BYTE_DATA;
			p.payload = blobs[i];
			p.payload_size = lens[i];
			if (anx_so_create(&p, &o[i]) != ANX_OK)
				return -36 - (int)i;
			oids[i] = o[i]->oid;

			if (anx_uor_project_object(o[i], &r) != ANX_OK)
				return -39 - (int)i;
			bks[i] = r.boundary_key;

			if (anx_disk_write_obj_bk(&oids[i], ANX_OBJ_BYTE_DATA,
						   r.boundary_key,
						   o[i]->payload,
						   (uint32_t)o[i]->payload_size)
			    != ANX_OK)
				return -42 - (int)i;
		}

		/* Type-bucket scan covers every BYTE_DATA bk. */
		region_lo = (uint64_t)ANX_OBJ_BYTE_DATA << 56;
		region_hi = region_lo | ((1ULL << 56) - 1);

		anx_memset(&acc, 0, sizeof(acc));
		if (anx_disk_range_scan(region_lo, region_hi,
					 collect_cb, &acc) != ANX_OK)
			return -45;
		if (acc.count != 3)
			return -46;

		/* Persisted bk == projected bk for each object. */
		for (i = 0; i < 3; i++) {
			uint64_t bk_persisted = 0;

			if (anx_disk_get_boundary_key(&oids[i],
						       &bk_persisted) != ANX_OK)
				return -47 - (int)i;
			if (bk_persisted != bks[i])
				return -50 - (int)i;
		}

		/* Scan above the BYTE_DATA bucket returns nothing. */
		anx_memset(&acc, 0, sizeof(acc));
		if (anx_disk_range_scan(region_hi + 1, (uint64_t)-1,
					 collect_cb, &acc) != ANX_OK)
			return -53;
		if (acc.count != 0)
			return -54;

		test_mock_blk_teardown();
	}

	return 0;
}
