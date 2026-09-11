/* Cancel queue ownership, then reap only terminal, unreferenced tasks. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/sched.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct reaping_state {
	struct anx_cell *cell;
	uint32_t calls;
	bool cancel_self;
};

static int reaping_handler(struct anx_external_call *call, void *context)
{
	struct reaping_state *state = context;
	(void)call;
	state->calls++;
	if (anx_cell_destroy(state->cell) != ANX_EBUSY || anx_cell_run(state->cell) != ANX_EBUSY)
		return ANX_EIO;
	return state->cancel_self ? anx_cell_cancel(state->cell) : ANX_OK;
}

int anx_research_day010(void)
{
	struct anx_cell *parent = NULL, *child = NULL, *held = NULL, *found;
	struct anx_cell_intent intent = {0};
	struct anx_external_call call = {0};
	struct reaping_state state = {0};
	anx_cid_t cid;
	anx_oid_t trace;
	uint32_t interactive, batch, count, i;
	int rc;

	anx_strlcpy(intent.name, "research-day-010", sizeof(intent.name));
	anx_strlcpy(call.endpoint, "anxresearch010://tool", sizeof(call.endpoint));
	rc = anx_external_register_handler("anxresearch010", reaping_handler, &state);
	if (rc != ANX_OK)
		return rc;
	rc = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &parent);
	if (rc != ANX_OK)
		goto out;
	parent->execution.allow_recursive_cells = true;
	parent->execution.allow_side_effects = true;
	rc = anx_cell_derive_child(parent, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child);
	if (rc != ANX_OK)
		goto out;
	child->ext_call = &call;
	cid = child->cid;
	interactive = anx_sched_queue_depth(ANX_QUEUE_INTERACTIVE);
	batch = anx_sched_queue_depth(ANX_QUEUE_BATCH);
	if (anx_sched_enqueue(&cid, ANX_QUEUE_INTERACTIVE, ANX_PRIO_NORMAL) != ANX_OK ||
	    anx_sched_enqueue(&cid, ANX_QUEUE_INTERACTIVE, ANX_PRIO_HIGH) != ANX_OK ||
	    anx_sched_enqueue(&cid, ANX_QUEUE_BATCH, ANX_PRIO_LOW) != ANX_OK) {
		rc = -1000;
		goto out;
	}
	if (anx_cell_cancel(parent) != ANX_OK || child->status != ANX_CELL_CANCELLED ||
	    anx_sched_queue_depth(ANX_QUEUE_INTERACTIVE) != interactive ||
	    anx_sched_queue_depth(ANX_QUEUE_BATCH) != batch || state.calls != 0) {
		rc = -1001;
		goto out;
	}
	trace = child->trace_oid;
	if (anx_uuid_is_nil(&trace) || anx_cell_run(child) != ANX_EBUSY ||
	    anx_uuid_compare(&trace, &child->trace_oid) != 0 ||
	    anx_sched_enqueue(&cid, ANX_QUEUE_BATCH, ANX_PRIO_NORMAL) != ANX_EPERM) {
		rc = -1002;
		goto out;
	}
	held = anx_cell_store_lookup(&cid);
	if (anx_cell_reap_children(parent, &count) != ANX_OK || count != 0) {
		rc = -1003;
		goto out;
	}
	anx_cell_store_release(held);
	held = NULL;
	if (anx_cell_reap_children(parent, &count) != ANX_OK || count != 1 || parent->child_count != 0) {
		rc = -1004;
		goto out;
	}
	child = NULL;
	found = anx_cell_store_lookup(&cid);
	if (found) {
		anx_cell_store_release(found);
		rc = -1005;
		goto out;
	}
	anx_cell_destroy(parent);
	parent = NULL;
	rc = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &parent);
	if (rc != ANX_OK)
		goto out;
	parent->execution.allow_recursive_cells = true;
	parent->execution.allow_side_effects = true;
	for (i = 0; i < 2; i++) {
		rc = anx_cell_derive_child(parent, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child);
		if (rc != ANX_OK)
			goto out;
		state.cell = child;
		state.cancel_self = i != 0;
		child->ext_call = &call;
		if (anx_cell_run(child) != (i == 0 ? ANX_OK : ANX_ECANCELED) || state.calls != i + 1) {
			rc = -1006;
			goto out;
		}
		trace = child->trace_oid;
		if (anx_cell_run(child) != ANX_EBUSY || child->attempt_count != 1 ||
		    anx_uuid_compare(&trace, &child->trace_oid) != 0) {
			rc = -1007;
			goto out;
		}
		if (anx_cell_reap_children(parent, &count) != ANX_OK || count != 1) {
			rc = -1008;
			goto out;
		}
		child = NULL;
	}
	if (anx_cell_cancel(parent) != ANX_OK ||
	    anx_cell_derive_child(parent, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child) != ANX_EPERM || child) {
		rc = -1009;
		goto out;
	}
	rc = ANX_OK;
out:
	if (held)
		anx_cell_store_release(held);
	if (child) {
		while (anx_sched_cancel(&child->cid) == ANX_OK) { }
		anx_cell_destroy(child);
	}
	if (parent)
		anx_cell_destroy(parent);
	anx_external_unregister_handler("anxresearch010");
	return rc;
}
#endif
