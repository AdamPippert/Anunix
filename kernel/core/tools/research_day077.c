#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/workload.h>
#include <anx/cell.h>
#include <anx/string.h>
int anx_research_day077(void)
{
	struct anx_cell *owner = NULL; struct anx_cell_intent intent = {0};
	struct anx_workload_contract contract = {.minimum_output=4,.maximum_output=8,.maximum_tokens=8,.prefix_size=4,.prefix="AAAA"};
	struct anx_workload_view view;
	anx_strlcpy(intent.name,"research-day-077",sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL,&intent,&owner);
	if (ret == ANX_OK && anx_workload_create(&owner->cid,&contract,&view) != ANX_OK) ret = -7701;
	if (owner) anx_cell_destroy(owner);
	return ret;
}
#endif
