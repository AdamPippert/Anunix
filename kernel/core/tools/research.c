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

	if (argc != 2) {
		kprintf("usage: research-test day-NNN\n");
		return;
	}
	if (anx_strcmp(argv[1], "day-001") == 0)
		ret = anx_research_day001();
	else if (anx_strcmp(argv[1], "day-002") == 0)
		ret = anx_research_day002();
	else if (anx_strcmp(argv[1], "day-003") == 0)
		ret = anx_research_day003();
	else if (anx_strcmp(argv[1], "day-004") == 0)
		ret = anx_research_day004();
	else if (anx_strcmp(argv[1], "day-005") == 0)
		ret = anx_research_day005();
	else if (anx_strcmp(argv[1], "day-006") == 0)
		ret = anx_research_day006();
	else if (anx_strcmp(argv[1], "day-007") == 0)
		ret = anx_research_day007();
	else if (anx_strcmp(argv[1], "day-008") == 0)
		ret = anx_research_day008();
	else if (anx_strcmp(argv[1], "day-009") == 0)
		ret = anx_research_day009();
	else if (anx_strcmp(argv[1], "day-010") == 0)
		ret = anx_research_day010();
	else if (anx_strcmp(argv[1], "day-011") == 0)
		ret = anx_research_day011();
	else if (anx_strcmp(argv[1], "day-012") == 0)
		ret = anx_research_day012();
	else if (anx_strcmp(argv[1], "day-013") == 0)
		ret = anx_research_day013();
	else if (anx_strcmp(argv[1], "day-014") == 0)
		ret = anx_research_day014();
	else if (anx_strcmp(argv[1], "day-015") == 0)
		ret = anx_research_day015();
	else if (anx_strcmp(argv[1], "day-016") == 0)
		ret = anx_research_day016();
	else if (anx_strcmp(argv[1], "day-017") == 0)
		ret = anx_research_day017();
	else if (anx_strcmp(argv[1], "day-018") == 0)
		ret = anx_research_day018();
	else if (anx_strcmp(argv[1], "day-019") == 0)
		ret = anx_research_day019();
	else if (anx_strcmp(argv[1], "day-020") == 0)
		ret = anx_research_day020();
	else if (anx_strcmp(argv[1], "day-021") == 0)
		ret = anx_research_day021();
	else if (anx_strcmp(argv[1], "day-022") == 0)
		ret = anx_research_day022();
	else if (anx_strcmp(argv[1], "day-023") == 0)
		ret = anx_research_day023();
	else if (anx_strcmp(argv[1], "day-024") == 0)
		ret = anx_research_day024();
	else if (anx_strcmp(argv[1], "day-025") == 0)
		ret = anx_research_day025();
	else if (anx_strcmp(argv[1], "day-026") == 0)
		ret = anx_research_day026();
	else if (anx_strcmp(argv[1], "day-027") == 0)
		ret = anx_research_day027();
	else if (anx_strcmp(argv[1], "day-028") == 0)
		ret = anx_research_day028();
	else if (anx_strcmp(argv[1], "day-029") == 0)
		ret = anx_research_day029();
	else if (anx_strcmp(argv[1], "day-030") == 0)
		ret = anx_research_day030();
	else if (anx_strcmp(argv[1], "day-031") == 0)
		ret = anx_research_day031();
	else if (anx_strcmp(argv[1], "day-032") == 0)
		ret = anx_research_day032();
	else if (anx_strcmp(argv[1], "day-033") == 0)
		ret = anx_research_day033();
	else if (anx_strcmp(argv[1], "day-034") == 0)
		ret = anx_research_day034();
	else if (anx_strcmp(argv[1], "day-035") == 0)
		ret = anx_research_day035();
	else if (anx_strcmp(argv[1], "day-036") == 0)
		ret = anx_research_day036();
	else if (anx_strcmp(argv[1], "day-037") == 0)
		ret = anx_research_day037();
	else if (anx_strcmp(argv[1], "day-038") == 0)
		ret = anx_research_day038();
	else if (anx_strcmp(argv[1], "day-039") == 0)
		ret = anx_research_day039();
	else if (anx_strcmp(argv[1], "day-040") == 0)
		ret = anx_research_day040();
	else if (anx_strcmp(argv[1], "day-041") == 0)
		ret = anx_research_day041();
	else if (anx_strcmp(argv[1], "day-042") == 0)
		ret = anx_research_day042();
	else if (anx_strcmp(argv[1], "day-043") == 0)
		ret = anx_research_day043();
	else if (anx_strcmp(argv[1], "day-044") == 0)
		ret = anx_research_day044();
	else if (anx_strcmp(argv[1], "day-045") == 0)
		ret = anx_research_day045();
	else if (anx_strcmp(argv[1], "day-046") == 0)
		ret = anx_research_day046();
	else if (anx_strcmp(argv[1], "day-047") == 0)
		ret = anx_research_day047();
	else if (anx_strcmp(argv[1], "day-048") == 0)
		ret = anx_research_day048();
	else if (anx_strcmp(argv[1], "day-049") == 0)
		ret = anx_research_day049();
	else if (anx_strcmp(argv[1], "day-050") == 0)
		ret = anx_research_day050();
	else if (anx_strcmp(argv[1], "day-051") == 0)
		ret = anx_research_day051();
	else if (anx_strcmp(argv[1], "day-052") == 0)
		ret = anx_research_day052();
	else if (anx_strcmp(argv[1], "day-053") == 0)
		ret = anx_research_day053();
	else if (anx_strcmp(argv[1], "day-054") == 0)
		ret = anx_research_day054();
	else if (anx_strcmp(argv[1], "day-055") == 0)
		ret = anx_research_day055();
	else if (anx_strcmp(argv[1], "day-056") == 0)
		ret = anx_research_day056();
	else if (anx_strcmp(argv[1], "day-057") == 0)
		ret = anx_research_day057();
	else if (anx_strcmp(argv[1], "day-058") == 0)
		ret = anx_research_day058();
	else if (anx_strcmp(argv[1], "day-059") == 0)
		ret = anx_research_day059();
	else if (anx_strcmp(argv[1], "day-060") == 0)
		ret = anx_research_day060();
	else if (anx_strcmp(argv[1], "day-061") == 0)
		ret = anx_research_day061();
	else if (anx_strcmp(argv[1], "day-062") == 0)
		ret = anx_research_day062();
	else if (anx_strcmp(argv[1], "day-063") == 0)
		ret = anx_research_day063();
	else if (anx_strcmp(argv[1], "day-064") == 0)
		ret = anx_research_day064();
	else if (anx_strcmp(argv[1], "day-065") == 0)
		ret = anx_research_day065();
	else if (anx_strcmp(argv[1], "day-066") == 0)
		ret = anx_research_day066();
	else if (anx_strcmp(argv[1], "day-067") == 0)
		ret = anx_research_day067();
	else if (anx_strcmp(argv[1], "day-068") == 0)
		ret = anx_research_day068();
	else if (anx_strcmp(argv[1], "day-069") == 0)
		ret = anx_research_day069();
	else if (anx_strcmp(argv[1], "day-070") == 0)
		ret = anx_research_day070();
	else if (anx_strcmp(argv[1], "day-071") == 0)
		ret = anx_research_day071();
	else {
		kprintf("unknown research test: %s\n", argv[1]);
		return;
	}
	kprintf("RESEARCH %s %s rc=%d\n", argv[1], ret == ANX_OK ? "PASS" : "FAIL", ret);
}
#endif
#endif
