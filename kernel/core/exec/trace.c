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

int anx_trace_finalize(struct anx_cell_trace *trace, anx_oid_t *trace_oid_out)
{
	struct anx_state_object *obj;
	struct anx_so_create_params params;
	int ret;

	if (!trace)
		return ANX_EINVAL;
	if (trace->finalized)
		return ANX_EPERM;

	trace->completed_at = arch_time_now();
	trace->finalized = true;

	/*
	 * Materialize the trace as an execution_trace State Object.
	 * The trace struct itself becomes the payload.
	 */
	anx_memset(&params, 0, sizeof(params));
	params.object_type = ANX_OBJ_EXECUTION_TRACE;
	params.payload = trace;
	params.payload_size = sizeof(*trace);
	params.creator_cell = trace->cell_ref;

	ret = anx_so_create(&params, &obj);
	if (ret != ANX_OK)
		return ret;

	if (trace_oid_out)
		*trace_oid_out = obj->oid;

	/* Release the object store's reference — caller gets the OID */
	anx_objstore_release(obj);
	return ANX_OK;
}

void anx_trace_destroy(struct anx_cell_trace *trace)
{
	if (trace)
		anx_free(trace);
}
