/* Resource allocation changes while task and phase identity remain stable. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/phase.h>
#include <anx/cell.h>
#include <anx/string.h>
#include <anx/uuid.h>

int anx_research_day051(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_phase_contract contract = { .role = ANX_ROLE_RUNNER };
	struct anx_phase_request request = { ANX_PHASE_TOOL, 1024, 10 };
	struct anx_phase_view before, after;
	bool attached = false;
	int ret;
	anx_strlcpy(intent.name, "research-day-051", sizeof(intent.name));
	contract.limits[ANX_PHASE_TOOL] = (struct anx_phase_limit){ true, ANX_MEM_L1, 8192, ANX_ACCEL_GPU, 50 };
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_phase_attach(&owner->cid, &contract);
	if (ret != ANX_OK) goto out;
	attached = true;
	ret = anx_phase_get(&owner->cid, &before);
	if (ret == ANX_OK) ret = anx_phase_begin(&owner->cid, before.epoch, &request);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &before);
	if (ret != ANX_OK) goto out;
	ret = -5101;
	if (anx_phase_resize(&owner->cid, before.epoch, 2048, 20, &after) != ANX_OK ||
	    anx_uuid_compare(&before.owner, &after.owner) || anx_uuid_compare(&before.lease_id, &after.lease_id) ||
	    before.phase != after.phase || after.memory_bytes != 2048 || after.accelerator_pct != 20) goto out;
	ret = ANX_OK;
out:
	if (attached) {
		struct anx_phase_view current;
		if (anx_phase_get(&owner->cid, &current) == ANX_OK && current.phase != ANX_PHASE_IDLE) anx_phase_finish(&owner->cid, current.epoch);
		anx_phase_detach(&owner->cid);
	}
	if (owner) anx_cell_destroy(owner);
	return ret;
}
#endif
