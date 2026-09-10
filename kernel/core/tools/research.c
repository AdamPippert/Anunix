/* Native regressions for the daily research images. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>

static int research_echo(struct anx_external_call *call, void *ctx)
{
	uint32_t *calls = ctx;

	(*calls)++;
	call->response_buf[0] = 0x5a;
	call->response_size = 1;
	return ANX_OK;
}

int anx_research_day001(void)
{
	struct anx_cell *cell = NULL;
	struct anx_cell_intent intent;
	struct anx_external_call call;
	uint32_t calls = 0;
	int ret;

	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "research-day-001", sizeof(intent.name));
	anx_memset(&call, 0, sizeof(call));
	anx_strlcpy(call.endpoint, "anxresearch001://effect", sizeof(call.endpoint));
	ret = anx_external_register_handler("anxresearch001", research_echo, &calls);
	if (ret != ANX_OK)
		return ret;

	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
	if (ret != ANX_OK)
		goto out;
	cell->ext_call = &call;
	ret = -101;
	if (anx_cell_run(cell) != ANX_EPERM || calls != 0 ||
	    cell->status != ANX_CELL_FAILED || cell->error_code != ANX_EPERM ||
	    cell->attempt_count != 0 || anx_uuid_is_nil(&cell->trace_id))
		goto out;
	anx_cell_destroy(cell);
	cell = NULL;

	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
	if (ret != ANX_OK)
		goto out;
	cell->execution.allow_side_effects = true;
	cell->ext_call = &call;
	ret = -102;
	if (anx_cell_run(cell) != ANX_OK || calls != 1 ||
	    cell->status != ANX_CELL_COMPLETED || cell->attempt_count != 1 ||
	    call.response_size != 1 || call.response_buf[0] != 0x5a)
		goto out;
	anx_cell_destroy(cell);
	cell = NULL;

	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
	if (ret != ANX_OK)
		goto out;
	cell->execution.allow_side_effects = true;
	ret = -103;
	if (anx_cell_run(cell) != ANX_EINVAL || calls != 1 ||
	    cell->status != ANX_CELL_FAILED)
		goto out;
	ret = ANX_OK;
out:
	if (cell)
		anx_cell_destroy(cell);
	anx_external_unregister_handler("anxresearch001");
	return ret;
}

#ifdef ANX_RESEARCH_TEST
void cmd_research_test(int argc, char **argv)
{
	int ret;

	if (argc != 2 || anx_strcmp(argv[1], "day-001") != 0) {
		kprintf("usage: research-test day-001\n");
		return;
	}
	ret = anx_research_day001();
	kprintf("RESEARCH day-001 %s rc=%d\n", ret == ANX_OK ? "PASS" : "FAIL", ret);
}
#endif
#endif
