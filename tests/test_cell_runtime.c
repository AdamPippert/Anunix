/*
 * test_cell_runtime.c — Tests for the cell runtime pipeline.
 */

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/state_object.h>
#include <anx/uuid.h>
#include <anx/string.h>

int test_cell_runtime(void)
{
	struct anx_cell *cell;
	struct anx_cell *child;
	struct anx_cell_intent intent;
	int ret;

	anx_objstore_init();
	anx_cell_store_init();

	/* Create and run a simple cell through the full pipeline */
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "test_run", sizeof(intent.name));
	anx_strlcpy(intent.objective, "test full pipeline",
		     sizeof(intent.objective));

	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &cell);
	if (ret != ANX_OK)
		return -1;

	ret = anx_cell_run(cell);
	if (ret != ANX_OK)
		return -2;

	if (cell->status != ANX_CELL_COMPLETED)
		return -3;

	if (cell->attempt_count != 1)
		return -4;

	/* Trace should have been written */
	if (anx_uuid_is_nil(&cell->trace_id))
		return -5;

	/* Test child cell derivation */
	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "parent", sizeof(intent.name));
	{
		struct anx_cell *parent;
		struct anx_cell_intent child_intent;

		ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &parent);
		if (ret != ANX_OK)
			return -6;

		parent->execution.allow_recursive_cells = true;

		anx_memset(&child_intent, 0, sizeof(child_intent));
		anx_strlcpy(child_intent.name, "child",
			     sizeof(child_intent.name));

		ret = anx_cell_derive_child(parent, ANX_CELL_TASK_EXECUTION,
					    &child_intent, &child);
		if (ret != ANX_OK)
			return -7;

		if (child->recursion_depth != 1)
			return -8;

		if (parent->child_count != 1)
			return -9;

		anx_cell_destroy(child);
		anx_cell_destroy(parent);
	}

	anx_cell_destroy(cell);
	return 0;
}
