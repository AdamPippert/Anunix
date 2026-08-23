/*
 * test_exec_execution_contract.c — Tests for the Execution Contract
 * (RFC-0003 extension: staged/transactional effects).
 */

#include <anx/types.h>
#include <anx/cell.h>
#include <anx/string.h>

int test_exec_execution_contract(void)
{
	struct anx_cell *cell;
	struct anx_cell_intent intent;
	int ret;

	anx_cell_store_init();

	anx_memset(&intent, 0, sizeof(intent));
	anx_strlcpy(intent.name, "contract_test", sizeof(intent.name));
	anx_strlcpy(intent.objective, "test execution contract",
		     sizeof(intent.objective));

	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &cell);
	if (ret != ANX_OK)
		return -1;

	/* Default contract reproduces pre-contract behavior exactly. */
	if (cell->contract.consistency != ANX_CONSISTENCY_BEST_EFFORT)
		return -2;
	if (cell->contract.effect_mode != ANX_EFFECT_DIRECT)
		return -3;

	/* Null cell is rejected. */
	if (anx_cell_set_contract(NULL, ANX_CONSISTENCY_SEMANTIC,
				  ANX_EFFECT_STAGED) != ANX_EINVAL)
		return -4;

	/* Settable while CREATED. */
	ret = anx_cell_set_contract(cell, ANX_CONSISTENCY_TRANSACTIONAL,
				    ANX_EFFECT_STAGED);
	if (ret != ANX_OK)
		return -5;
	if (cell->contract.consistency != ANX_CONSISTENCY_TRANSACTIONAL)
		return -6;
	if (cell->contract.effect_mode != ANX_EFFECT_STAGED)
		return -7;

	/* Leave CREATED. */
	ret = anx_cell_transition(cell, ANX_CELL_ADMITTED);
	if (ret != ANX_OK)
		return -8;

	/* No longer settable once admitted. */
	ret = anx_cell_set_contract(cell, ANX_CONSISTENCY_BEST_EFFORT,
				    ANX_EFFECT_DIRECT);
	if (ret != ANX_EBUSY)
		return -9;

	/* Rejection must not have mutated the already-declared contract. */
	if (cell->contract.consistency != ANX_CONSISTENCY_TRANSACTIONAL)
		return -10;
	if (cell->contract.effect_mode != ANX_EFFECT_STAGED)
		return -11;

	anx_cell_destroy(cell);

	/*
	 * A STAGED-effect contract survives the full run pipeline
	 * unmodified. The runtime's commit stage does not yet perform
	 * real State Object writes (Memory Control Plane wiring is
	 * still pending — see runtime_commit in kernel/core/exec/runtime.c),
	 * so this checks that the declared contract is preserved end to
	 * end rather than asserting a write path that does not exist yet.
	 */
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &cell);
	if (ret != ANX_OK)
		return -12;

	ret = anx_cell_set_contract(cell, ANX_CONSISTENCY_TRANSACTIONAL,
				    ANX_EFFECT_STAGED);
	if (ret != ANX_OK)
		return -13;

	ret = anx_cell_run(cell);
	if (ret != ANX_OK)
		return -14;

	if (cell->status != ANX_CELL_COMPLETED)
		return -15;
	if (cell->contract.consistency != ANX_CONSISTENCY_TRANSACTIONAL)
		return -16;
	if (cell->contract.effect_mode != ANX_EFFECT_STAGED)
		return -17;

	anx_cell_destroy(cell);
	return 0;
}
