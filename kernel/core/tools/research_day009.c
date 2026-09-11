/* Access checks follow the executing cell, including nested dispatch. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/state_object.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct access_test_state {
	anx_oid_t oid;
	struct anx_cell *nested;
	uint32_t writes;
};

static int access_handler(struct anx_external_call *call, void *context)
{
	struct access_test_state *state = context;
	struct anx_object_handle handle = {0};
	int rc;
	if (call->method[0] == 'N' && anx_cell_run(state->nested) != ANX_EPERM)
		return ANX_EIO;
	rc = anx_so_open(&state->oid,
		call->method[0] == 'B' ? ANX_OPEN_READWRITE : ANX_OPEN_WRITE, &handle);
	if (rc != ANX_OK)
		return rc;
	if (call->method[0] == 'R') {
		char data[4];
		rc = anx_so_read_payload(&handle, 0, data, sizeof(data));
	} else {
		rc = anx_so_replace_payload(&handle, "done", 4);
		if (rc == ANX_OK)
			state->writes++;
	}
	anx_so_close(&handle);
	return rc;
}

int anx_research_day009(void)
{
	struct anx_cell *cells[5] = {0};
	struct anx_cell_intent intent = {0};
	struct anx_external_call calls[5] = {0};
	struct anx_state_object *obj = NULL;
	struct anx_so_create_params params = {0};
	struct anx_object_handle handle = {0};
	struct access_test_state state = {0};
	uint64_t version;
	uint32_t i;
	int rc;

	anx_strlcpy(intent.name, "research-day-009", sizeof(intent.name));
	rc = anx_external_register_handler("anxresearch009", access_handler, &state);
	if (rc != ANX_OK)
		return rc;
	for (i = 0; i < 5; i++) {
		rc = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cells[i]);
		if (rc != ANX_OK)
			goto out;
		anx_strlcpy(calls[i].endpoint, "anxresearch009://object", sizeof(calls[i].endpoint));
		cells[i]->execution.allow_side_effects = true;
		cells[i]->ext_call = &calls[i];
	}
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = "keep";
	params.payload_size = 4;
	params.creator_cell = cells[0]->cid;
	rc = anx_so_create(&params, &obj);
	if (rc != ANX_OK)
		goto out;
	state.oid = obj->oid;
	version = obj->version;
	obj->access_policy.rule_count = 2;
	obj->access_policy.rules[0].principal = cells[0]->cid;
	obj->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD | ANX_ACCESS_WRITE_PAYLOAD;
	obj->access_policy.rules[0].effect = ANX_EFFECT_ALLOW;
	obj->access_policy.rules[1].operations = ANX_ACCESS_READ_PAYLOAD | ANX_ACCESS_WRITE_PAYLOAD;
	obj->access_policy.rules[1].effect = ANX_EFFECT_DENY;
	if (anx_cell_run(cells[1]) != ANX_EPERM || state.writes != 0 ||
	    obj->version != version || anx_memcmp(obj->payload, "keep", 4) != 0) {
		rc = -901;
		goto out;
	}
	/* A nested unrelated cell cannot borrow the outer cell's identity. */
	state.nested = cells[2];
	calls[0].method[0] = 'N';
	if (anx_cell_run(cells[0]) != ANX_OK || cells[2]->status != ANX_CELL_FAILED ||
	    state.writes != 1 || obj->version != version + 1 || anx_memcmp(obj->payload, "done", 4) != 0) {
		rc = -902;
		goto out;
	}
	if (anx_so_open(&state.oid, ANX_OPEN_WRITE, &handle) != ANX_EPERM) {
		rc = -903;
		goto out;
	}
	/* Write permission never confers read permission. */
	obj->access_policy.rules[0].principal = ANX_UUID_NIL;
	obj->access_policy.rules[0].operations = ANX_ACCESS_WRITE_PAYLOAD;
	obj->access_policy.rules[1].operations = ANX_ACCESS_READ_PAYLOAD;
	calls[3].method[0] = 'B';
	calls[4].method[0] = 'R';
	if (anx_cell_run(cells[3]) != ANX_EPERM || anx_cell_run(cells[4]) != ANX_EPERM || state.writes != 1) {
		rc = -904;
		goto out;
	}
	rc = ANX_OK;
out:
	if (handle.obj)
		anx_so_close(&handle);
	if (obj) {
		anx_so_delete(&obj->oid, false);
		anx_objstore_release(obj);
	}
	for (i = 0; i < 5; i++)
		if (cells[i])
			anx_cell_destroy(cells[i]);
	anx_external_unregister_handler("anxresearch009");
	return rc;
}
#endif
