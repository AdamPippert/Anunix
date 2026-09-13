/* Observation admission rejects obsolete state without deleting historical bytes. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/observation.h>
#include <anx/cell.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct observation_context { anx_oid_t current, old; struct anx_observation_spec spec; bool foreign; };
static int observe_handler(struct anx_external_call *call, void *arg)
{
	struct observation_context *c = arg;
	char bytes[32] = {0};
	anx_oid_t denied = ANX_UUID_NIL;
	(void)call;
	if (c->foreign) return anx_observation_read(&c->current, 0, bytes, sizeof(bytes)) == ANX_EPERM ? ANX_OK : -4107;
	if (anx_observation_read(&c->current, 0, bytes, sizeof(bytes)) != 8 || anx_strcmp(bytes, "state-v2") ||
	    anx_observation_read(&c->old, 0, bytes, sizeof(bytes)) != ANX_EBUSY ||
	    anx_observation_publish(&c->spec, &denied) != ANX_EPERM || !anx_uuid_is_nil(&denied) ||
	    anx_observation_supersede(&c->old, &c->current) != ANX_EPERM ||
	    anx_observation_release(&c->current) != ANX_EPERM) return -4106;
	return ANX_OK;
}

int anx_research_day041(void)
{
	struct anx_state_object *objects[5] = {0};
	const char *payloads[] = {"state-v1", "visual-v1", "state-v2", "tool-read", "other"};
	struct anx_so_create_params p = {0};
	struct anx_cell_intent intent = {0};
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_external_call *call = NULL;
	struct anx_object_handle h = {0};
	struct anx_observation_spec spec = {0};
	struct anx_observation_view view;
	struct observation_context context = {0};
	anx_oid_t records[5] = {0};
	char bytes[32] = {0};
	int ret;
	for (uint32_t i = 0; i < 5; i++) {
		p.object_type = i == 3 ? ANX_OBJ_EXECUTION_TRACE : ANX_OBJ_BYTE_DATA;
		p.payload = payloads[i]; p.payload_size = anx_strlen(payloads[i]);
		ret = anx_so_create(&p, &objects[i]);
		if (ret == ANX_OK && i != 0 && i != 4) ret = anx_so_seal(&objects[i]->oid);
		if (ret != ANX_OK) goto out;
	}
	anx_strlcpy(intent.name, "research-day-041", sizeof(intent.name));
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret != ANX_OK) goto out;
	spec.owner = owner->cid; spec.subject = objects[0]->oid;
	spec.content = objects[1]->oid; spec.provenance = objects[3]->oid;
	spec.kind = ANX_OBSERVATION_VISUAL; spec.full_coverage = true;
	ret = anx_observation_publish(&spec, &records[0]);
	if (ret != ANX_OK) goto out;
	ret = -4100;
	if (anx_observation_read(&records[0], 0, bytes, sizeof(bytes)) != 9 || anx_strcmp(bytes, "visual-v1")) goto out;
	ret = anx_so_open(&objects[0]->oid, ANX_OPEN_READWRITE, &h);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&h, "state-v2", 8);
	anx_so_close(&h);
	if (ret != ANX_OK) goto out;
	anx_memset(bytes, 'X', sizeof(bytes));
	ret = -4101;
	if (anx_observation_read(&records[0], 0, bytes, sizeof(bytes)) != ANX_EBUSY || bytes[0] != 'X') goto out;
	struct anx_observation_spec bad = spec;
	anx_oid_t denied = ANX_UUID_NIL;
	bad.provenance = objects[1]->oid;
	ret = -4109;
	if (anx_observation_publish(&bad, &denied) != ANX_EPERM || !anx_uuid_is_nil(&denied)) goto out;
	bad = spec; bad.content = objects[4]->oid;
	if (anx_observation_publish(&bad, &denied) != ANX_EPERM || !anx_uuid_is_nil(&denied)) goto out;
	objects[0]->sensitivity = ANX_SENSITIVITY_CONFIDENTIAL;
	if (anx_observation_publish(&spec, &denied) != ANX_EPERM || !anx_uuid_is_nil(&denied)) goto out;
	objects[0]->sensitivity = ANX_SENSITIVITY_PUBLIC;
	ret = anx_observation_publish(&spec, &records[1]);
	spec.kind = ANX_OBSERVATION_STRUCTURED; spec.content = objects[2]->oid; spec.full_coverage = false;
	if (ret == ANX_OK) ret = anx_observation_publish(&spec, &records[2]);
	if (ret != ANX_OK) goto out;
	ret = -4102;
	if (anx_observation_supersede(&records[1], &records[2]) != ANX_EPERM) goto out;
	spec.full_coverage = true;
	ret = anx_observation_publish(&spec, &records[3]);
	context.spec = spec; context.current = records[3]; context.old = records[1];
	spec.subject = objects[4]->oid;
	if (ret == ANX_OK) ret = anx_observation_publish(&spec, &records[4]);
	if (ret != ANX_OK) goto out;
	ret = -4103;
	if (anx_observation_supersede(&records[1], &records[4]) != ANX_EPERM) goto out;
	ret = anx_observation_supersede(&records[1], &records[3]);
	if (ret != ANX_OK) goto out;
	ret = -4104;
	if (anx_observation_describe(&records[1], &view) != ANX_OK ||
	    anx_uuid_compare(&view.superseded_by, &records[3]) ||
	    anx_observation_read(&records[1], 0, bytes, sizeof(bytes)) != ANX_EBUSY) goto out;
	objects[0]->access_policy.rule_count = 1;
	objects[0]->access_policy.rules[0].operations = ANX_ACCESS_READ_PAYLOAD;
	objects[0]->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	if (anx_observation_read(&records[3], 0, bytes, sizeof(bytes)) != ANX_EPERM) goto out;
	objects[0]->access_policy.rule_count = 0;
	ret = anx_so_open(&objects[1]->oid, ANX_OPEN_READ, &h);
	if (ret != ANX_OK) goto out;
	ret = -4105;
	if (anx_so_read_payload(&h, 0, bytes, 9) != 9 || anx_memcmp(bytes, "visual-v1", 9)) goto out;
	anx_so_close(&h);
	ret = anx_external_register_handler("anxresearch041", observe_handler, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch041://observe", sizeof(call->endpoint));
	owner->ext_call = foreign->ext_call = call;
	owner->execution.allow_side_effects = foreign->execution.allow_side_effects = true;
	ret = anx_cell_run(owner);
	context.foreign = true;
	if (ret == ANX_OK) ret = anx_cell_run(foreign);
	if (ret == ANX_OK) ret = anx_so_delete(&objects[3]->oid, false);
	if (ret != ANX_OK) goto out;
	ret = -4108;
	if (anx_observation_read(&records[3], 0, bytes, sizeof(bytes)) != ANX_ENOENT) goto out;
	ret = ANX_OK;
out:
	anx_so_close(&h);
	for (uint32_t i = 0; i < 5; i++) if (!anx_uuid_is_nil(&records[i])) anx_observation_release(&records[i]);
	if (owner) anx_cell_destroy(owner);
	if (foreign) anx_cell_destroy(foreign);
	if (call) anx_free(call);
	anx_external_unregister_handler("anxresearch041");
	for (uint32_t i = 0; i < 5; i++) if (objects[i]) {
		anx_so_delete(&objects[i]->oid, false); anx_objstore_release(objects[i]);
	}
	return ret;
}
#endif
