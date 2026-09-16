/* Revoking a scheduler domain contains its descendants without stopping peers. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/sched_domain.h>
#include <anx/string.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/arch.h>
#include <anx/kprintf.h>
#include <anx/external_operation.h>
#include <anx/cell_trace.h>
#include <anx/state_object.h>

struct fixture061 {
	struct anx_external_call call, side, response;
	struct anx_sched_domain_view own;
	uint64_t root_id;
	anx_oid_t operation;
	anx_time_t expiry;
	uint32_t calls, side_calls;
	bool expiring;
};
static int side061(struct anx_external_call *call, void *arg)
{
	(void)call;
	((struct fixture061 *)arg)->side_calls++;
	return ANX_OK;
}
static int active061(struct anx_external_call *call, void *arg)
{
	struct fixture061 *f = arg;
	struct anx_sched_domain_view out, sentinel;
	(void)call;
	f->calls++;
	anx_memset(&out, 0x55, sizeof(out)); sentinel = out;
	if (anx_sched_domain_create(anx_cell_current_id(), 0, &f->own.authority, &out) != ANX_EPERM ||
	    anx_sched_domain_revoke(f->own.id, f->own.epoch) != ANX_EPERM ||
	    anx_sched_domain_destroy(f->own.id) != ANX_EPERM || anx_memcmp(&out, &sentinel, sizeof(out)) ||
	    anx_sched_domain_get(f->root_id, &out) != ANX_EPERM || anx_memcmp(&out, &sentinel, sizeof(out)) ||
	    anx_sched_domain_get(f->own.id, &out) != ANX_OK || out.id != f->own.id) return -6111;
	if (!f->expiring) return ANX_OK;
	/* The real clock expires authority during this synchronous callback. */
	while (arch_time_now() < f->expiry) { }
	uint32_t limit = 0x5555;
	if (anx_external_invoke(&f->side) != ANX_ETIMEDOUT || f->side_calls ||
	    anx_external_operation_dispatch(&f->operation, &f->response) != ANX_ETIMEDOUT || f->side_calls ||
	    anx_cell_cognitive_limit(4, &limit) != ANX_ETIMEDOUT || limit != 0x5555) return -6112;
	return ANX_OK;
}
static bool traced061(struct anx_cell *cell)
{
	struct anx_state_object *obj = anx_objstore_lookup(&cell->trace_oid);
	if (!obj) return false;
	const struct anx_cell_trace *trace = obj->payload;
	bool valid = obj->payload_size == sizeof(*trace) && trace->finalized && !trace->tool.attempted &&
		trace->denied_gate == ANX_ADMISSION_SCHEDULER_DOMAIN;
	anx_objstore_release(obj);
	return valid;
}
int anx_research_day061(void)
{
	struct anx_cell *root = NULL, *bad = NULL, *grand = NULL, *leaf = NULL, *good = NULL, *expiry = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_sched_domain_view domain = {0}, child = {0}, grand_domain = {0}, good_domain = {0}, expiry_domain = {0};
	struct anx_sched_domain_view output, sentinel;
	struct anx_sched_domain_spec spec = { .schema = 1, .cpu_mask = 1,
		.queue_mask = (1U << ANX_QUEUE_CLASS_COUNT) - 1, .maximum_priority = ANX_PRIO_HIGH };
	struct fixture061 *f = anx_zalloc(sizeof(*f));
	if (!f) return ANX_ENOMEM;
	anx_strlcpy(intent.name, "research-day-061", sizeof(intent.name));
	int ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &root);
	if (ret != ANX_OK) goto out;
	root->execution.allow_recursive_cells = true;
	root->execution.allow_side_effects = true;
	ret = -6101;
	if (anx_sched_domain_create(&root->cid, 0, &spec, &domain) != ANX_OK) goto out;
	ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXECUTION, &intent, &bad);
	if (ret == ANX_OK) ret = anx_cell_derive_child(bad, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &grand);
	if (ret == ANX_OK) ret = anx_cell_derive_child(grand, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &leaf);
	if (ret == ANX_OK) ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &good);
	if (ret != ANX_OK) goto out;
	anx_strlcpy(f->call.endpoint, "anxresearch061://domain", sizeof(f->call.endpoint));
	anx_strlcpy(f->side.endpoint, "anxresearch061side://effect", sizeof(f->side.endpoint));
	grand->ext_call = leaf->ext_call = good->ext_call = &f->call;
	ret = anx_external_register_handler("anxresearch061", active061, f);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch061side", side061, f);
	if (ret != ANX_OK) goto out;
	anx_memset(&output, 0x55, sizeof(output)); sentinel = output;
	struct anx_sched_domain_spec proposed = spec;
	proposed.maximum_priority = ANX_PRIO_CRITICAL;
	ret = -6102;
	if (anx_sched_domain_create(&bad->cid, domain.id, &proposed, &output) != ANX_EPERM ||
	    anx_sched_domain_create(&bad->cid, 0, &spec, &output) != ANX_EPERM ||
	    anx_sched_domain_create(&root->cid, 0, &spec, &output) != ANX_EEXIST ||
	    anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	proposed = spec; proposed.cpu_mask = 3;
	if (anx_sched_domain_create(&bad->cid, domain.id, &proposed, &output) != ANX_ENOTSUP) goto out;
	proposed = spec; proposed.queue_mask = 1U << ANX_QUEUE_CLASS_COUNT;
	if (anx_sched_domain_create(&bad->cid, domain.id, &proposed, &output) != ANX_EINVAL) goto out;
	proposed = spec; proposed.queue_mask = (1U << ANX_QUEUE_BACKGROUND) | (1U << ANX_QUEUE_BATCH);
	proposed.maximum_priority = ANX_PRIO_NORMAL;
	ret = anx_sched_domain_create(&bad->cid, domain.id, &proposed, &child);
	if (ret != ANX_OK) goto out;
	proposed.queue_mask = 1U << ANX_QUEUE_INTERACTIVE;
	ret = -6103;
	if (anx_sched_domain_create(&grand->cid, child.id, &proposed, &output) != ANX_EPERM ||
	    anx_sched_domain_create(&grand->cid, domain.id, &spec, &output) != ANX_EPERM ||
	    anx_memcmp(&output, &sentinel, sizeof(output))) goto out;
	proposed.queue_mask = 1U << ANX_QUEUE_BATCH; proposed.maximum_priority = ANX_PRIO_LOW;
	ret = anx_sched_domain_create(&grand->cid, child.id, &proposed, &grand_domain);
	proposed = spec; proposed.queue_mask = 1U << ANX_QUEUE_INTERACTIVE;
	if (ret == ANX_OK) ret = anx_sched_domain_create(&good->cid, domain.id, &proposed, &good_domain);
	if (ret != ANX_OK) goto out;
	/* Editing a public copy cannot widen private queue or priority authority. */
	child.authority = spec;
	ret = -6104;
	if (anx_sched_enqueue(&leaf->cid, ANX_QUEUE_INTERACTIVE, ANX_PRIO_LOW) != ANX_EPERM ||
	    anx_sched_enqueue(&leaf->cid, ANX_QUEUE_BATCH, ANX_PRIO_NORMAL) != ANX_EPERM ||
	    anx_sched_enqueue(&leaf->cid, ANX_QUEUE_BATCH, (enum anx_sched_priority)-1) != ANX_EINVAL ||
	    anx_sched_domain_destroy(domain.id) != ANX_EBUSY || anx_cell_destroy(bad) != ANX_EBUSY) goto out;
	anx_cid_t saved_parent = bad->parent_cid;
	bad->parent_cid = ANX_UUID_NIL;
	int drift = anx_sched_domain_check(leaf);
	bad->parent_cid = saved_parent;
	if (drift != ANX_EPERM) goto out;
	ret = anx_sched_enqueue(&bad->cid, ANX_QUEUE_BATCH, ANX_PRIO_NORMAL);
	if (ret == ANX_OK) ret = anx_sched_enqueue(&grand->cid, ANX_QUEUE_BATCH, ANX_PRIO_LOW);
	if (ret == ANX_OK) ret = anx_sched_enqueue(&leaf->cid, ANX_QUEUE_BATCH, ANX_PRIO_LOW);
	if (ret == ANX_OK) ret = anx_sched_enqueue(&good->cid, ANX_QUEUE_INTERACTIVE, ANX_PRIO_HIGH);
	if (ret != ANX_OK) goto out;
	ret = -6105;
	if (anx_sched_domain_revoke(child.id, child.epoch + 1) != ANX_EBUSY || anx_sched_domain_check(leaf) != ANX_OK) goto out;
	ret = anx_sched_domain_revoke(child.id, child.epoch);
	if (ret != ANX_OK) goto out;
	ret = -6106;
	if (anx_sched_domain_get(grand_domain.id, &output) != ANX_OK || !output.revoked || output.epoch != grand_domain.epoch + 1 ||
	    anx_sched_domain_get(child.id, &output) != ANX_OK || !output.revoked || output.epoch != child.epoch + 1 ||
	    anx_sched_domain_revoke(child.id, child.epoch) != ANX_EBUSY ||
	    anx_sched_domain_check(root) != ANX_OK || anx_sched_domain_check(good) != ANX_OK ||
	    anx_sched_domain_check(bad) != ANX_EPERM || anx_sched_domain_check(grand) != ANX_EPERM ||
	    anx_sched_domain_check(leaf) != ANX_EPERM ||
	    anx_sched_enqueue(&leaf->cid, ANX_QUEUE_BATCH, ANX_PRIO_LOW) != ANX_EPERM) goto out;
	anx_cid_t next = ANX_UUID_NIL;
	if (anx_sched_dequeue(ANX_QUEUE_BATCH, &next) != ANX_ENOENT || !anx_uuid_is_nil(&next) ||
	    anx_sched_queue_depth(ANX_QUEUE_BATCH) || anx_cell_run(leaf) != ANX_EPERM || f->calls || !traced061(leaf)) goto out;
	ret = -6107;
	if (anx_sched_dequeue(ANX_QUEUE_INTERACTIVE, &next) != ANX_OK || anx_uuid_compare(&next, &good->cid)) goto out;
	f->own = good_domain; f->root_id = domain.id;
	ret = anx_cell_run(good);
	if (ret != ANX_OK || f->calls != 1) { ret = -6108; goto out; }
	/* Expiry is checked again inside an already-running callback. */
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &expiry);
	if (ret != ANX_OK) goto out;
	expiry->execution.allow_side_effects = true; expiry->ext_call = &f->call;
	f->expiring = true; f->expiry = arch_time_now() + 250000000ULL;
	proposed = spec; proposed.expires_at = f->expiry;
	ret = anx_sched_domain_create(&expiry->cid, 0, &proposed, &expiry_domain);
	if (ret == ANX_OK) ret = anx_external_operation_prepare(&expiry->cid, &f->side, NULL, NULL, &f->operation);
	if (ret != ANX_OK) goto out;
	f->own = expiry_domain;
	ret = -6109;
	if (anx_cell_run(expiry) != ANX_ETIMEDOUT || f->calls != 2 || f->side_calls || expiry->status != ANX_CELL_FAILED) goto out;
	struct anx_external_operation_view operation;
	if (anx_external_operation_get(&f->operation, &operation) != ANX_OK || operation.phase != ANX_EFFECT_PREPARED) goto out;
	ret = ANX_OK;
out:
	if (ret != ANX_OK) kprintf("day061 failure rc=%d\n", ret);
	anx_external_unregister_handler("anxresearch061"); anx_external_unregister_handler("anxresearch061side");
	if (!anx_uuid_is_nil(&f->operation)) anx_external_operation_discard(&f->operation);
	if (root) anx_cell_cancel(root);
	if (expiry && !anx_cell_status_terminal(expiry->status)) anx_cell_cancel(expiry);
	uint64_t ids[] = {expiry_domain.id, good_domain.id, grand_domain.id, child.id, domain.id};
	for (uint32_t i = 0; i < 5; i++) if (ids[i]) {
		int cleanup = anx_sched_domain_destroy(ids[i]);
		if (ret == ANX_OK && cleanup != ANX_OK) ret = -6114;
	}
	struct anx_cell *cells[] = {expiry, good, leaf, grand, bad, root};
	for (uint32_t i = 0; i < 6; i++) if (cells[i]) {
		int cleanup = anx_cell_destroy(cells[i]);
		if (ret == ANX_OK && cleanup != ANX_OK) ret = -6115;
	}
	anx_free(f);
	return ret;
}
#endif
