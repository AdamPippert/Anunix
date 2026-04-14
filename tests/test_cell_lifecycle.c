/*
 * test_cell_lifecycle.c — Tests for cell lifecycle state machine.
 */

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/string.h>

int test_cell_lifecycle(void)
{
	struct anx_cell *cell;
	struct anx_cell_intent intent;
	int ret;

	anx_cell_store_init();

	/* Create a cell */
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "test_task", sizeof(intent.name));
	anx_strlcpy(intent.objective, "test lifecycle", sizeof(intent.objective));

	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &cell);
	if (ret != ANX_OK)
		return -1;

	if (cell->status != ANX_CELL_CREATED)
		return -2;

	/* Valid transition: created -> admitted */
	ret = anx_cell_transition(cell, ANX_CELL_ADMITTED);
	if (ret != ANX_OK)
		return -3;

	/* Invalid transition: admitted -> completed (should fail) */
	ret = anx_cell_transition(cell, ANX_CELL_COMPLETED);
	if (ret == ANX_OK)
		return -4;	/* should have failed */

	/* Valid path: admitted -> planning -> planned */
	ret = anx_cell_transition(cell, ANX_CELL_PLANNING);
	if (ret != ANX_OK)
		return -5;
	ret = anx_cell_transition(cell, ANX_CELL_PLANNED);
	if (ret != ANX_OK)
		return -6;

	/* Valid path: planned -> queued -> running */
	ret = anx_cell_transition(cell, ANX_CELL_QUEUED);
	if (ret != ANX_OK)
		return -7;
	ret = anx_cell_transition(cell, ANX_CELL_RUNNING);
	if (ret != ANX_OK)
		return -8;

	/* Valid path: running -> validating -> committing -> completed */
	ret = anx_cell_transition(cell, ANX_CELL_VALIDATING);
	if (ret != ANX_OK)
		return -9;
	ret = anx_cell_transition(cell, ANX_CELL_COMMITTING);
	if (ret != ANX_OK)
		return -10;
	ret = anx_cell_transition(cell, ANX_CELL_COMPLETED);
	if (ret != ANX_OK)
		return -11;

	/* Completed is terminal — should reject further transitions */
	ret = anx_cell_transition(cell, ANX_CELL_RUNNING);
	if (ret == ANX_OK)
		return -12;

	/* Lookup by CID */
	{
		struct anx_cell *found = anx_cell_store_lookup(&cell->cid);
		if (!found)
			return -13;
		anx_cell_store_release(found);
	}

	anx_cell_destroy(cell);
	return 0;
}
