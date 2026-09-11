/*
 * trace.c — Execution Cell Trace recording.
 *
 * Append-only event log for cell execution. On finalization,
 * the trace is materialized as an execution_trace State Object.
 */

#include <anx/types.h>
#include <anx/cell_trace.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/uuid.h>
#include <anx/string.h>
#include <anx/arch.h>

#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
static bool fail_reservation, fail_finalize;
void anx_trace_test_fail_reservation(bool fail) { fail_reservation = fail; }
void anx_trace_test_fail_finalize(bool fail) { fail_finalize = fail; }
#endif

int anx_trace_create(const anx_cid_t *cell_ref, struct anx_cell_trace **out)
{
	struct anx_cell_trace *trace;

	if (!cell_ref || !out)
		return ANX_EINVAL;

	trace = anx_zalloc(sizeof(*trace));
	if (!trace)
		return ANX_ENOMEM;

	anx_uuid_generate(&trace->trace_id);
	trace->cell_ref = *cell_ref;
	trace->started_at = arch_time_now();

	*out = trace;
	return ANX_OK;
}

int anx_trace_append(struct anx_cell_trace *trace,
		     enum anx_trace_event_type type,
		     const char *description,
		     int status_code)
{
	struct anx_trace_event *event;

	if (!trace)
		return ANX_EINVAL;
	if (trace->finalized)
		return ANX_EPERM;
	if (trace->event_count >= ANX_MAX_TRACE_EVENTS)
		return ANX_ENOMEM;

	event = &trace->events[trace->event_count];
	event->event_index = trace->event_count;
	event->type = type;
	event->timestamp = arch_time_now();
	event->status_code = status_code;
	if (description)
		anx_strlcpy(event->description, description,
			     sizeof(event->description));

	trace->event_count++;
	return ANX_OK;
}

int anx_trace_prepare(struct anx_cell_trace *trace)
{
	struct anx_state_object *obj;
	struct anx_so_create_params params = {0};
	int ret;

	if (!trace)
		return ANX_EINVAL;
	if (trace->finalized)
		return ANX_EPERM;
	if (!anx_uuid_is_nil(&trace->storage_oid))
		return ANX_OK;
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
	if (fail_reservation) {
		fail_reservation = false;
		return ANX_ENOMEM;
	}
#endif
	params.object_type = ANX_OBJ_EXECUTION_TRACE;
	params.schema_uri = ANX_CELL_TRACE_SCHEMA;
	params.schema_version = ANX_CELL_TRACE_SCHEMA_VERSION;
	params.payload = trace;
	params.payload_size = sizeof(*trace);
	params.creator_cell = trace->cell_ref;

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK)
		return ret;
	trace->storage_oid = obj->oid;
	anx_objstore_release(obj);
	return ANX_OK;
}

int anx_trace_finalize(struct anx_cell_trace *trace, anx_oid_t *trace_oid_out)
{
	struct anx_object_handle handle = {0};
	int ret;

	if (trace_oid_out)
		*trace_oid_out = ANX_UUID_NIL;
	if (!trace)
		return ANX_EINVAL;
	if (trace->finalized)
		return ANX_EPERM;
	ret = anx_trace_prepare(trace);
	if (ret != ANX_OK)
		return ret;
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
	if (fail_finalize) {
		fail_finalize = false;
		return ANX_EIO;
	}
#endif
	ret = anx_so_open(&trace->storage_oid, ANX_OPEN_WRITE, &handle);
	if (ret != ANX_OK)
		return ret;
	trace->completed_at = arch_time_now();
	trace->finalized = true;
	ret = anx_so_write_payload(&handle, 0, trace, sizeof(*trace));
	anx_so_close(&handle);
	if (ret != (int)sizeof(*trace)) {
		ret = ret < 0 ? ret : ANX_EIO;
		goto fail;
	}
	ret = anx_so_seal(&trace->storage_oid);
	if (ret != ANX_OK)
		goto fail;
	if (trace_oid_out)
		*trace_oid_out = trace->storage_oid;
	return ANX_OK;
fail:
	trace->finalized = false;
	trace->completed_at = 0;
	return ret;
}

void anx_trace_destroy(struct anx_cell_trace *trace)
{
	if (trace)
		anx_free(trace);
}
