/* Bounded discovery and invocation consult the same current namespace grants. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/tool_namespace.h>
#include <anx/cell.h>
#include <anx/capability.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct namespace_context { struct anx_tool_namespace_ref *namespace; uint32_t calls; };

static int namespace_handler(struct anx_external_call *call, void *arg)
{
	struct namespace_context *context = arg;
	struct anx_tool_namespace_ref output;
	context->calls++;
	if (anx_tool_namespace_replace(context->namespace, NULL, 0, &output) != ANX_EPERM ||
	    anx_external_unregister_handler("anxresearch025") != ANX_EPERM ||
	    anx_external_register_handler("anxresearch025", namespace_handler, context) != ANX_EPERM)
		return -2502;
	call->response_buf[0] = 'o';
	call->response_buf[1] = 'k';
	call->response_size = 2;
	call->status_code = 200;
	return ANX_OK;
}

static int run_request(struct anx_cell *root, const struct anx_tool_catalog_entry *entry,
		       int mutation, int expected, struct namespace_context *context)
{
	struct anx_cell_intent intent = {0};
	struct anx_cell *child = NULL;
	struct anx_external_call *call = anx_zalloc(sizeof(*call));
	uint32_t before = context->calls;
	int ret = ANX_ENOMEM;
	if (!call)
		return ret;
	anx_strlcpy(intent.name, "research-day-025-tool", sizeof(intent.name));
	ret = anx_cell_derive_child(root, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child);
	if (ret != ANX_OK)
		goto out;
	call->tool_handle = entry->handle;
	anx_strlcpy(call->endpoint, entry->descriptor.endpoint, sizeof(call->endpoint));
	anx_strlcpy(call->method, entry->descriptor.method, sizeof(call->method));
	if (mutation == 1)
		anx_strlcpy(call->endpoint, "anxresearch025://different", sizeof(call->endpoint));
	if (mutation == 2)
		anx_strlcpy(call->method, "DELETE", sizeof(call->method));
	if (mutation == 3)
		anx_memset(&call->tool_handle, 0, sizeof(call->tool_handle));
	if (mutation == 4)
		anx_uuid_generate(&call->tool_handle.namespace.id);
	child->ext_call = call;
	ret = anx_cell_run(child);
	if (ret != expected) {
		ret = -2503;
		goto out;
	}
	if (expected == ANX_OK) {
		if (context->calls != before + 1 || call->response_size != 2 || call->status_code != 200 ||
		    anx_memcmp(call->response_buf, "ok", 2))
			ret = -2504;
	} else if (context->calls != before) {
		ret = -2505;
	} else {
		ret = ANX_OK;
	}
out:
	if (child)
		anx_cell_destroy(child);
	anx_free(call);
	return ret;
}

int anx_research_day025(void)
{
	struct anx_tool_descriptor descriptor = {0}, network_descriptor;
	struct anx_tool_ref tools[4] = {0};
	struct anx_tool_grant grants[3] = {0};
	struct anx_tool_namespace_ref namespace, saved_namespace;
	struct anx_tool_catalog_entry *entries = NULL, saved, forged;
	struct anx_cell_intent intent = {0};
	struct anx_cell *root = NULL;
	struct namespace_context context = {0};
	uint32_t registered = 0, count = 0;
	int ret = -2501;
	anx_strlcpy(descriptor.name, "research.inspect", sizeof(descriptor.name));
	anx_strlcpy(descriptor.endpoint, "anxresearch025://inspect", sizeof(descriptor.endpoint));
	anx_strlcpy(descriptor.method, "GET", sizeof(descriptor.method));
	anx_strlcpy(descriptor.schema_version, "1", sizeof(descriptor.schema_version));
	descriptor.required_authority = ANX_CAP_AUTH_SIDE_EFFECT;
	if (anx_tool_register(&descriptor, &tools[0]) != ANX_OK)
		goto out;
	registered++;
	for (uint32_t i = 1; i < 4; i++) {
		anx_snprintf(descriptor.name, sizeof(descriptor.name), "research.tool%u", i);
		descriptor.required_authority = i == 3 ? ANX_CAP_AUTH_NETWORK : 0;
		ret = anx_tool_register(&descriptor, &tools[i]);
		if (ret != ANX_OK)
			goto out;
		registered++;
	}
	network_descriptor = descriptor;
	anx_strlcpy(intent.name, "research-day-025-namespace", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &root);
	if (ret != ANX_OK)
		goto out;
	root->execution.allow_side_effects = true;
	root->execution.allow_network = false;
	root->execution.allow_recursive_cells = true;
	root->execution.max_recursion_depth = root->constraints.max_recursion_depth = 3;
	root->constraints.max_child_cells = 2;
	grants[0] = (struct anx_tool_grant){tools[0].id, ANX_TOOL_DISCOVER | ANX_TOOL_INVOKE};
	grants[1] = (struct anx_tool_grant){tools[1].id, ANX_TOOL_DISCOVER};
	grants[2] = (struct anx_tool_grant){tools[3].id, ANX_TOOL_DISCOVER | ANX_TOOL_INVOKE};
	ret = -2506;
	if (anx_tool_namespace_create(root, grants, ANX_TOOL_WORKING_SET_MAX + 1, &namespace) != ANX_EINVAL ||
	    !anx_uuid_is_nil(&root->tool_namespace_id) ||
	    anx_tool_namespace_create(root, grants, 3, &namespace) != ANX_OK)
		goto out;
	context.namespace = &namespace;
	ret = anx_external_register_handler("anxresearch025", namespace_handler, &context);
	if (ret != ANX_OK)
		goto out;
	entries = anx_alloc(sizeof(*entries) * ANX_TOOL_WORKING_SET_MAX);
	ret = ANX_ENOMEM;
	if (!entries)
		goto out;
	anx_memset(entries, 0x5a, sizeof(*entries) * ANX_TOOL_WORKING_SET_MAX);
	count = 77;
	ret = -2507;
	if (anx_tool_discover(root, entries, 0, &count) != ANX_EINVAL || count != 77 ||
	    anx_tool_discover(root, entries, 1, &count) != ANX_OK || count != 1 ||
	    entries[1].descriptor.name[0] != 0x5a ||
	    anx_tool_discover(root, entries, ANX_TOOL_WORKING_SET_MAX, &count) != ANX_OK || count != 2 ||
	    anx_uuid_compare(&entries[0].handle.tool.id, &tools[0].id) ||
	    anx_uuid_compare(&entries[1].handle.tool.id, &tools[1].id))
		goto out;
	saved = entries[0];
	ret = run_request(root, &entries[0], 0, ANX_OK, &context);
	if (ret != ANX_OK)
		goto out;
	for (int mutation = 1; mutation <= 4; mutation++) {
		ret = run_request(root, &entries[0], mutation, ANX_EPERM, &context);
		if (ret != ANX_OK)
			goto out;
	}
	ret = run_request(root, &entries[1], 0, ANX_EPERM, &context);
	if (ret != ANX_OK)
		goto out;
	forged = entries[0];
	forged.handle.tool = tools[2];
	ret = run_request(root, &forged, 0, ANX_EPERM, &context);
	if (ret != ANX_OK)
		goto out;
	forged.handle.tool = tools[3];
	forged.descriptor = network_descriptor;
	ret = run_request(root, &forged, 0, ANX_EPERM, &context);
	if (ret != ANX_OK)
		goto out;
	descriptor = saved.descriptor;
	anx_strlcpy(descriptor.schema_version, "2", sizeof(descriptor.schema_version));
	ret = anx_tool_update(&tools[0], &descriptor, &tools[0]);
	if (ret != ANX_OK)
		goto out;
	ret = run_request(root, &saved, 0, ANX_EBUSY, &context);
	if (ret != ANX_OK)
		goto out;
	ret = anx_tool_discover(root, entries, ANX_TOOL_WORKING_SET_MAX, &count);
	if (ret != ANX_OK)
		goto out;
	saved = entries[0];
	grants[0].rights = ANX_TOOL_DISCOVER;
	saved_namespace = namespace;
	ret = anx_tool_namespace_replace(&namespace, grants, 3, &namespace);
	if (ret != ANX_OK)
		goto out;
	ret = run_request(root, &saved, 0, ANX_EBUSY, &context);
	if (ret != ANX_OK)
		goto out;
	ret = anx_tool_discover(root, entries, ANX_TOOL_WORKING_SET_MAX, &count);
	if (ret != ANX_OK)
		goto out;
	ret = run_request(root, &entries[0], 0, ANX_EPERM, &context);
	if (ret != ANX_OK)
		goto out;
	grants[0].rights |= ANX_TOOL_INVOKE;
	ret = -2508;
	if (anx_tool_namespace_replace(&saved_namespace, grants, 3, &saved_namespace) != ANX_EBUSY ||
	    anx_tool_namespace_replace(&namespace, grants, 3, &namespace) != ANX_OK ||
	    anx_tool_discover(root, entries, ANX_TOOL_WORKING_SET_MAX, &count) != ANX_OK)
		goto out;
	ret = run_request(root, &entries[0], 0, ANX_OK, &context);
out:
	if (entries) anx_free(entries);
	if (root) anx_cell_destroy(root);
	for (uint32_t i = 0; i < registered; i++)
		anx_tool_remove(&tools[i]);
	anx_external_unregister_handler("anxresearch025");
	return ret;
}
#endif
