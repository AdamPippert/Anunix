/* Resource allocation changes while task and phase identity remain stable. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/phase.h>
#include <anx/cell.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/external_call.h>
#include <anx/alloc.h>

struct resize_context { anx_cid_t owner; uint64_t epoch; struct anx_engine_lease *lease; };
static int active_resize(struct anx_external_call *call, void *arg)
{
	struct resize_context *c = arg;
	struct anx_phase_view view;
	(void)call;
	return anx_phase_resize(&c->owner, c->epoch, 2048, 20, &view) == ANX_EPERM &&
		anx_lease_resize(c->lease, 2048, 20) == ANX_EPERM ? ANX_OK : -5110;
}

int anx_research_day051(void)
{
	struct anx_cell *owner = NULL;
	struct anx_cell *foreign = NULL;
	struct anx_external_call *call = NULL;
	struct anx_engine_lease *lease = NULL, *children[2] = {0}, *rival = NULL;
	anx_eid_t ids[3];
	anx_time_t granted = 0;
	uint64_t memory_before, available;
	uint32_t accel_before, percent;
	struct resize_context context;
	struct anx_cell_intent intent = {0};
	struct anx_phase_contract contract = { .role = ANX_ROLE_RUNNER };
	struct anx_phase_request request = { ANX_PHASE_TOOL, 1024, 10 };
	struct anx_phase_view before, after;
	bool attached = false;
	int ret;
	anx_strlcpy(intent.name, "research-day-051", sizeof(intent.name));
	anx_lease_avail_mem(ANX_MEM_L1, &memory_before);
	anx_lease_avail_accel(ANX_ACCEL_GPU, &accel_before);
	contract.limits[ANX_PHASE_TOOL] = (struct anx_phase_limit){ true, ANX_MEM_L1, 8192, ANX_ACCEL_GPU, 50 };
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_phase_attach(&owner->cid, &contract);
	if (ret != ANX_OK) goto out;
	attached = true;
	ret = anx_phase_get(&owner->cid, &before);
	if (ret == ANX_OK) ret = anx_phase_begin(&owner->cid, before.epoch, &request);
	if (ret == ANX_OK) ret = anx_phase_get(&owner->cid, &before);
	if (ret != ANX_OK) goto out;
	lease = anx_lease_lookup(&before.lease_id);
	if (!lease) { ret = ANX_ENOENT; goto out; }
	granted = lease->granted_at;
	ret = -5101;
	if (anx_phase_resize(&owner->cid, before.epoch, 2048, 20, &after) != ANX_OK ||
	    anx_uuid_compare(&before.owner, &after.owner) || anx_uuid_compare(&before.lease_id, &after.lease_id) ||
	    before.phase != after.phase || after.memory_bytes != 2048 || after.accelerator_pct != 20) goto out;
	ret = -5102;
	anx_lease_avail_mem(ANX_MEM_L1, &available);
	anx_lease_avail_accel(ANX_ACCEL_GPU, &percent);
	if (anx_lease_lookup(&after.lease_id) != lease || lease->granted_at != granted ||
	    lease->mem_reserved_bytes != 2048 || lease->accel_pct != 20 ||
	    available != memory_before - 2048 || percent != accel_before - 20 || after.epoch <= before.epoch) goto out;
	struct anx_phase_view sentinel;
	anx_memset(&sentinel, 0x55, sizeof(sentinel));
	struct anx_phase_view unchanged = sentinel;
	if (anx_phase_resize(&owner->cid, before.epoch, 4096, 30, &sentinel) != ANX_EBUSY ||
	    anx_phase_finish(&owner->cid, before.epoch) != ANX_EBUSY ||
	    anx_phase_resize(&owner->cid, after.epoch, 8193, 20, &sentinel) != ANX_EPERM ||
	    anx_phase_resize(&owner->cid, after.epoch, 2048, 51, &sentinel) != ANX_EPERM ||
	    anx_memcmp(&sentinel, &unchanged, sizeof(sentinel))) goto out;
	for (uint32_t i = 0; i < 3; i++) anx_uuid_generate(&ids[i]);
	ret = anx_lease_grant(&ids[2], ANX_MEM_L1, available, ANX_ACCEL_NONE, 0, &rival);
	if (ret != ANX_OK) goto out;
	ret = -5103;
	if (anx_phase_resize(&owner->cid, after.epoch, 2049, 20, &sentinel) != ANX_ENOMEM ||
	    lease->mem_reserved_bytes != 2048 || anx_memcmp(&sentinel, &unchanged, sizeof(sentinel))) goto out;
	ret = anx_lease_release(rival); rival = NULL;
	if (ret == ANX_OK) ret = anx_lease_grant(&ids[2], ANX_MEM_L1, 0, ANX_ACCEL_GPU, percent, &rival);
	if (ret != ANX_OK) goto out;
	ret = -5104;
	if (anx_phase_resize(&owner->cid, after.epoch, 2048, 21, &sentinel) != ANX_ENOMEM || lease->accel_pct != 20) goto out;
	ret = anx_lease_release(rival); rival = NULL;
	if (ret == ANX_OK) ret = anx_lease_grant_child(lease, &ids[0], 512, 5, &children[0]);
	if (ret == ANX_OK) ret = anx_lease_grant_child(lease, &ids[1], 256, 5, &children[1]);
	if (ret != ANX_OK) goto out;
	lease->mem_used_bytes = 256;
	ret = anx_lease_resize(children[0], 1024, 10);
	if (ret != ANX_OK) goto out;
	ret = -5105;
	if (anx_lease_resize(children[0], 1537, 10) != ANX_ENOMEM ||
	    anx_lease_resize(children[0], 1024, 16) != ANX_ENOMEM || children[0]->mem_reserved_bytes != 1024 ||
	    anx_phase_resize(&owner->cid, after.epoch, 1024, 10, &sentinel) != ANX_EBUSY ||
	    anx_phase_resize(&owner->cid, after.epoch, 255, 20, &sentinel) != ANX_EBUSY ||
	    anx_memcmp(&sentinel, &unchanged, sizeof(sentinel))) goto out;
	ret = anx_lease_resize(children[0], 512, 5);
	if (ret == ANX_OK) ret = anx_phase_resize(&owner->cid, after.epoch, 1024, 10, &before);
	if (ret != ANX_OK) goto out;
	ret = -5106;
	if (anx_uuid_compare(&before.owner, &owner->cid) || anx_uuid_compare(&before.lease_id, &after.lease_id) ||
	    before.epoch <= after.epoch || lease->granted_at != granted ||
	    children[0]->parent != lease || children[1]->parent != lease || lease->mem_reserved_bytes != 1024) goto out;
	context = (struct resize_context){owner->cid, before.epoch, lease};
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch051", active_resize, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	if (!call) { ret = ANX_ENOMEM; goto out; }
	anx_strlcpy(call->endpoint, "anxresearch051://resize", sizeof(call->endpoint));
	foreign->ext_call = call; foreign->execution.allow_side_effects = true;
	ret = anx_cell_run(foreign);
	if (ret != ANX_OK) goto out;
	lease->mem_used_bytes = 0;
	ret = anx_lease_revoke(children[0]);
	if (ret != ANX_OK) goto out;
	ret = -5107;
	if (anx_lease_resize(children[0], 512, 5) != ANX_EPERM || anx_lease_resize(NULL, 0, 0) != ANX_EINVAL ||
	    anx_lease_resize(children[1], 256, 101) != ANX_EINVAL) goto out;
	for (uint32_t i = 0; i < 2; i++) {
		ret = anx_lease_release(children[i]);
		if (ret != ANX_OK) goto out;
		children[i] = NULL;
	}
	/* The phase wrapper detects direct controller changes to its backing lease. */
	ret = anx_lease_resize(lease, 1025, 10);
	if (ret != ANX_OK) goto out;
	ret = -5108;
	if (anx_phase_resize(&owner->cid, before.epoch, 2048, 20, &sentinel) != ANX_EBUSY) goto out;
	ret = anx_lease_resize(lease, 1024, 10);
	if (ret != ANX_OK) goto out;
	owner->status = ANX_CELL_CANCELLED;
	ret = -5109;
	if (anx_phase_resize(&owner->cid, before.epoch, 2048, 20, &sentinel) != ANX_EPERM ||
	    anx_memcmp(&sentinel, &unchanged, sizeof(sentinel))) goto out;
	ret = ANX_OK;
out:
	anx_external_unregister_handler("anxresearch051");
	if (lease) lease->mem_used_bytes = 0;
	for (uint32_t i = 0; i < 2; i++) if (children[i]) anx_lease_release(children[i]);
	if (rival) anx_lease_release(rival);
	if (attached) {
		struct anx_phase_view current;
		if (anx_phase_get(&owner->cid, &current) == ANX_OK && current.phase != ANX_PHASE_IDLE) anx_phase_finish(&owner->cid, current.epoch);
		anx_phase_detach(&owner->cid);
	}
	anx_lease_avail_mem(ANX_MEM_L1, &available);
	anx_lease_avail_accel(ANX_ACCEL_GPU, &percent);
	if (ret == ANX_OK && (available != memory_before || percent != accel_before)) ret = -5111;
	if (foreign) anx_cell_destroy(foreign);
	anx_free(call);
	if (owner) anx_cell_destroy(owner);
	return ret;
}
#endif
