#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/exposure.h>
#include <anx/cell.h>
int anx_research_day070(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_exposure_view view;
	int ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret != ANX_OK) return ret;
	ret = anx_exposure_create(&owner->cid, 0, 100, &view);
	if (ret == ANX_OK) anx_exposure_destroy(view.id);
	anx_cell_destroy(owner);
	return ret == ANX_OK ? ANX_OK : -7001;
}
#endif
