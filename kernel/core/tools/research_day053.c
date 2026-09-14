/* Unsupported consistency guarantees fail before effects; object staging is explicit. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/cell.h>
#include <anx/cell_trace.h>
#include <anx/external_call.h>
#include <anx/state_object.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct contract_context { uint32_t calls, mode; struct anx_cell *target; };
static int contract_call(struct anx_external_call *call, void *arg)
{
	struct contract_context *c = arg;
	c->calls++;
	if (!c->mode) return ANX_OK;
	if (c->calls != 1) return -5308;
	struct anx_cell *active = anx_cell_store_lookup(anx_cell_current_id());
	if (!active) return ANX_ENOENT;
	int ret = -5308;
	if (anx_cell_set_contract(active, ANX_CONSISTENCY_BEST_EFFORT, ANX_EFFECT_DIRECT) != ANX_EPERM ||
	    anx_cell_set_contract(c->target, ANX_CONSISTENCY_TRANSACTIONAL, ANX_EFFECT_STAGED) != ANX_EPERM) goto out;
	/* Deliberate trusted-fixture drift exercises the second effect boundary. */
	active->contract.consistency = ANX_CONSISTENCY_TOKEN_STABLE;
	if (anx_external_invoke(call) != ANX_ENOTSUP || c->calls != 1) goto out;
	ret = ANX_OK;
out:
	if (c->mode == 1) active->contract.consistency = ANX_CONSISTENCY_BEST_EFFORT;
	anx_cell_store_release(active);
	return ret;
}

static bool denied_trace(struct anx_cell *cell)
{
	struct anx_state_object *obj = anx_objstore_lookup(&cell->trace_oid);
	bool valid = false;
	if (!obj) return false;
	if (obj->payload_size == sizeof(struct anx_cell_trace)) {
		const struct anx_cell_trace *trace = obj->payload;
		valid = trace->finalized && trace->denied_gate == ANX_ADMISSION_EXECUTION_CONTRACT && !trace->tool.attempted;
		for (uint32_t i = 0; i < trace->event_count; i++)
			if (trace->events[i].type == ANX_TRACE_COMMITTED) valid = false;
	}
	anx_objstore_release(obj);
	return valid;
}

int anx_research_day053(void)
{
	struct anx_cell *cell = NULL, *owner = NULL;
	struct anx_external_call *call = NULL;
	struct anx_cell_intent intent = {0};
	struct contract_context context = {0};
	struct anx_state_object *obj = NULL;
	struct anx_object_handle write = {0}, read = {0};
	struct anx_so_create_params params = {0};
	char bytes[8];
	int ret;
	anx_strlcpy(intent.name, "research-day-053", sizeof(intent.name));
	call = anx_zalloc(sizeof(*call));
	if (!call) return ANX_ENOMEM;
	anx_strlcpy(call->endpoint, "anxresearch053://contract", sizeof(call->endpoint));
	ret = anx_external_register_handler("anxresearch053", contract_call, &context);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
	if (ret != ANX_OK) goto out;
	cell->ext_call = call; cell->execution.allow_side_effects = true;
	ret = anx_cell_set_contract(cell, ANX_CONSISTENCY_TRANSACTIONAL, ANX_EFFECT_STAGED);
	if (ret != ANX_OK) goto out;
	ret = -5301;
	if (anx_cell_run(cell) != ANX_ENOTSUP || context.calls || cell->status != ANX_CELL_FAILED) goto out;
	if (!denied_trace(cell)) { ret = -5302; goto out; }
	anx_cell_destroy(cell); cell = NULL;
	for (uint32_t consistency = 0; consistency <= ANX_CONSISTENCY_TOKEN_STABLE; consistency++) {
		for (uint32_t effect = 0; effect <= ANX_EFFECT_STAGED; effect++) {
			ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
			if (ret != ANX_OK) goto out;
			cell->ext_call = call; cell->execution.allow_side_effects = true;
			ret = anx_cell_set_contract(cell, consistency, effect);
			if (ret != ANX_OK) goto out;
			bool supported = consistency == ANX_CONSISTENCY_BEST_EFFORT && effect == ANX_EFFECT_DIRECT;
			uint32_t before = context.calls;
			ret = -5303;
			if (anx_cell_run(cell) != (supported ? ANX_OK : ANX_ENOTSUP) ||
			    context.calls != before + (supported ? 1U : 0U) ||
			    (!supported && !denied_trace(cell)) || cell->contract.consistency != (enum anx_consistency_class)consistency ||
			    cell->contract.effect_mode != (enum anx_effect_mode)effect) goto out;
			anx_cell_destroy(cell); cell = NULL;
		}
	}
	ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret != ANX_OK) goto out;
	context.target = owner;
	ret = -5304;
	if (anx_cell_set_contract(owner, (enum anx_consistency_class)-1, ANX_EFFECT_DIRECT) != ANX_EINVAL ||
	    anx_cell_set_contract(owner, ANX_CONSISTENCY_BEST_EFFORT, (enum anx_effect_mode)2) != ANX_EINVAL ||
	    owner->contract.consistency != ANX_CONSISTENCY_BEST_EFFORT || owner->contract.effect_mode != ANX_EFFECT_DIRECT) goto out;
	/* Admission validates public fields even if a controller bypassed the setter. */
	ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
	if (ret != ANX_OK) goto out;
	cell->ext_call = call; cell->execution.allow_side_effects = true;
	cell->contract.consistency = (enum anx_consistency_class)99;
	uint32_t before_calls = context.calls;
	ret = -5305;
	if (anx_cell_run(cell) != ANX_EINVAL || context.calls != before_calls || !denied_trace(cell)) goto out;
	anx_cell_destroy(cell); cell = NULL;
	for (uint32_t mode = 1; mode <= 2; mode++) {
		ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &cell);
		if (ret != ANX_OK) goto out;
		context.mode = mode; context.calls = 0;
		cell->ext_call = call; cell->execution.allow_side_effects = true;
		ret = -5306;
		if (anx_cell_run(cell) != (mode == 1 ? ANX_OK : ANX_EPERM) || context.calls != 1 ||
		    owner->contract.consistency != ANX_CONSISTENCY_BEST_EFFORT ||
		    (mode == 2 && cell->status != ANX_CELL_FAILED)) goto out;
		anx_cell_destroy(cell); cell = NULL;
	}
	/* Explicit single-object staging is supported independently of runtime claims. */
	owner->execution.allow_side_effects = true;
	params.object_type = ANX_OBJ_BYTE_DATA; params.payload = "original"; params.payload_size = 8;
	ret = anx_so_create(&params, &obj);
	if (ret == ANX_OK) ret = anx_so_open(&obj->oid, ANX_OPEN_READWRITE, &write);
	if (ret == ANX_OK) ret = anx_so_open(&obj->oid, ANX_OPEN_READ, &read);
	if (ret == ANX_OK) ret = anx_object_stage(&write, owner->cid);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&write, "proposed", 8);
	if (ret != ANX_OK) goto out;
	uint64_t version = obj->version;
	uint32_t provenance = anx_prov_log_count(obj->provenance);
	ret = -5309;
	if (anx_so_read_payload(&read, 0, bytes, 8) != 8 || anx_memcmp(bytes, "original", 8) ||
	    anx_object_stage(&write, owner->cid) != ANX_EBUSY) goto out;
	obj->version++; /* Model a conflicting base revision under trusted fixture control. */
	if (anx_object_commit(&write) != ANX_EBUSY || obj->version != version + 1 ||
	    anx_so_read_payload(&read, 0, bytes, 8) != 8 || anx_memcmp(bytes, "original", 8)) goto out;
	ret = anx_object_abort(&write);
	if (ret != ANX_OK) goto out;
	ret = -5310;
	const struct anx_prov_event *event = anx_prov_log_get(obj->provenance, provenance);
	if (obj->staged || anx_prov_log_count(obj->provenance) != provenance + 1 || !event ||
	    event->event_type != ANX_PROV_STAGE_ABORTED || anx_uuid_compare(&event->actor_cell, &owner->cid)) goto out;
	ret = anx_object_stage(&write, owner->cid);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&write, "accepted", 8);
	if (ret == ANX_OK) ret = anx_object_commit(&write);
	if (ret != ANX_OK) goto out;
	ret = -5311;
	if (obj->staged || obj->version != version + 2 || anx_so_read_payload(&read, 0, bytes, 8) != 8 ||
	    anx_memcmp(bytes, "accepted", 8)) goto out;
	ret = ANX_OK;
out:
	anx_external_unregister_handler("anxresearch053");
	if (obj && obj->staged) anx_object_abort(&write);
	if (read.obj) anx_so_close(&read);
	if (write.obj) anx_so_close(&write);
	if (obj) { anx_oid_t id = obj->oid; anx_objstore_release(obj); anx_so_delete(&id, false); }
	if (owner) anx_cell_destroy(owner);
	if (cell) anx_cell_destroy(cell);
	anx_free(call);
	return ret;
}
#endif
