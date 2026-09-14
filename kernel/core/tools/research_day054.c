/* Versioned model state becomes visible through explicit publication. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/adapter.h>
#include <anx/cell.h>
#include <anx/string.h>
int anx_research_day054(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_adapter_image initial = { .format = 1, .count = 2, .deltas = {{'~','A',4096},{'A','A',4096}} };
	struct anx_adapter_view current;
	bool created = false;
	anx_strlcpy(intent.name, "research-day-054", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret != ANX_OK) return ret;
	ret = -5401;
	if (anx_adapter_create(&owner->cid, &initial, &current) != ANX_OK) goto out;
	created = true;
	ret = ANX_OK;
out:
	if (created) anx_adapter_destroy(&current.id);
	anx_cell_destroy(owner);
	return ret;
}
#endif
