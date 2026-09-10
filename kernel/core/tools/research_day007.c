/* Invocation accounting survives both failure and child scope cleanup. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/cell_trace.h>
#include <anx/external_call.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct accounting_state {
	struct anx_cell *parent;
	uint32_t calls;
	int result;
};

static int accounting_handler(struct anx_external_call *call, void *context)
{
	struct accounting_state *state = context;
	if (state->parent->child_count != 1)
		return ANX_EBUSY;
	state->calls++;
	call->response_buf[0] = 'o';
	call->response_buf[1] = 'k';
	call->response_size = 2;
	call->status_code = state->result == ANX_OK ? 200 : 503;
	return state->result;
}

int anx_research_day007(void)
{
	struct anx_cell *parent = NULL, *child = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call call = {0};
	struct anx_cell_trace *trace = NULL;
	struct anx_state_object *obj = NULL;
	struct accounting_state state = {0};
	anx_oid_t saved;
	anx_cid_t cid;
	uint32_t i;
	int rc;

	anx_strlcpy(intent.name, "research-day-007", sizeof(intent.name));
	anx_strlcpy(call.endpoint, "anxresearch007://tool", sizeof(call.endpoint));
	call.request_body = "ping";
	call.request_size = 4;
	rc = anx_external_register_handler("anxresearch007", accounting_handler, &state);
	if (rc != ANX_OK)
		return rc;
	trace = anx_alloc(sizeof(*trace));
	if (!trace) {
		rc = ANX_ENOMEM;
		goto out;
	}
	rc = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &parent);
	if (rc != ANX_OK)
		goto out;
	state.parent = parent;
	parent->execution.allow_recursive_cells = true;
	parent->constraints.max_child_cells = 1;
	for (i = 0; i < 3; i++) {
		parent->execution.allow_side_effects = i != 0;
		state.result = i == 1 ? ANX_EIO : ANX_OK;
		rc = anx_cell_derive_child(parent, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child);
		if (rc != ANX_OK)
			goto out;
		child->ext_call = &call;
		if (anx_cell_run(child) != (i == 0 ? ANX_EPERM : state.result) || state.calls != i) {
			rc = -700;
			goto out;
		}
		if (anx_uuid_is_nil(&child->trace_oid)) {
			rc = -701;
			goto out;
		}
		saved = child->trace_oid;
		cid = child->cid;
		rc = anx_cell_destroy(child);
		if (rc != ANX_OK)
			goto out;
		child = NULL;
		if (parent->child_count != 0) {
			rc = -702;
			goto out;
		}
		obj = anx_objstore_lookup(&saved);
		if (!obj || anx_so_read_payload(obj, trace, sizeof(*trace), 0) != (int)sizeof(*trace) ||
		    anx_uuid_compare(&trace->cell_ref, &cid) != 0 || !trace->finalized) {
			rc = -703;
			goto out;
		}
		if (trace->tool.attempted != (i != 0) || (i != 0 &&
		    (trace->tool.started_at == 0 || trace->tool.completed_at < trace->tool.started_at ||
		     trace->tool.request_bytes != 4 || trace->tool.response_bytes != 2 ||
		     trace->tool.transport_result != state.result ||
		     trace->tool.status_code != (i == 1 ? 503 : 200)))) {
			rc = -704;
			goto out;
		}
		anx_objstore_release(obj);
		obj = NULL;
	}
	rc = ANX_OK;
out:
	if (obj)
		anx_objstore_release(obj);
	if (child)
		anx_cell_destroy(child);
	if (parent)
		anx_cell_destroy(parent);
	if (trace)
		anx_free(trace);
	anx_external_unregister_handler("anxresearch007");
	return rc;
}
#endif
