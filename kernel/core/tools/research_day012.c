/* Typed denial records and audit reservation precede tool dispatch. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/cell_trace.h>
#include <anx/external_call.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct audit_test_state {
	struct anx_cell *cell;
	uint32_t calls;
};

static int audit_handler(struct anx_external_call *call, void *context)
{
	struct audit_test_state *state = context;
	struct anx_object_handle handle = {0};
	int ret;
	(void)call;
	state->calls++;
	ret = anx_so_open(&state->cell->trace_oid, ANX_OPEN_READ, &handle);
	if (ret != ANX_OK)
		return ANX_EIO;
	ret = handle.obj->state == ANX_OBJ_ACTIVE && handle.obj->payload_size == sizeof(struct anx_cell_trace)
		? ANX_OK : ANX_EIO;
	anx_so_close(&handle);
	return ret;
}

int anx_research_day012(void)
{
	struct anx_cell *cell = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call call = {0};
	struct anx_object_handle handle = {0};
	struct anx_cell_trace *trace = NULL;
	struct audit_test_state state = {0};
	const enum anx_admission_gate gates[] = {ANX_ADMISSION_AUDIT_REQUIRED,
		ANX_ADMISSION_AUTHORITY, ANX_ADMISSION_DESCRIPTOR, ANX_ADMISSION_AUDIT_STORAGE};
	const int results[] = {ANX_EPERM, ANX_EPERM, ANX_EINVAL, ANX_ENOMEM, ANX_OK, ANX_EAUDIT};
	uint32_t i, j;
	int rc;

	anx_strlcpy(intent.name, "research-day-012", sizeof(intent.name));
	anx_strlcpy(call.endpoint, "anxresearch012://audit", sizeof(call.endpoint));
	rc = anx_external_register_handler("anxresearch012", audit_handler, &state);
	if (rc != ANX_OK)
		return rc;
	trace = anx_alloc(sizeof(*trace));
	if (!trace) {
		rc = ANX_ENOMEM;
		goto out;
	}
	for (i = 0; i < 6; i++) {
		bool denied = false;
		rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
		if (rc != ANX_OK)
			goto out;
		state.cell = cell;
		cell->execution.allow_side_effects = i != 1;
		cell->commit.write_trace = i != 0;
		cell->ext_call = i == 2 ? NULL : &call;
		anx_trace_test_fail_reservation(i == 3);
		anx_trace_test_fail_finalize(i == 5);
		if (anx_cell_run(cell) != results[i] || state.calls != (i < 4 ? 0 : i - 3)) {
			rc = -1201;
			goto out;
		}
		if (anx_so_open(&cell->trace_oid, ANX_OPEN_READ, &handle) != ANX_OK ||
		    anx_so_read_payload(&handle, 0, trace, sizeof(*trace)) != (int)sizeof(*trace)) {
			rc = -1202;
			goto out;
		}
		if (i == 5) {
			if (cell->status != ANX_CELL_COMPLETED || cell->error_code != ANX_EAUDIT ||
			    handle.obj->state != ANX_OBJ_ACTIVE || trace->finalized || anx_cell_run(cell) != ANX_EBUSY) {
				rc = -1203;
				goto out;
			}
		} else if (handle.obj->state != ANX_OBJ_SEALED || !trace->finalized ||
		    trace->tool.attempted != (i == 4)) {
			rc = -1204;
			goto out;
		}
		if (i < 4) {
			if (trace->denied_gate != gates[i]) {
				rc = -1205;
				goto out;
			}
			for (j = 0; j < trace->event_count; j++) {
				if (trace->events[j].type == ANX_TRACE_ADMISSION_DENIED && trace->events[j].status_code == results[i])
					denied = true;
				if (trace->events[j].type == ANX_TRACE_COMMITTED || trace->events[j].type == ANX_TRACE_COMPLETED) {
					rc = -1206;
					goto out;
				}
			}
			if (!denied) {
				rc = -1207;
				goto out;
			}
		}
		anx_so_close(&handle);
		anx_cell_destroy(cell);
		cell = NULL;
	}
	rc = ANX_OK;
out:
	anx_trace_test_fail_reservation(false);
	anx_trace_test_fail_finalize(false);
	if (handle.obj)
		anx_so_close(&handle);
	if (cell)
		anx_cell_destroy(cell);
	if (trace)
		anx_free(trace);
	anx_external_unregister_handler("anxresearch012");
	return rc;
}
#endif
