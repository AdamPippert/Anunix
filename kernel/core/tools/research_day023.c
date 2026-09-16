/* Observe, authorize, focus, and verify against the native Interface Plane. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/a11y.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/input.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct action_context {
	struct anx_a11y_observation observation;
	enum anx_a11y_action action;
	int expected;
	bool deny_effects;
	bool checked;
};

static int action_handler(struct anx_external_call *call, void *arg)
{
	struct action_context *context = arg;
	struct anx_a11y_receipt receipt, saved;
	struct anx_cell *caller = anx_cell_store_lookup(anx_cell_current_id());
	anx_oid_t before = anx_input_focus_get(), after;
	int ret;
	(void)call;
	if (!caller)
		return ANX_ENOENT;
	anx_memset(&receipt, 0x5a, sizeof(receipt));
	saved = receipt;
	if (context->deny_effects)
		caller->execution.allow_side_effects = false;
	ret = anx_a11y_action_checked(context->observation.node.id, context->observation.generation,
				      context->action, &receipt);
	caller->execution.allow_side_effects = true;
	anx_cell_store_release(caller);
	after = anx_input_focus_get();
	if (ret != context->expected)
		return -2302;
	if (ret == ANX_OK) {
		if (!receipt.verified_focus ||
		    receipt.observation_generation != context->observation.generation ||
		    receipt.resulting_generation <= receipt.observation_generation ||
		    anx_uuid_compare(&receipt.focused_surface, &context->observation.node.surf_oid) ||
		    anx_uuid_compare(&after, &receipt.focused_surface))
			return -2303;
		saved = receipt;
		if (anx_a11y_action_checked(context->observation.node.id, context->observation.generation,
					    context->action, &receipt) != ANX_EBUSY ||
		    anx_memcmp(&saved, &receipt, sizeof(receipt)))
			return -2304;
	} else if (anx_memcmp(&saved, &receipt, sizeof(receipt)) || anx_uuid_compare(&before, &after)) {
		return -2305;
	}
	if (anx_a11y_action(context->observation.node.id, ANX_A11Y_ACTION_FOCUS) != ANX_EPERM ||
	    anx_a11y_node_update(&context->observation.node) != ANX_EPERM ||
	    anx_a11y_node_remove(context->observation.node.id) != ANX_EPERM)
		return -2306;
	context->checked = true;
	return ANX_OK;
}

static int run_action(struct anx_a11y_node *node, enum anx_a11y_action action,
		      bool granted, bool stale, bool deny_effects, int expected)
{
	struct anx_cell_intent intent = {0};
	struct anx_cell *caller = NULL;
	struct anx_external_call *call = NULL;
	struct action_context context = {0};
	int ret;
	anx_strlcpy(intent.name, "research-day-023-action", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (ret != ANX_OK)
		goto out;
	node->action_principal = granted ? caller->cid : ANX_UUID_NIL;
	ret = anx_a11y_node_update(node);
	if (ret != ANX_OK)
		goto out;
	ret = anx_a11y_observe(node->id, &context.observation);
	if (ret != ANX_OK)
		goto out;
	if (stale) {
		/* The same integer node ID now refers to a different observation. */
		ret = anx_a11y_node_remove(node->id);
		if (ret == ANX_OK)
			ret = anx_a11y_node_add(node);
		if (ret != ANX_OK)
			goto out;
	}
	context.action = action;
	context.expected = expected;
	context.deny_effects = deny_effects;
	ret = anx_external_register_handler("anxresearch023", action_handler, &context);
	if (ret != ANX_OK)
		goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call)
		goto out;
	anx_strlcpy(call->endpoint, "anxresearch023://focus", sizeof(call->endpoint));
	caller->ext_call = call;
	caller->execution.allow_side_effects = true;
	ret = anx_cell_run(caller);
	if (ret == ANX_OK && !context.checked)
		ret = -2307;
out:
	anx_external_unregister_handler("anxresearch023");
	if (caller)
		anx_cell_destroy(caller);
	if (call)
		anx_free(call);
	return ret;
}

int anx_research_day023(void)
{
	struct anx_surface *surface = NULL;
	struct anx_a11y_node node = {0}, existing;
	struct anx_a11y_observation observation;
	anx_oid_t previous_focus = anx_input_focus_get();
	static uint32_t next_id = 23000;
	bool added = false;
	int ret;
	while (anx_a11y_node_get(++next_id, &existing) == ANX_OK)
		;
	node.id = next_id;
	node.role = ANX_A11Y_ROLE_INPUT;
	node.visible = node.enabled = node.focusable = true;
	anx_strlcpy(node.name, "Research focus target", sizeof(node.name));
	ret = anx_iface_surface_create(ANX_ENGINE_RENDERER_HEADLESS, NULL, 0, 0, 100, 100, &surface);
	if (ret != ANX_OK)
		goto out;
	ret = anx_iface_surface_map(surface);
	if (ret != ANX_OK)
		goto out;
	node.surf_oid = surface->oid;
	ret = anx_a11y_node_add(&node);
	if (ret != ANX_OK)
		goto out;
	added = true;
	ret = -2301;
	if (anx_a11y_observe(node.id, &observation) != ANX_OK || !observation.generation)
		goto out;
	ret = run_action(&node, ANX_A11Y_ACTION_FOCUS, true, true, false, ANX_EBUSY);
	if (ret != ANX_OK)
		goto out;
	ret = run_action(&node, ANX_A11Y_ACTION_FOCUS, false, false, false, ANX_EPERM);
	if (ret != ANX_OK)
		goto out;
	ret = run_action(&node, ANX_A11Y_ACTION_FOCUS, true, false, true, ANX_EPERM);
	if (ret != ANX_OK)
		goto out;
	node.enabled = false;
	ret = run_action(&node, ANX_A11Y_ACTION_FOCUS, true, false, false, ANX_EPERM);
	if (ret != ANX_OK)
		goto out;
	node.enabled = true;
	node.visible = false;
	ret = run_action(&node, ANX_A11Y_ACTION_FOCUS, true, false, false, ANX_EPERM);
	if (ret != ANX_OK)
		goto out;
	node.visible = true;
	node.focusable = false;
	ret = run_action(&node, ANX_A11Y_ACTION_FOCUS, true, false, false, ANX_EPERM);
	if (ret != ANX_OK)
		goto out;
	node.focusable = true;
	ret = run_action(&node, (enum anx_a11y_action)99, true, false, false, ANX_EINVAL);
	if (ret != ANX_OK)
		goto out;
	ret = run_action(&node, ANX_A11Y_ACTION_CLICK, true, false, false, ANX_ENOTSUP);
	if (ret != ANX_OK)
		goto out;
	ret = run_action(&node, ANX_A11Y_ACTION_FOCUS, true, false, false, ANX_OK);
	if (ret != ANX_OK)
		goto out;
	anx_iface_surface_destroy(surface);
	surface = NULL;
	ret = run_action(&node, ANX_A11Y_ACTION_FOCUS, true, false, false, ANX_ENOENT);
out:
	if (added)
		anx_a11y_node_remove(node.id);
	if (surface)
		anx_iface_surface_destroy(surface);
	anx_input_focus_set(previous_focus);
	return ret;
}
#endif
