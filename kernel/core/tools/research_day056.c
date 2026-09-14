/* Frozen planning separates retained bytes from restoration working capacity. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/twin.h>
#include <anx/string.h>
int anx_research_day056(void)
{
	struct anx_cell *cell = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_resource_twin *twin = NULL;
	struct anx_route_weight_policy policy;
	struct anx_twin_restore_request request = { .tier = ANX_MEM_L1,
		.retained_bytes = 64ULL * 1024 * 1024 * 1024, .resident_bytes = 0,
		.restore_peak_bytes = 512ULL * 1024 * 1024, .accelerator = ANX_ACCEL_NONE };
	struct anx_twin_restore_result result;
	anx_strlcpy(intent.name, "research-day-056", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &cell);
	if (ret == ANX_OK) ret = anx_twin_snapshot(&twin);
	if (ret != ANX_OK) goto out;
	anx_route_weight_policy_incumbent(&policy);
	ret = -5601;
	if (anx_twin_simulate_restoration(twin, cell, &policy, &request, &result) != ANX_OK ||
	    result.additional_memory_bytes != request.restore_peak_bytes) goto out;
	ret = ANX_OK;
out:
	anx_twin_destroy(twin);
	if (cell) anx_cell_destroy(cell);
	return ret;
}
#endif
