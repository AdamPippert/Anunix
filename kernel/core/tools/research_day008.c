/* A child trace preserves its lineage after cells leave the registry. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/cell_trace.h>
#include <anx/external_call.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

static int lineage_handler(struct anx_external_call *call, void *context)
{
	uint32_t *calls = context;
	(void)call;
	(*calls)++;
	return ANX_OK;
}

int anx_research_day008(void)
{
	struct anx_cell *parent = NULL, *child = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_external_call call = {0};
	struct anx_object_handle handle = {0};
	struct anx_cell_trace *trace = NULL;
	anx_cid_t parent_id, child_id;
	anx_pid_t plan_id;
	anx_oid_t saved;
	uint32_t i, calls = 0;
	int rc;

	anx_strlcpy(intent.name, "research-day-008", sizeof(intent.name));
	anx_strlcpy(call.endpoint, "anxresearch008://tool", sizeof(call.endpoint));
	rc = anx_external_register_handler("anxresearch008", lineage_handler, &calls);
	if (rc != ANX_OK)
		return rc;
	trace = anx_alloc(sizeof(*trace));
	if (!trace) {
		rc = ANX_ENOMEM;
		goto out;
	}
	for (i = 0; i < 2; i++) {
		rc = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &parent);
		if (rc != ANX_OK)
			goto out;
		parent->execution.allow_recursive_cells = true;
		parent->execution.allow_side_effects = i != 0;
		rc = anx_cell_derive_child(parent, ANX_CELL_TASK_EXTERNAL_CALL, &intent, &child);
		if (rc != ANX_OK)
			goto out;
		child->ext_call = &call;
		if (anx_cell_run(child) != (i == 0 ? ANX_EPERM : ANX_OK) || calls != i) {
			rc = -800;
			goto out;
		}
		saved = child->trace_oid;
		child_id = child->cid;
		parent_id = parent->cid;
		plan_id = child->plan_id;
		rc = anx_cell_destroy(child);
		if (rc != ANX_OK)
			goto out;
		child = NULL;
		rc = anx_cell_destroy(parent);
		if (rc != ANX_OK)
			goto out;
		parent = NULL;
		rc = anx_so_open(&saved, ANX_OPEN_READ, &handle);
		if (rc != ANX_OK)
			goto out;
		if (handle.obj->state != ANX_OBJ_SEALED ||
		    anx_strcmp(handle.obj->schema_uri, ANX_CELL_TRACE_SCHEMA) != 0 ||
		    anx_strcmp(handle.obj->schema_version, ANX_CELL_TRACE_SCHEMA_VERSION) != 0) {
			rc = -801;
			goto out;
		}
		if (anx_so_read_payload(&handle, 0, trace, sizeof(*trace)) != (int)sizeof(*trace) ||
		    anx_uuid_compare(&trace->cell_ref, &child_id) != 0 ||
		    anx_uuid_compare(&trace->parent_cell_ref, &parent_id) != 0 ||
		    anx_uuid_compare(&trace->plan_ref, &plan_id) != 0 ||
		    anx_uuid_compare(&handle.obj->creator_cell, &child_id) != 0 ||
		    anx_uuid_is_nil(&trace->plan_ref) != (i == 0)) {
			rc = -802;
			goto out;
		}
		anx_so_close(&handle);
		if (anx_so_open(&saved, ANX_OPEN_WRITE, &handle) != ANX_EPERM) {
			rc = -803;
			goto out;
		}
	}
	rc = ANX_OK;
out:
	if (handle.obj)
		anx_so_close(&handle);
	if (child)
		anx_cell_destroy(child);
	if (parent)
		anx_cell_destroy(parent);
	if (trace)
		anx_free(trace);
	anx_external_unregister_handler("anxresearch008");
	return rc;
}
#endif
