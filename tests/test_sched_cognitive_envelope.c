/*
 * test_sched_cognitive_envelope.c — Cell cognitive envelope (RFC-0029).
 */

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/string.h>

int test_sched_cognitive_envelope(void)
{
	struct anx_cell *cell;
	struct anx_cell_intent intent;
	int ret;

	anx_cell_store_init();

	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "envelope_test", sizeof(intent.name));
	if (anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &cell) != ANX_OK)
		return -1;

	/* Default is unset — zero-cost, matches pre-RFC-0029 behavior. */
	if (cell->cognitive.max_tokens != 0)
		return -2;
	if (cell->cognitive.max_reasoning_depth != 0)
		return -3;

	/* Set while CREATED succeeds. */
	ret = anx_cell_set_cognitive_envelope(cell, 512, 3);
	if (ret != ANX_OK)
		return -4;
	if (cell->cognitive.max_tokens != 512)
		return -5;
	if (cell->cognitive.max_reasoning_depth != 3)
		return -6;

	/* Null cell rejected. */
	if (anx_cell_set_cognitive_envelope(NULL, 1, 1) != ANX_EINVAL)
		return -7;

	/* Once the cell leaves CREATED, the envelope is fixed. */
	ret = anx_cell_transition(cell, ANX_CELL_ADMITTED);
	if (ret != ANX_OK)
		return -8;

	ret = anx_cell_set_cognitive_envelope(cell, 999, 9);
	if (ret != ANX_EBUSY)
		return -9;

	/* Unchanged after the rejected attempt. */
	if (cell->cognitive.max_tokens != 512)
		return -10;

	return 0;
}
