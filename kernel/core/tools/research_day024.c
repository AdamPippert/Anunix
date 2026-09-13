/* A branch hold fences sibling effects without stopping unrelated computation. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/effect_fence.h>
#include <anx/effect.h>
#include <anx/cell.h>
#include <anx/cell_trace.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct fence_context {
	struct anx_pending_effect *sibling;
	bool hold;
	uint32_t calls;
};

static int fence_handler(struct anx_external_call *call, void *arg)
{
	struct fence_context *context = arg;
	struct anx_cell *caller = anx_cell_store_lookup(anx_cell_current_id());
	struct anx_effect_fence_view fence;
	struct anx_pending_effect *attempt = NULL;
	int ret = ANX_OK;
	(void)call;
	context->calls++;
	if (!caller)
		return ANX_ENOENT;
	if (context->hold) {
		ret = -2402;
		if (anx_effect_fence_hold(caller) != ANX_OK ||
		    anx_effect_fence_get(&caller->effect_fence_id, &fence) != ANX_OK ||
		    fence.state != ANX_FENCE_HELD ||
		    anx_effect_fence_transition(&fence.id, fence.generation, ANX_FENCE_RUNNING) != ANX_EPERM ||
		    anx_effect_prepare(caller->cid, NULL, NULL, &attempt) != ANX_EBUSY || attempt)
			goto out;
		if (anx_external_invoke(call) != ANX_EBUSY || context->calls != 1)
			goto out;
		/* The active branch cannot dispatch a different cell's prepared effect. */
		if (anx_effect_mark_dispatching(context->sibling) != ANX_EPERM)
			goto out;
		ret = ANX_OK;
	}
out:
	if (attempt)
		anx_effect_destroy(attempt);
	anx_cell_store_release(caller);
	return ret;
}

static int make_cell(struct anx_cell **out, struct anx_external_call *call)
{
	struct anx_cell_intent intent = {0};
	int ret;
	anx_strlcpy(intent.name, "research-day-024-run", sizeof(intent.name));
	ret = anx_cell_create(call ? ANX_CELL_TASK_EXTERNAL_CALL : ANX_CELL_TASK_EXECUTION, &intent, out);
	if (ret == ANX_OK) {
		(*out)->ext_call = call;
		(*out)->execution.allow_side_effects = true;
		(*out)->execution.allow_recursive_cells = true;
		(*out)->execution.max_recursion_depth = (*out)->constraints.max_recursion_depth = 3;
		(*out)->constraints.max_child_cells = 4;
	}
	return ret;
}

static int check_denial(struct anx_cell *cell)
{
	struct anx_cell_trace *trace = anx_zalloc(sizeof(*trace));
	struct anx_object_handle handle = {0};
	int ret = ANX_ENOMEM;
	if (!trace)
		return ret;
	ret = anx_so_open(&cell->trace_oid, ANX_OPEN_READ, &handle);
	if (ret == ANX_OK && (anx_so_read_payload(&handle, 0, trace, sizeof(*trace)) != (int)sizeof(*trace) ||
	    trace->denied_gate != ANX_ADMISSION_EFFECT_FENCE))
		ret = -2403;
	if (handle.obj)
		anx_so_close(&handle);
	anx_free(trace);
	return ret;
}

int anx_research_day024(void)
{
	struct anx_effect_fence_view fence, held, current, rejected, timeout;
	struct anx_cell *root = NULL, *a = NULL, *b = NULL, *c = NULL, *compute = NULL;
	struct anx_cell *unrelated = NULL, *fresh = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_pending_effect *prepared = NULL, *late = NULL, *inflight = NULL;
	struct anx_external_call *call = NULL;
	struct fence_context context = {0};
	int ret = -2401;
	if (anx_effect_fence_create(&fence) != ANX_OK || fence.state != ANX_FENCE_RUNNING || fence.epoch != 1)
		goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call)
		goto out;
	anx_strlcpy(call->endpoint, "anxresearch024://effect", sizeof(call->endpoint));
	ret = make_cell(&root, NULL);
	if (ret != ANX_OK)
		goto out;
	ret = anx_effect_fence_bind(root, &fence.id);
	if (ret != ANX_OK)
		goto out;
	anx_strlcpy(intent.name, "research-day-024-branch", sizeof(intent.name));
	ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &a);
	if (ret == ANX_OK)
		ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &b);
	if (ret == ANX_OK)
		ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &c);
	if (ret != ANX_OK)
		goto out;
	a->ext_call = b->ext_call = c->ext_call = call;
	ret = -2404;
	if (anx_uuid_compare(&a->effect_fence_id, &fence.id) ||
	    anx_uuid_compare(&b->effect_fence_id, &fence.id) ||
	    anx_effect_prepare(b->cid, NULL, NULL, &prepared) != ANX_OK ||
	    anx_uuid_compare(&prepared->fence_id, &fence.id) || prepared->fence_epoch != fence.epoch)
		goto out;
	context.sibling = prepared;
	context.hold = true;
	ret = anx_external_register_handler("anxresearch024", fence_handler, &context);
	if (ret != ANX_OK)
		goto out;
	ret = anx_cell_run(a);
	if (ret != ANX_OK)
		goto out;
	ret = -2405;
	if (context.calls != 1 || anx_effect_fence_get(&fence.id, &held) != ANX_OK ||
	    held.state != ANX_FENCE_HELD || anx_effect_mark_dispatching(prepared) != ANX_EBUSY ||
	    prepared->phase != ANX_EFFECT_PREPARED || anx_cell_run(c) != ANX_EBUSY || context.calls != 1)
		goto out;
	ret = check_denial(c);
	if (ret != ANX_OK)
		goto out;
	ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXECUTION, &intent, &compute);
	if (ret == ANX_OK)
		ret = anx_cell_run(compute);
	if (ret != ANX_OK)
		goto out;
	context.hold = false;
	ret = make_cell(&unrelated, call);
	if (ret == ANX_OK)
		ret = anx_cell_run(unrelated);
	if (ret != ANX_OK)
		goto out;
	ret = -2406;
	if (context.calls != 2 ||
	    anx_effect_fence_transition(&fence.id, fence.generation, ANX_FENCE_RUNNING) != ANX_EBUSY ||
	    anx_effect_fence_transition(&fence.id, held.generation, ANX_FENCE_RUNNING) != ANX_OK ||
	    anx_effect_mark_dispatching(prepared) != ANX_OK || anx_effect_commit(prepared) != ANX_OK ||
	    anx_effect_mark_dispatching(prepared) != ANX_EINVAL)
		goto out;
	ret = anx_effect_prepare(b->cid, NULL, NULL, &late);
	if (ret == ANX_OK)
		ret = anx_effect_prepare(b->cid, NULL, NULL, &inflight);
	if (ret == ANX_OK)
		ret = anx_effect_mark_dispatching(inflight);
	if (ret != ANX_OK)
		goto out;
	ret = -2407;
	if (anx_cell_cancel(root) != ANX_OK || anx_effect_fence_get(&fence.id, &current) != ANX_OK ||
	    current.state != ANX_FENCE_CANCELLED || current.epoch <= fence.epoch ||
	    anx_effect_mark_dispatching(late) != ANX_EPERM || late->phase != ANX_EFFECT_PREPARED ||
	    anx_effect_mark_unknown(inflight) != ANX_OK ||
	    anx_effect_fence_transition(&fence.id, current.generation, ANX_FENCE_RUNNING) != ANX_EPERM)
		goto out;
	ret = make_cell(&fresh, call);
	if (ret != ANX_OK)
		goto out;
	ret = -2408;
	if (anx_effect_fence_bind(fresh, &fence.id) != ANX_OK ||
	    anx_cell_run(fresh) != ANX_EPERM || context.calls != 2)
		goto out;
	ret = anx_effect_fence_create(&rejected);
	if (ret != ANX_OK)
		goto out;
	ret = -2409;
	if (anx_effect_fence_transition(&rejected.id, rejected.generation, ANX_FENCE_HELD) != ANX_OK ||
	    anx_effect_fence_get(&rejected.id, &current) != ANX_OK ||
	    anx_effect_fence_transition(&rejected.id, current.generation, ANX_FENCE_REJECTED) != ANX_OK ||
	    anx_effect_fence_get(&rejected.id, &current) != ANX_OK ||
	    anx_effect_fence_transition(&rejected.id, current.generation, ANX_FENCE_RUNNING) != ANX_EPERM)
		goto out;
	ret = anx_effect_fence_create(&timeout);
	if (ret != ANX_OK)
		goto out;
	ret = -2410;
	if (anx_effect_fence_transition(&timeout.id, timeout.generation, ANX_FENCE_TIMED_OUT) != ANX_OK ||
	    anx_effect_fence_get(&timeout.id, &current) != ANX_OK ||
	    current.epoch <= timeout.epoch ||
	    anx_effect_fence_transition(&timeout.id, current.generation, ANX_FENCE_RUNNING) != ANX_EPERM)
		goto out;
	ret = ANX_OK;
out:
	if (prepared) anx_effect_destroy(prepared);
	if (late) anx_effect_destroy(late);
	if (inflight) anx_effect_destroy(inflight);
	if (a) anx_cell_destroy(a);
	if (b) anx_cell_destroy(b);
	if (c) anx_cell_destroy(c);
	if (compute) anx_cell_destroy(compute);
	if (root) anx_cell_destroy(root);
	if (unrelated) anx_cell_destroy(unrelated);
	if (fresh) anx_cell_destroy(fresh);
	if (call) anx_free(call);
	anx_external_unregister_handler("anxresearch024");
	return ret;
}
#endif
