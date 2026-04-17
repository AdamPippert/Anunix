/*
 * test_disk_store.c — Locality-ordered storage (boundary_key).
 *
 * Exercises the new boundary-key APIs on the journaled disk store:
 * sorted index insertion, range scans, and bk retrieval. Runs against
 * the RAM-backed mock block device in the test harness.
 */

#include <anx/types.h>
#include <anx/objstore_disk.h>
#include <anx/mock_blk.h>
#include <anx/string.h>

struct scan_acc {
	uint64_t keys[32];
	anx_oid_t oids[32];
	uint32_t count;
};

static int collect_cb(const anx_oid_t *oid, uint64_t bk,
		      uint32_t obj_type, void *arg)
{
	struct scan_acc *a = arg;

	(void)obj_type;
	if (a->count >= 32)
		return 1;
	a->keys[a->count] = bk;
	a->oids[a->count] = *oid;
	a->count++;
	return 0;
}

int test_disk_store(void)
{
	anx_oid_t o1 = { .hi = 0, .lo = 1 };
	anx_oid_t o2 = { .hi = 0, .lo = 2 };
	anx_oid_t o3 = { .hi = 0, .lo = 3 };
	anx_oid_t o4 = { .hi = 0, .lo = 4 };
	anx_oid_t o5 = { .hi = 0, .lo = 5 };
	struct scan_acc acc;
	uint64_t bk;
	int ret;

	/* Mount a fresh mock disk. */
	test_mock_blk_init(1024);
	ret = anx_disk_format("test");
	if (ret != ANX_OK)
		return -1;

	/* Insert objects out of bk order. The sorted index should
	 * return them in ascending bk order. */
	if (anx_disk_write_obj_bk(&o1, 1, 500, "a", 1) != ANX_OK)
		return -2;
	if (anx_disk_write_obj_bk(&o2, 1, 100, "b", 1) != ANX_OK)
		return -3;
	if (anx_disk_write_obj_bk(&o3, 1, 900, "c", 1) != ANX_OK)
		return -4;
	if (anx_disk_write_obj_bk(&o4, 1, 100, "d", 1) != ANX_OK)
		return -5;
	if (anx_disk_write_obj_bk(&o5, 1, 300, "e", 1) != ANX_OK)
		return -6;

	/* Full range [0, UINT64_MAX] — all five in bk order. */
	anx_memset(&acc, 0, sizeof(acc));
	if (anx_disk_range_scan(0, (uint64_t)-1, collect_cb, &acc) != ANX_OK)
		return -7;
	if (acc.count != 5)
		return -8;
	/* Sorted by bk asc; ties broken by oid. o2 and o4 share bk=100. */
	if (acc.keys[0] != 100 || acc.keys[1] != 100 ||
	    acc.keys[2] != 300 ||
	    acc.keys[3] != 500 ||
	    acc.keys[4] != 900)
		return -9;
	/* Tie-break: o2 (lo=2) before o4 (lo=4) */
	if (acc.oids[0].lo != 2 || acc.oids[1].lo != 4)
		return -10;

	/* Narrow range [200, 600] — should hit o5 (300) and o1 (500) only. */
	anx_memset(&acc, 0, sizeof(acc));
	if (anx_disk_range_scan(200, 600, collect_cb, &acc) != ANX_OK)
		return -11;
	if (acc.count != 2)
		return -12;
	if (acc.keys[0] != 300 || acc.oids[0].lo != 5)
		return -13;
	if (acc.keys[1] != 500 || acc.oids[1].lo != 1)
		return -14;

	/* Empty range below everything. */
	anx_memset(&acc, 0, sizeof(acc));
	if (anx_disk_range_scan(0, 50, collect_cb, &acc) != ANX_OK)
		return -15;
	if (acc.count != 0)
		return -16;

	/* Empty range above everything. */
	anx_memset(&acc, 0, sizeof(acc));
	if (anx_disk_range_scan(1000, 2000, collect_cb, &acc) != ANX_OK)
		return -17;
	if (acc.count != 0)
		return -18;

	/* Boundary-key accessor. */
	if (anx_disk_get_boundary_key(&o3, &bk) != ANX_OK)
		return -19;
	if (bk != 900)
		return -20;

	/* Non-existent OID -> ENOENT. */
	{
		anx_oid_t miss = { .hi = 0xDEAD, .lo = 0xBEEF };

		if (anx_disk_get_boundary_key(&miss, &bk) != ANX_ENOENT)
			return -21;
	}

	/* Rewrite o1 with a new boundary_key — index stays sorted. */
	if (anx_disk_write_obj_bk(&o1, 1, 50, "A", 1) != ANX_OK)
		return -22;
	if (anx_disk_get_boundary_key(&o1, &bk) != ANX_OK)
		return -23;
	if (bk != 50)
		return -24;

	anx_memset(&acc, 0, sizeof(acc));
	if (anx_disk_range_scan(0, (uint64_t)-1, collect_cb, &acc) != ANX_OK)
		return -25;
	if (acc.count != 5)
		return -26;
	/* o1 now has bk=50, which is the smallest. */
	if (acc.keys[0] != 50 || acc.oids[0].lo != 1)
		return -27;
	/* Verify the rest is still in order */
	{
		uint32_t i;

		for (i = 1; i < acc.count; i++) {
			if (acc.keys[i] < acc.keys[i - 1])
				return -28;
		}
	}

	/* Invalid range bk_hi < bk_lo. */
	if (anx_disk_range_scan(500, 100, collect_cb, &acc) != ANX_EINVAL)
		return -29;

	/* Legacy write (no bk) lands at bk=0. */
	{
		anx_oid_t o6 = { .hi = 0, .lo = 6 };

		if (anx_disk_write_obj(&o6, 1, "f", 1) != ANX_OK)
			return -30;
		if (anx_disk_get_boundary_key(&o6, &bk) != ANX_OK)
			return -31;
		if (bk != 0)
			return -32;
	}

	test_mock_blk_teardown();
	return 0;
}
