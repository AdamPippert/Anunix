/* Retained intent names connect allowed and denied calls to their cells. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/cell_trace.h>
#include <anx/external_call.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct causal_state {
	struct anx_cell *cell;
	anx_cid_t caller;
	uint32_t calls;
};

static int causal_handler(struct anx_external_call *call, void *context)
{
	struct causal_state *state = context;
	const anx_cid_t *caller = anx_cell_current_id();
	if (!caller)
		return ANX_EIO;
	state->caller = *caller;
	state->calls++;
	/* The original name must already be copied into the audit record. */
	anx_strlcpy(state->cell->intent.name, "changed-after-dispatch",
		    sizeof(state->cell->intent.name));
	call->response_buf[0] = 'x';
	call->response_size = 1;
	call->status_code = 201;
	return ANX_OK;
}

int anx_research_day014(void)
{
	const char *names[] = {"denied-effect", "allowed-effect", "cancelled-effect"};
	struct anx_cell *cell = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call call = {0};
	struct anx_cell_trace *trace = NULL;
	struct anx_object_handle handle = {0};
	struct causal_state state = {0};
	anx_oid_t saved[3];
	anx_cid_t cid;
	uint32_t i, j;
	int rc;

	anx_strlcpy(call.endpoint, "anxresearch014://effect", sizeof(call.endpoint));
	rc = anx_external_register_handler("anxresearch014", causal_handler, &state);
	if (rc != ANX_OK)
		return rc;
	trace = anx_alloc(sizeof(*trace));
	if (!trace) {
		rc = ANX_ENOMEM;
		goto out;
	}
	for (i = 0; i < 3; i++) {
		bool named = false, denied = false, committed = false, cancelled = false;
		anx_strlcpy(intent.name, names[i], sizeof(intent.name));
		rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
		if (rc != ANX_OK)
			goto out;
		state.cell = cell;
		cell->ext_call = &call;
		cell->execution.allow_side_effects = i != 0;
		rc = -1400;
		if ((i == 2 ? anx_cell_cancel(cell) : anx_cell_run(cell)) != (i == 0 ? ANX_EPERM : ANX_OK) ||
		    state.calls != (i == 0 ? 0 : 1))
			goto out;
		cid = cell->cid;
		saved[i] = cell->trace_oid;
		if (anx_cell_destroy(cell) != ANX_OK)
			goto out;
		cell = NULL;
		rc = -1401;
		if (anx_so_open(&saved[i], ANX_OPEN_READ, &handle) != ANX_OK ||
		    anx_so_read_payload(&handle, 0, trace, sizeof(*trace)) != (int)sizeof(*trace) ||
		    handle.obj->state != ANX_OBJ_SEALED || !trace->finalized ||
		    anx_uuid_compare(&trace->cell_ref, &cid) != 0 ||
		    anx_uuid_compare(&handle.obj->creator_cell, &cid) != 0)
			goto out;
		for (j = 0; j < trace->event_count; j++) {
			const struct anx_trace_event *e = &trace->events[j];
			if (e->type == ANX_TRACE_CREATED && anx_strcmp(e->description, names[i]) == 0)
				named = true;
			denied |= e->type == ANX_TRACE_ADMISSION_DENIED;
			committed |= e->type == ANX_TRACE_COMMITTED;
			cancelled |= e->type == ANX_TRACE_CANCELLED;
		}
		if (!named)
			goto out;
		rc = -1402;
		if (denied != (i == 0) || committed != (i == 1) || cancelled != (i == 2) ||
		    trace->tool.attempted != (i == 1) || (i == 1 &&
		    (anx_uuid_compare(&state.caller, &cid) != 0 || trace->tool.response_bytes != 1 ||
		     trace->tool.transport_result != ANX_OK || trace->tool.status_code != 201)))
			goto out;
		anx_so_close(&handle);
	}
	rc = -1403;
	if (anx_uuid_compare(&saved[0], &saved[1]) == 0 ||
	    anx_uuid_compare(&saved[1], &saved[2]) == 0)
		goto out;
	rc = ANX_OK;
out:
	if (handle.obj)
		anx_so_close(&handle);
	if (cell)
		anx_cell_destroy(cell);
	anx_free(trace);
	anx_external_unregister_handler("anxresearch014");
	return rc;
}
#endif
