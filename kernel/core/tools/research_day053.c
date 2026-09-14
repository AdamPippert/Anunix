/* Unsupported consistency guarantees must fail before an external effect. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>

static int contract_call(struct anx_external_call *call, void *arg)
{
	(void)call;
	(*(uint32_t *)arg)++;
	return ANX_OK;
}

int anx_research_day053(void)
{
	struct anx_cell *cell = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	uint32_t calls = 0;
	int ret;
	anx_strlcpy(intent.name, "research-day-053", sizeof(intent.name));
	call = anx_zalloc(sizeof(*call));
	if (!call) return ANX_ENOMEM;
	anx_strlcpy(call->endpoint, "anxresearch053://contract", sizeof(call->endpoint));
	ret = anx_external_register_handler("anxresearch053", contract_call, &calls);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
	if (ret != ANX_OK) goto out;
	cell->ext_call = call; cell->execution.allow_side_effects = true;
	ret = anx_cell_set_contract(cell, ANX_CONSISTENCY_TRANSACTIONAL, ANX_EFFECT_STAGED);
	if (ret != ANX_OK) goto out;
	ret = -5301;
	if (anx_cell_run(cell) != ANX_ENOTSUP || calls || cell->status != ANX_CELL_FAILED) goto out;
	ret = ANX_OK;
out:
	anx_external_unregister_handler("anxresearch053");
	if (cell) anx_cell_destroy(cell);
	anx_free(call);
	return ret;
}
#endif
