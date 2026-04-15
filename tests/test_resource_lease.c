/*
 * test_resource_lease.c — Tests for engine resource leasing.
 */

#include <anx/types.h>
#include <anx/engine_lease.h>
#include <anx/uuid.h>

int test_resource_lease(void)
{
	struct anx_engine_lease *lease1, *lease2;
	anx_eid_t eid1, eid2, eid_bad;
	uint64_t avail_mem;
	uint32_t avail_pct;
	int ret;

	anx_lease_init();

	anx_uuid_generate(&eid1);
	anx_uuid_generate(&eid2);
	anx_uuid_generate(&eid_bad);

	/* Grant a lease: 4 GiB memory, 50% GPU */
	ret = anx_lease_grant(&eid1, ANX_MEM_L1,
			      4ULL * 1024 * 1024 * 1024,
			      ANX_ACCEL_GPU, 50, &lease1);
	if (ret != ANX_OK)
		return -1;

	if (lease1->mem_reserved_bytes != 4ULL * 1024 * 1024 * 1024)
		return -2;
	if (lease1->accel_pct != 50)
		return -3;

	/* Check available memory decreased */
	ret = anx_lease_avail_mem(ANX_MEM_L1, &avail_mem);
	if (ret != ANX_OK)
		return -4;
	if (avail_mem != (16ULL - 4) * 1024 * 1024 * 1024)
		return -5;

	/* Check available GPU decreased */
	ret = anx_lease_avail_accel(ANX_ACCEL_GPU, &avail_pct);
	if (ret != ANX_OK)
		return -6;
	if (avail_pct != 50)
		return -7;

	/* Grant second lease: 8 GiB memory, 30% GPU */
	ret = anx_lease_grant(&eid2, ANX_MEM_L1,
			      8ULL * 1024 * 1024 * 1024,
			      ANX_ACCEL_GPU, 30, &lease2);
	if (ret != ANX_OK)
		return -8;

	/* Check cumulative availability */
	ret = anx_lease_avail_mem(ANX_MEM_L1, &avail_mem);
	if (ret != ANX_OK)
		return -9;
	if (avail_mem != 4ULL * 1024 * 1024 * 1024)
		return -10;

	ret = anx_lease_avail_accel(ANX_ACCEL_GPU, &avail_pct);
	if (ret != ANX_OK)
		return -11;
	if (avail_pct != 20)
		return -12;

	/* Lookup by engine ID */
	{
		struct anx_engine_lease *found;

		found = anx_lease_lookup(&eid1);
		if (!found || found != lease1)
			return -13;

		found = anx_lease_lookup(&eid_bad);
		if (found)
			return -14;
	}

	/* Exhaustion: try to grant more memory than available */
	{
		struct anx_engine_lease *bad;
		anx_eid_t eid3;

		anx_uuid_generate(&eid3);
		ret = anx_lease_grant(&eid3, ANX_MEM_L1,
				      5ULL * 1024 * 1024 * 1024,
				      ANX_ACCEL_NONE, 0, &bad);
		if (ret != ANX_ENOMEM)
			return -15;
	}

	/* Exhaustion: try to grant more GPU than available */
	{
		struct anx_engine_lease *bad;
		anx_eid_t eid3;

		anx_uuid_generate(&eid3);
		ret = anx_lease_grant(&eid3, ANX_MEM_L1,
				      1ULL * 1024 * 1024 * 1024,
				      ANX_ACCEL_GPU, 25, &bad);
		if (ret != ANX_ENOMEM)
			return -16;
	}

	/* Release first lease, resources returned */
	ret = anx_lease_release(lease1);
	if (ret != ANX_OK)
		return -17;

	ret = anx_lease_avail_mem(ANX_MEM_L1, &avail_mem);
	if (ret != ANX_OK)
		return -18;
	if (avail_mem != (16ULL - 8) * 1024 * 1024 * 1024)
		return -19;

	ret = anx_lease_avail_accel(ANX_ACCEL_GPU, &avail_pct);
	if (ret != ANX_OK)
		return -20;
	if (avail_pct != 70)
		return -21;

	/* Different tier is independent */
	ret = anx_lease_avail_mem(ANX_MEM_L0, &avail_mem);
	if (ret != ANX_OK)
		return -22;
	if (avail_mem != 16ULL * 1024 * 1024 * 1024)
		return -23;

	anx_lease_release(lease2);
	return 0;
}
