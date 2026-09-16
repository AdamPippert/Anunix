#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/continuation_group.h>
#include <anx/engine_lease.h>
#include <anx/effect_fence.h>
#include <anx/external_call.h>
#include <anx/sched.h>
#include <anx/alloc.h>
#include <anx/page.h>
#include <anx/string.h>
#include <anx/uuid.h>
#include <anx/kprintf.h>
struct fixture082 {
	struct anx_cell *owner, *members[3], *foreign;
	struct anx_continuation_group_view group, output;
	struct anx_external_call call;
	uint32_t calls;
	bool foreign_active;
};
static int active082(struct anx_external_call *call, void *arg)
{
	struct fixture082 *f = arg; (void)call;
	char bytes[8] = {0};
	if (anx_continuation_group_create(&f->owner->cid, f->group.members, 3, 2, &f->output) != ANX_EPERM ||
	    anx_continuation_group_destroy(f->group.id) != ANX_EPERM ||
	    anx_continuation_group_revoke(f->group.id, f->group.epoch, &f->output) != ANX_EPERM ||
	    anx_continuation_group_handoff(f->group.id, f->group.epoch, &f->members[2]->cid, &f->output) != ANX_EPERM) return -8210;
	if (f->foreign_active) return anx_continuation_group_get(f->group.id, &f->output) == ANX_EPERM &&
		anx_continuation_group_read(f->group.id, f->group.epoch, 4094, bytes, 8) == ANX_EPERM ? ANX_OK : -8211;
	if (anx_continuation_group_get(f->group.id, &f->output) != ANX_OK || f->output.holder != f->calls ||
	    anx_continuation_group_read(f->group.id, f->group.epoch + 1, 4094, bytes, 8) != ANX_EBUSY ||
	    anx_continuation_group_write(f->group.id, f->group.epoch, 8190, "overflow", 8) != ANX_EINVAL) return -8212;
	if (f->calls && (anx_continuation_group_read(f->group.id, f->group.epoch, 4094, bytes, 8) != ANX_OK ||
	    anx_memcmp(bytes, "step-one", 8))) return -8213;
	int ret = anx_continuation_group_write(f->group.id, f->group.epoch, 4094, f->calls ? "step-two" : "step-one", 8);
	if (ret == ANX_OK) f->calls++;
	return ret;
}
int anx_research_day082(void)
{
	struct fixture082 *f = anx_zalloc(sizeof(*f));
	struct anx_cell_intent intent = {0};
	struct anx_effect_fence_view fence;
	anx_cid_t members[3], queued;
	anx_eid_t lease_id;
	uint64_t before, available, total, free_before, free_after;
	char bytes[8];
	int ret = ANX_ENOMEM;
	if (!f) return ret;
	anx_strlcpy(intent.name, "research-day-082", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &f->owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &f->foreign);
	if (ret != ANX_OK) goto out;
	f->owner->execution.allow_recursive_cells = f->owner->execution.allow_side_effects = true;
	f->foreign->execution.allow_side_effects = true;
	ret = anx_effect_fence_create(&fence);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(f->owner, &fence.id);
	for (uint32_t i = 0; ret == ANX_OK && i < 3; i++) {
		ret = anx_cell_derive_child(f->owner, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &f->members[i]);
		if (ret == ANX_OK) members[i] = f->members[i]->cid;
	}
	if (ret != ANX_OK) goto out;
	ret = anx_lease_avail_mem(ANX_MEM_L0, &before);
	if (ret != ANX_OK) goto out;
	ret = -8201;
	if (anx_continuation_group_create(&f->owner->cid, members, 3, 2, &f->group) != ANX_OK) goto out;
	lease_id = f->group.lease_id;
	anx_lease_avail_mem(ANX_MEM_L0, &available);
	if (f->group.cpu_slots != 1 || f->group.physical_pages != 2 || f->group.memory_bytes != 8192 ||
	    f->group.holder || f->group.epoch != 1 || before - available != 8192 ||
	    anx_cell_destroy(f->members[0]) != ANX_EBUSY) { ret = -8202; goto out; }
	struct anx_engine_lease *lease = anx_lease_lookup(&lease_id);
	if (!lease || lease->mem_used_bytes != 8192 || lease->mem_reserved_bytes != 8192) { ret = -8202; goto out; }
	f->output = f->group; f->output.holder = 2; f->output.memory_bytes = 65536;
	if (anx_continuation_group_create(&f->owner->cid, members, 3, 2, &f->output) != ANX_EEXIST ||
	    anx_continuation_group_create(&f->owner->cid, members, 3, 5, &f->output) != ANX_EINVAL ||
	    anx_continuation_group_create(&f->owner->cid, members, 0, 2, &f->output) != ANX_EINVAL) { ret = -8203; goto out; }
	ret = anx_external_register_handler("anxresearch082", active082, f);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint, "anxresearch082://step", sizeof(f->call.endpoint));
	for (uint32_t i = 0; i < 3; i++) f->members[i]->ext_call = &f->call;
	f->foreign->ext_call = &f->call;
	if (anx_cell_run(f->members[1]) != ANX_EBUSY || f->members[1]->status != ANX_CELL_CREATED || f->calls ||
	    anx_sched_enqueue(&members[1], ANX_QUEUE_BATCH, ANX_PRIO_NORMAL) != ANX_EBUSY) { ret = -8204; goto out; }
	f->output = f->group;
	if (anx_continuation_group_handoff(f->group.id, 2, &members[1], &f->output) != ANX_EBUSY ||
	    anx_continuation_group_handoff(f->group.id, 1, &f->foreign->cid, &f->output) != ANX_EPERM ||
	    anx_memcmp(&f->output, &f->group, sizeof(f->group))) { ret = -8205; goto out; }
	f->members[1]->parent_cid = ANX_UUID_NIL;
	ret = anx_continuation_group_handoff(f->group.id, 1, &members[1], &f->output);
	f->members[1]->parent_cid = f->owner->cid;
	if (ret != ANX_EPERM || anx_memcmp(&f->output, &f->group, sizeof(f->group))) { ret = -8205; goto out; }
	f->foreign_active = true; ret = anx_cell_run(f->foreign); f->foreign_active = false;
	if (ret == ANX_OK) ret = anx_cell_run(f->members[0]);
	if (ret != ANX_OK) goto out;
	ret = anx_continuation_group_handoff(f->group.id, f->group.epoch, &members[1], &f->group);
	if (ret != ANX_OK) goto out;
	anx_lease_avail_mem(ANX_MEM_L0, &available);
	if (f->group.epoch != 2 || f->group.holder != 1 || f->group.physical_pages != 2 ||
	    anx_uuid_compare(&f->group.lease_id, &lease_id) || before - available != 8192) { ret = -8206; goto out; }
	anx_memset(bytes, 0x55, sizeof(bytes));
	if (anx_continuation_group_read(f->group.id, 1, 4094, bytes, 8) != ANX_EBUSY || bytes[0] != 0x55) { ret = -8206; goto out; }
	ret = anx_cell_run(f->members[1]);
	if (ret == ANX_OK) ret = anx_continuation_group_read(f->group.id, f->group.epoch, 4094, bytes, 8);
	/* A completed holder cannot access the group again; controller hands off first. */
	if (ret != ANX_EPERM || f->calls != 2) { ret = -8207; goto out; }
	ret = anx_effect_fence_hold(f->owner);
	if (ret != ANX_OK) goto out;
	f->output = f->group;
	if (anx_continuation_group_handoff(f->group.id, f->group.epoch, &members[2], &f->output) != ANX_EBUSY ||
	    anx_memcmp(&f->output, &f->group, sizeof(f->group))) { ret = -8207; goto out; }
	ret = anx_effect_fence_get(&fence.id, &fence);
	if (ret == ANX_OK) ret = anx_effect_fence_transition(&fence.id, fence.generation, ANX_FENCE_RUNNING);
	if (ret == ANX_OK) ret = anx_continuation_group_handoff(f->group.id, f->group.epoch, &members[2], &f->group);
	if (ret == ANX_OK) ret = anx_continuation_group_read(f->group.id, f->group.epoch, 4094, bytes, 8);
	if (ret != ANX_OK || anx_memcmp(bytes, "step-two", 8)) { ret = -8208; goto out; }
	ret = anx_sched_enqueue(&members[2], ANX_QUEUE_BATCH, ANX_PRIO_NORMAL);
	if (ret != ANX_OK) goto out;
	if (anx_continuation_group_revoke(f->group.id, 2, &f->output) != ANX_EBUSY) { ret = -8208; goto out; }
	anx_page_stats(&total, &free_before);
	ret = anx_continuation_group_revoke(f->group.id, f->group.epoch, &f->group);
	anx_page_stats(&total, &free_after);
	anx_lease_avail_mem(ANX_MEM_L0, &available);
	if (ret != ANX_OK || !f->group.revoked || f->group.physical_pages || free_after != free_before + 2 ||
	    available != before || anx_lease_lookup(&lease_id) || anx_cell_run(f->members[2]) != ANX_EPERM || f->calls != 2 ||
	    anx_sched_dequeue(ANX_QUEUE_BATCH, &queued) != ANX_ENOENT ||
	    anx_continuation_group_handoff(f->group.id, f->group.epoch, &members[2], &f->output) != ANX_EPERM ||
	    anx_continuation_group_write(f->group.id, f->group.epoch, 0, "denied", 6) != ANX_EPERM) { ret = -8209; goto out; }
	ret = ANX_OK;
	kprintf("day082 holders=3 executed=2 shared_bytes=8192 lease_identity=stable physical_pages=2->0 revoked_dispatch=denied\n");
out:
	anx_external_unregister_handler("anxresearch082");
	if (f->group.id && anx_continuation_group_destroy(f->group.id) != ANX_OK && ret == ANX_OK) ret = -8214;
	for (uint32_t i = 0; i < 3; i++) if (f->members[i]) { anx_sched_cancel(&f->members[i]->cid); anx_cell_destroy(f->members[i]); }
	if (f->foreign) anx_cell_destroy(f->foreign);
	if (f->owner) anx_cell_destroy(f->owner);
	if (ret != ANX_OK) kprintf("day082 native failure rc=%d calls=%u\n", ret, f->calls);
	anx_free(f); return ret;
}
#endif
