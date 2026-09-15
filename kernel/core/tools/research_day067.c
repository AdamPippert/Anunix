/* A continuation survives replacement of its capability workers. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/continuation.h>
#include <anx/string.h>
int anx_research_day067(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_continuation_view view;
	anx_strlcpy(intent.name, "research-day-067", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret != ANX_OK) return ret;
	owner->execution.allow_recursive_cells = owner->execution.allow_side_effects = true;
	ret = anx_continuation_create(&owner->cid, &view) == ANX_OK ? ANX_OK : -6701;
	anx_cell_destroy(owner);
	return ret;
}
#endif
