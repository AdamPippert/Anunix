/* One phase must relinquish its reservation before another can start. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/phase.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct phase_context { anx_cid_t owner; struct anx_phase_contract contract; bool foreign; };
static int phase_handler(struct anx_external_call *call, void *arg)
{
	struct phase_context *c = arg;
	struct anx_phase_view view;
	struct anx_phase_request request = { .phase = ANX_PHASE_WAIT, .memory_bytes = 64 };
	(void)call;
	if (c->foreign) {
		return anx_phase_get(&c->owner, &view) == ANX_EPERM &&
			anx_phase_begin(&c->owner, 1, &request) == ANX_EPERM &&
			anx_phase_finish(&c->owner, 1) == ANX_EPERM ? ANX_OK : -4208;
	}
	if (anx_phase_attach(&c->owner, &c->contract) != ANX_EPERM ||
	    anx_phase_detach(&c->owner) != ANX_EPERM || anx_phase_get(&c->owner, &view) != ANX_OK ||
	    view.role != ANX_ROLE_RUNNER || anx_phase_begin(&c->owner, view.epoch, &request) != ANX_OK ||
	    anx_phase_get(&c->owner, &view) != ANX_OK || view.phase != ANX_PHASE_WAIT ||
	    anx_phase_finish(&c->owner, view.epoch) != ANX_OK) return -4207;
	return ANX_OK;
}

int anx_research_day042(void)
{
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_phase_contract contract = { .role = ANX_ROLE_RUNNER };
	struct anx_phase_view view = {0}, next;
	struct anx_phase_request tool = { .phase = ANX_PHASE_TOOL, .memory_bytes = 2048 };
	struct anx_phase_request inference = { .phase = ANX_PHASE_INFERENCE, .memory_bytes = 4096, .accelerator_pct = 10 };
	struct anx_engine_lease *lease = NULL, *child = NULL;
	struct anx_external_call *call = NULL;
	struct phase_context context = {0};
	anx_eid_t first = ANX_UUID_NIL, child_id;
	uint64_t l0_before, l1_before, memory, old_epoch;
	uint32_t gpu_before, pct;
	bool attached = false;
	int ret;
	contract.limits[ANX_PHASE_TOOL] = (struct anx_phase_limit){ true, ANX_MEM_L1, 2048, ANX_ACCEL_NONE, 0 };
	contract.limits[ANX_PHASE_INFERENCE] = (struct anx_phase_limit){ true, ANX_MEM_L0, 4096, ANX_ACCEL_GPU, 10 };
	contract.limits[ANX_PHASE_WAIT] = (struct anx_phase_limit){ true, ANX_MEM_L1, 64, ANX_ACCEL_NONE, 0 };
	anx_strlcpy(intent.name, "research-day-042", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	struct anx_phase_contract bad = contract;
	bad.role = ANX_ROLE_CONTROL;
	ret = -4200;
	if (anx_phase_attach(&owner->cid, &bad) != ANX_EPERM) goto out;
	ret = anx_phase_attach(&owner->cid, &contract);
	if (ret != ANX_OK) goto out;
	attached = true;
	context.owner = owner->cid; context.contract = contract;
	/* Later edits to the caller's policy cannot expand the issued contract. */
	contract.limits[ANX_PHASE_INFERENCE].memory_bytes = 8192;
	ret = -4200;
	if (anx_phase_attach(&owner->cid, &contract) != ANX_EEXIST || anx_cell_destroy(owner) != ANX_EBUSY) goto out;
	anx_lease_avail_mem(ANX_MEM_L0, &l0_before);
	anx_lease_avail_mem(ANX_MEM_L1, &l1_before);
	anx_lease_avail_accel(ANX_ACCEL_GPU, &gpu_before);
	ret = anx_phase_get(&owner->cid, &view);
	if (ret == ANX_OK) ret = anx_phase_begin(&owner->cid, view.epoch, &tool);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &view);
	if (ret != ANX_OK) goto out;
	first = view.lease_id;
	ret = -4201;
	if (anx_phase_begin(&owner->cid, view.epoch, &inference) != ANX_EBUSY ||
	    anx_phase_get(&owner->cid, &next) != ANX_OK || next.epoch != view.epoch ||
	    anx_uuid_compare(&next.lease_id, &first)) goto out;
	anx_lease_avail_mem(ANX_MEM_L0, &memory); anx_lease_avail_accel(ANX_ACCEL_GPU, &pct);
	if (memory != l0_before || pct != gpu_before) goto out;
	anx_lease_avail_mem(ANX_MEM_L1, &memory);
	if (memory != l1_before - 2048) goto out;
	ret = -4202;
	lease = anx_lease_lookup(&first);
	if (!lease || anx_phase_finish(&owner->cid, view.epoch - 1) != ANX_EBUSY ||
	    anx_phase_detach(&owner->cid) != ANX_EBUSY) goto out;
	lease->mem_used_bytes = 1;
	if (anx_phase_finish(&owner->cid, view.epoch) != ANX_EBUSY ||
	    anx_phase_begin(&owner->cid, view.epoch, &inference) != ANX_EBUSY) goto out;
	lease->mem_used_bytes = 0;
	anx_uuid_generate(&child_id);
	ret = anx_lease_grant_child(lease, &child_id, 512, 0, &child);
	if (ret != ANX_OK) goto out;
	ret = -4203;
	if (anx_phase_finish(&owner->cid, view.epoch) != ANX_EBUSY) goto out;
	ret = anx_lease_release(child); child = NULL;
	if (ret == ANX_OK) ret = anx_phase_finish(&owner->cid, view.epoch);
	if (ret != ANX_OK) goto out;
	lease = NULL; old_epoch = view.epoch;
	anx_lease_avail_mem(ANX_MEM_L1, &memory);
	ret = -4204;
	if (memory != l1_before || anx_lease_lookup(&first) ||
	    anx_phase_finish(&owner->cid, old_epoch) != ANX_EBUSY ||
	    anx_phase_begin(&owner->cid, old_epoch, &inference) != ANX_EBUSY) goto out;
	ret = anx_phase_get(&owner->cid, &view);
	if (ret != ANX_OK) goto out;
	struct anx_phase_request over = inference;
	over.memory_bytes++;
	ret = -4205;
	if (anx_phase_begin(&owner->cid, view.epoch, &over) != ANX_EPERM) goto out;
	over = inference; over.accelerator_pct++;
	if (anx_phase_begin(&owner->cid, view.epoch, &over) != ANX_EPERM) goto out;
	over = tool; over.phase = ANX_PHASE_ORCHESTRATION;
	if (anx_phase_begin(&owner->cid, view.epoch, &over) != ANX_EPERM) goto out;
	ret = anx_phase_begin(&owner->cid, view.epoch, &inference);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &view);
	if (ret != ANX_OK) goto out;
	anx_lease_avail_mem(ANX_MEM_L0, &memory); anx_lease_avail_accel(ANX_ACCEL_GPU, &pct);
	ret = -4206;
	if (memory != l0_before - 4096 || pct != gpu_before - 10 ||
	    !anx_uuid_compare(&first, &view.lease_id) || view.tier != ANX_MEM_L0 ||
	    view.phase != ANX_PHASE_INFERENCE || view.accelerator != ANX_ACCEL_GPU) goto out;
	ret = anx_phase_finish(&owner->cid, view.epoch);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &view);
	if (ret != ANX_OK) goto out;
	old_epoch = view.epoch;
	ret = anx_phase_detach(&owner->cid);
	if (ret != ANX_OK) goto out;
	attached = false;
	ret = anx_phase_attach(&owner->cid, &context.contract);
	if (ret != ANX_OK) goto out;
	attached = true;
	ret = -4209;
	if (anx_phase_begin(&owner->cid, old_epoch, &tool) != ANX_EBUSY) goto out;
	ret = anx_external_register_handler("anxresearch042", phase_handler, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch042://phase", sizeof(call->endpoint));
	owner->ext_call = foreign->ext_call = call;
	owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	ret = anx_cell_run(owner);
	context.foreign = true;
	if (ret == ANX_OK) ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	ret = -4210;
	if (anx_phase_get(&owner->cid, &view) != ANX_OK ||
	    anx_phase_begin(&owner->cid, view.epoch, &tool) != ANX_EPERM) goto out;
	anx_lease_avail_mem(ANX_MEM_L0, &memory); anx_lease_avail_accel(ANX_ACCEL_GPU, &pct);
	if (memory != l0_before || pct != gpu_before) goto out;
	anx_lease_avail_mem(ANX_MEM_L1, &memory);
	if (memory != l1_before) goto out;
	ret = ANX_OK;
out:
	if (lease) lease->mem_used_bytes = 0;
	if (child) anx_lease_release(child);
	if (attached && owner) {
		if (anx_phase_get(&owner->cid, &view) == ANX_OK && view.phase != ANX_PHASE_IDLE)
			anx_phase_finish(&owner->cid, view.epoch);
		anx_phase_detach(&owner->cid);
	}
	if (owner) anx_cell_destroy(owner);
	if (foreign) anx_cell_destroy(foreign);
	if (call) anx_free(call);
	anx_external_unregister_handler("anxresearch042");
	return ret;
}
#endif
