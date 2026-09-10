/* Child tool scopes preserve parent limits and return slots on destruction. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/string.h>

struct scope_handler_state {
	uint32_t calls;
	int result;
};

static int scope_handler(struct anx_external_call *call, void *context)
{
	struct scope_handler_state *state = context;
	(void)call;
	state->calls++;
	return state->result;
}

int anx_research_day006(void)
{
	struct anx_cell *parent = NULL, *child = NULL, *extra = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call call = {0};
	struct scope_handler_state state = {0};
	uint32_t i;
	int rc;

	anx_strlcpy(intent.name, "research-day-006", sizeof(intent.name));
	anx_strlcpy(call.endpoint, "anxresearch006://tool", sizeof(call.endpoint));
	rc = anx_external_register_handler("anxresearch006", scope_handler, &state);
	if (rc != ANX_OK)
		return rc;
	rc = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &parent);
	if (rc != ANX_OK)
		goto out;
	if (anx_cell_derive_child(parent, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child) != ANX_EPERM || child) {
		rc = -601;
		goto out;
	}
	parent->execution.allow_recursive_cells = true;
	parent->execution.allow_side_effects = true;
	parent->constraints.max_child_cells = 1;
	parent->constraints.max_latency_ms = 10000;
	parent->constraints.max_cost_usd_cents = 10;
	anx_cell_set_cognitive_envelope(parent, 64, 2);

	for (i = 0; i < 7; i++) {
		rc = anx_cell_derive_child(parent, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child);
		if (rc != ANX_OK)
			goto out;
		child->ext_call = &call;
		if (child->cognitive.max_tokens != 64 || child->cognitive.max_reasoning_depth != 2 ||
		    parent->child_count != 1) {
			rc = -602;
			goto out;
		}
		if (anx_cell_derive_child(parent, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &extra) != ANX_ENOMEM || extra) {
			rc = -603;
			goto out;
		}
		if (anx_cell_destroy(parent) != ANX_EBUSY) {
			parent = NULL;
			rc = -604;
			goto out;
		}
		switch (i) {
		case 0: child->execution.allow_network = true; break;
		case 1: child->execution.allow_remote_models = true; break;
		case 2: child->cognitive.max_tokens = 0; break;
		case 3: child->cognitive.max_reasoning_depth = 3; break;
		case 4: child->constraints.max_latency_ms = 10001; break;
		case 5: child->constraints.max_cost_usd_cents = 0; break;
		case 6: parent->execution.allow_side_effects = false; break;
		}
		if (anx_cell_run(child) != ANX_EPERM || child->status != ANX_CELL_FAILED || state.calls != 0) {
			rc = -605 - (int)i;
			goto out;
		}
		parent->execution.allow_side_effects = true;
		if (anx_cell_destroy(child) != ANX_OK) {
			rc = -612;
			goto out;
		}
		child = NULL;
		if (parent->child_count != 0) {
			rc = -613;
			goto out;
		}
	}
	/* A failing handler also leaves the parent slot reusable after cleanup. */
	for (i = 0; i < 2; i++) {
		rc = anx_cell_derive_child(parent, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child);
		if (rc != ANX_OK)
			goto out;
		child->ext_call = &call;
		state.result = i == 0 ? ANX_EIO : ANX_OK;
		if (anx_cell_run(child) != state.result || state.calls != i + 1) {
			rc = -614;
			goto out;
		}
		rc = anx_cell_destroy(child);
		if (rc != ANX_OK)
			goto out;
		child = NULL;
		if (parent->child_count != 0) {
			rc = -615;
			goto out;
		}
	}
	rc = ANX_OK;
out:
	if (extra)
		anx_cell_destroy(extra);
	if (child)
		anx_cell_destroy(child);
	if (parent)
		anx_cell_destroy(parent);
	anx_external_unregister_handler("anxresearch006");
	return rc;
}
#endif
