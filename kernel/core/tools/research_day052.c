/* Idle capacity is released without discarding the phase entitlement. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/phase.h>
#include <anx/cell.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/external_call.h>
#include <anx/alloc.h>

struct capacity_context { anx_cid_t owner; uint64_t epoch; };
static int active_capacity(struct anx_external_call *call, void *arg)
{
	struct capacity_context *c = arg;
	struct anx_phase_view view;
	(void)call;
	return anx_phase_park(&c->owner, c->epoch, &view) == ANX_EPERM &&
		anx_phase_resume(&c->owner, c->epoch, &view) == ANX_EPERM ? ANX_OK : -5208;
}

int anx_research_day052(void)
{
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_external_call *call = NULL;
	struct anx_engine_lease *child = NULL, *rival = NULL, *lease = NULL;
	struct capacity_context context;
	anx_eid_t id;
	uint64_t memory_before, available;
	uint32_t pct_before, percent;
	struct anx_phase_view sentinel, saved;
	anx_memset(&sentinel, 0x55, sizeof(sentinel)); saved = sentinel;
	anx_lease_avail_mem(ANX_MEM_L1, &memory_before);
	anx_lease_avail_accel(ANX_ACCEL_GPU, &pct_before);
	struct anx_cell_intent intent = {0};
	struct anx_phase_contract contract = { .role = ANX_ROLE_RUNNER };
	struct anx_phase_request request = { ANX_PHASE_INFERENCE, 4096, 25 };
	struct anx_phase_view active, parked;
	bool attached = false;
	int ret;
	anx_strlcpy(intent.name, "research-day-052", sizeof(intent.name));
	contract.limits[ANX_PHASE_INFERENCE] = (struct anx_phase_limit){ true, ANX_MEM_L1, 8192, ANX_ACCEL_GPU, 50 };
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_phase_attach(&owner->cid, &contract);
	if (ret != ANX_OK) goto out;
	attached = true;
	ret = anx_phase_get(&owner->cid, &active);
	if (ret == ANX_OK) ret = anx_phase_begin(&owner->cid, active.epoch, &request);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &active);
	if (ret != ANX_OK) goto out;
	lease = anx_lease_lookup(&active.lease_id);
	if (!lease) { ret = ANX_ENOENT; goto out; }
	lease->mem_used_bytes = 1;
	ret = -5202;
	if (anx_phase_park(&owner->cid, active.epoch, &sentinel) != ANX_EBUSY) goto out;
	lease->mem_used_bytes = 0;
	anx_uuid_generate(&id);
	ret = anx_lease_grant_child(lease, &id, 0, 0, &child);
	if (ret != ANX_OK) goto out;
	ret = -5202;
	if (anx_phase_park(&owner->cid, active.epoch, &sentinel) != ANX_EBUSY ||
	    anx_memcmp(&sentinel, &saved, sizeof(saved))) goto out;
	ret = anx_lease_release(child); child = NULL;
	if (ret != ANX_OK) goto out;
	ret = -5201;
	if (anx_phase_park(&owner->cid, active.epoch, &parked) != ANX_OK || !parked.parked ||
	    parked.memory_bytes || parked.accelerator_pct || parked.phase != active.phase ||
	    anx_uuid_compare(&parked.owner, &active.owner)) goto out;
	lease = NULL;
	anx_lease_avail_mem(ANX_MEM_L1, &available);
	anx_lease_avail_accel(ANX_ACCEL_GPU, &percent);
	ret = -5203;
	if (available != memory_before || percent != pct_before || !anx_uuid_is_nil(&parked.lease_id) ||
	    anx_lease_lookup(&active.lease_id) || parked.epoch <= active.epoch ||
	    anx_phase_begin(&owner->cid, parked.epoch, &request) != ANX_EBUSY ||
	    anx_phase_detach(&owner->cid) != ANX_EBUSY ||
	    anx_phase_resize(&owner->cid, parked.epoch, 8192, 50, &sentinel) != ANX_EBUSY ||
	    anx_phase_park(&owner->cid, parked.epoch, &sentinel) != ANX_EBUSY ||
	    anx_phase_resume(&owner->cid, active.epoch, &sentinel) != ANX_EBUSY) goto out;
	ret = anx_lease_grant(&id, ANX_MEM_L1, memory_before, ANX_ACCEL_NONE, 0, &rival);
	if (ret != ANX_OK) goto out;
	struct anx_phase_view current;
	ret = -5204;
	if (anx_phase_resume(&owner->cid, parked.epoch, &sentinel) != ANX_ENOMEM ||
	    anx_phase_get(&owner->cid, &current) != ANX_OK || anx_memcmp(&current, &parked, sizeof(current))) goto out;
	ret = anx_lease_release(rival); rival = NULL;
	if (ret == ANX_OK) ret = anx_lease_grant(&id, ANX_MEM_L1, 0, ANX_ACCEL_GPU, pct_before, &rival);
	if (ret != ANX_OK) goto out;
	ret = -5205;
	if (anx_phase_resume(&owner->cid, parked.epoch, &sentinel) != ANX_ENOMEM ||
	    anx_phase_get(&owner->cid, &current) != ANX_OK || anx_memcmp(&current, &parked, sizeof(current)) ||
	    anx_memcmp(&sentinel, &saved, sizeof(saved))) goto out;
	ret = anx_lease_release(rival); rival = NULL;
	if (ret != ANX_OK) goto out;
	context = (struct capacity_context){ owner->cid, parked.epoch };
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch052", active_capacity, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	if (!call) { ret = ANX_ENOMEM; goto out; }
	anx_strlcpy(call->endpoint, "anxresearch052://capacity", sizeof(call->endpoint));
	foreign->ext_call = call; foreign->execution.allow_side_effects = true;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	/* Caller-owned request and contract storage cannot alter the saved entitlement. */
	contract.limits[ANX_PHASE_INFERENCE].memory_bytes = ~(uint64_t)0;
	request.memory_bytes = 8192;
	ret = anx_phase_resume(&owner->cid, parked.epoch, &current);
	if (ret != ANX_OK) goto out;
	ret = -5206;
	if (current.parked || current.memory_bytes != 4096 || current.accelerator_pct != 25 ||
	    current.epoch <= parked.epoch || current.phase != active.phase || anx_uuid_compare(&current.owner, &active.owner) ||
	    !anx_uuid_compare(&current.lease_id, &active.lease_id) ||
	    anx_phase_resume(&owner->cid, current.epoch, &sentinel) != ANX_EBUSY ||
	    anx_phase_resize(&owner->cid, current.epoch, 8193, 25, &sentinel) != ANX_EPERM) goto out;
	anx_lease_avail_mem(ANX_MEM_L1, &available);
	anx_lease_avail_accel(ANX_ACCEL_GPU, &percent);
	if (available != memory_before - 4096 || percent != pct_before - 25) goto out;
	ret = anx_phase_resize(&owner->cid, current.epoch, 1024, 10, &active);
	if (ret == ANX_OK) ret = anx_phase_park(&owner->cid, active.epoch, &parked);
	if (ret == ANX_OK) ret = anx_phase_resume(&owner->cid, parked.epoch, &current);
	if (ret != ANX_OK) goto out;
	ret = -5207;
	if (current.memory_bytes != 1024 || current.accelerator_pct != 10 ||
	    anx_uuid_compare(&current.owner, &owner->cid) || current.phase != ANX_PHASE_INFERENCE) goto out;
	ret = anx_phase_park(&owner->cid, current.epoch, &parked);
	if (ret != ANX_OK) goto out;
	owner->status = ANX_CELL_CANCELLED;
	ret = -5209;
	if (anx_phase_resume(&owner->cid, parked.epoch, &sentinel) != ANX_EPERM ||
	    anx_memcmp(&sentinel, &saved, sizeof(saved))) goto out;
	ret = anx_phase_finish(&owner->cid, parked.epoch);
	if (ret != ANX_OK) goto out;
	ret = -5210;
	if (anx_phase_get(&owner->cid, &current) != ANX_OK || current.parked || current.phase != ANX_PHASE_IDLE) goto out;
	ret = ANX_OK;
out:
	anx_external_unregister_handler("anxresearch052");
	if (attached) {
		struct anx_phase_view state;
		if (anx_phase_get(&owner->cid, &state) == ANX_OK) {
			lease = anx_lease_lookup(&state.lease_id);
			if (lease) lease->mem_used_bytes = 0;
		}
	}
	if (child) anx_lease_release(child);
	if (rival) anx_lease_release(rival);
	if (attached) {
		struct anx_phase_view current;
		if (anx_phase_get(&owner->cid, &current) == ANX_OK && current.phase != ANX_PHASE_IDLE) anx_phase_finish(&owner->cid, current.epoch);
		anx_phase_detach(&owner->cid);
	}
	anx_lease_avail_mem(ANX_MEM_L1, &available);
	anx_lease_avail_accel(ANX_ACCEL_GPU, &percent);
	if (ret == ANX_OK && (available != memory_before || percent != pct_before)) ret = -5211;
	if (foreign) anx_cell_destroy(foreign);
	anx_free(call);
	if (owner) anx_cell_destroy(owner);
	return ret;
}
#endif
