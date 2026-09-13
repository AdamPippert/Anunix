/* Baseline: independent engine leases have no shared phase boundary. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/engine_lease.h>
#include <anx/uuid.h>

int anx_research_day042(void)
{
	struct anx_engine_lease *first = NULL, *next = NULL;
	anx_eid_t a, b;
	int ret;
	anx_uuid_generate(&a); anx_uuid_generate(&b);
	ret = anx_lease_grant(&a, ANX_MEM_L1, 2048, ANX_ACCEL_NONE, 0, &first);
	if (ret != ANX_OK) goto out;
	ret = -4201;
	if (anx_lease_grant(&b, ANX_MEM_L0, 4096, ANX_ACCEL_GPU, 10, &next) != ANX_EBUSY) goto out;
	ret = ANX_OK;
out:
	if (next) anx_lease_release(next);
	if (first) anx_lease_release(first);
	return ret;
}
#endif
