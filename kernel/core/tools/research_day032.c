/* A shared lifetime creation budget survives child destruction and branch changes. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/effect_fence.h>
#include <anx/effect.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct lease_context { struct anx_cell *foreign; struct anx_effect_fence_view fence; };

static int count_cell(struct anx_cell *cell, void *arg)
{
	(void)cell;
	(*(uint32_t *)arg)++;
	return ANX_OK;
}

static int lease_handler(struct anx_external_call *call, void *arg)
{
	struct lease_context *context = arg;
	struct anx_cell *child = NULL;
	struct anx_cell_intent intent = {0};
	(void)call;
	if (anx_effect_fence_set_execution_lease(&context->fence.id, context->fence.generation, 10, 0) != ANX_EPERM ||
	    anx_cell_derive_child(context->foreign, ANX_CELL_TASK_EXECUTION, &intent, &child) != ANX_EPERM || child) {
		if (child) anx_cell_destroy(child);
		return -3205;
	}
	return ANX_OK;
}

int anx_research_day032(void)
{
	struct anx_effect_fence_view fence, current;
	struct anx_cell *root = NULL, *other = NULL, *child = NULL, *grandchild = NULL, *caller = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_pending_effect *prepared = NULL, *inflight = NULL;
	struct anx_external_call *call = NULL;
	struct lease_context context;
	uint32_t before = 0, after = 0;
	anx_time_t deadline;
	int ret = anx_effect_fence_create(&fence);
	if (ret != ANX_OK) return ret;
	anx_strlcpy(intent.name, "research-day-032", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &root);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &other);
	if (ret != ANX_OK) goto out;
	root->execution.allow_recursive_cells = other->execution.allow_recursive_cells = true;
	root->execution.allow_side_effects = other->execution.allow_side_effects = true;
	ret = anx_effect_fence_bind(root, &fence.id);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(other, &fence.id);
	if (ret == ANX_OK) ret = anx_effect_fence_set_execution_lease(&fence.id, fence.generation, 2, 0);
	if (ret == ANX_OK) ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXECUTION, &intent, &child);
	if (ret == ANX_OK) ret = anx_cell_derive_child(child, ANX_CELL_TASK_EXECUTION, &intent, &grandchild);
	if (ret != ANX_OK) goto out;
	ret = anx_cell_destroy(grandchild);
	if (ret != ANX_OK) goto out;
	grandchild = NULL;
	ret = anx_cell_destroy(child);
	if (ret != ANX_OK) goto out;
	child = NULL;
	anx_cell_store_iterate(count_cell, &before);
	ret = -3201;
	if (anx_cell_derive_child(other, ANX_CELL_TASK_EXECUTION, &intent, &child) != ANX_EFULL || child)
		goto out;
	anx_cell_store_iterate(count_cell, &after);
	ret = -3202;
	if (after != before || other->child_count || root->child_count ||
	    anx_effect_fence_get(&fence.id, &current) != ANX_OK || current.child_creations != 2 || current.child_limit != 2)
		goto out;
	if (anx_effect_fence_set_execution_lease(&fence.id, current.generation, 1, 0) != ANX_EINVAL ||
	    anx_effect_fence_set_execution_lease(&fence.id, fence.generation, 3, 0) != ANX_EBUSY)
		goto out;
	ret = anx_effect_fence_set_execution_lease(&fence.id, current.generation, 3, 0);
	if (ret != ANX_OK) goto out;
	ret = -3203;
	if (anx_cell_derive_child(other, ANX_CELL_TYPE_COUNT, &intent, &child) != ANX_EINVAL || child ||
	    anx_effect_fence_get(&fence.id, &current) != ANX_OK || current.child_creations != 2)
		goto out;
	ret = anx_cell_derive_child(other, ANX_CELL_TASK_EXECUTION, &intent, &child);
	if (ret != ANX_OK) goto out;
	ret = anx_cell_destroy(child);
	if (ret != ANX_OK) goto out;
	child = NULL;
	ret = anx_effect_fence_get(&fence.id, &current);
	if (ret != ANX_OK) goto out;
	context.foreign = other;
	context.fence = current;
	ret = anx_external_register_handler("anxresearch032", lease_handler, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch032://expand", sizeof(call->endpoint));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (ret != ANX_OK) goto out;
	caller->execution.allow_side_effects = true;
	caller->ext_call = call;
	ret = anx_cell_run(caller);
	if (ret == ANX_OK) ret = anx_effect_prepare(other->cid, NULL, NULL, &prepared);
	if (ret == ANX_OK) ret = anx_effect_prepare(other->cid, NULL, NULL, &inflight);
	if (ret == ANX_OK) ret = anx_effect_mark_dispatching(inflight);
	if (ret != ANX_OK) goto out;
	deadline = arch_time_now() + 10000000ULL;
	ret = anx_effect_fence_set_execution_lease(&fence.id, current.generation, 4, deadline);
	if (ret != ANX_OK) goto out;
	while (arch_time_now() < deadline) { }
	ret = -3204;
	if (anx_cell_derive_child(other, ANX_CELL_TASK_EXECUTION, &intent, &child) != ANX_EPERM || child ||
	    anx_effect_mark_dispatching(prepared) != ANX_EPERM || prepared->phase != ANX_EFFECT_PREPARED ||
	    anx_effect_fence_get(&fence.id, &current) != ANX_OK || current.state != ANX_FENCE_TIMED_OUT ||
	    current.child_creations != 3 || current.epoch <= fence.epoch ||
	    anx_effect_fence_set_execution_lease(&fence.id, current.generation, 5, 0) != ANX_EPERM ||
	    anx_effect_commit(inflight) != ANX_OK)
		goto out;
	ret = ANX_OK;
out:
	if (grandchild) anx_cell_destroy(grandchild);
	if (child) anx_cell_destroy(child);
	if (root) anx_cell_destroy(root);
	if (other) anx_cell_destroy(other);
	if (caller) anx_cell_destroy(caller);
	if (call) anx_free(call);
	if (prepared) anx_effect_destroy(prepared);
	if (inflight) anx_effect_destroy(inflight);
	anx_external_unregister_handler("anxresearch032");
	return ret;
}
#endif
