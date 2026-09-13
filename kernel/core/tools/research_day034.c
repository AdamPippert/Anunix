/* Private preparation does not authorize publication or a protected effect. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/state_object.h>
#include <anx/cell.h>
#include <anx/effect_fence.h>
#include <anx/effect.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct preparation_context {
	struct anx_object_handle write;
	struct anx_cell *owner;
	uint32_t mode, calls;
};

static int preparation_handler(struct anx_external_call *call, void *arg)
{
	struct preparation_context *c = arg;
	struct anx_state_object *obj = c->write.obj;
	struct anx_cell *caller = anx_cell_store_lookup(anx_cell_current_id());
	struct anx_pending_effect *effect = NULL;
	uint64_t version = obj->version;
	struct anx_hash hash = obj->content_hash;
	uint32_t provenance = anx_prov_log_count(obj->provenance);
	int ret = ANX_EPERM;
	(void)call;
	c->calls++;
	if (!caller) return ANX_ENOENT;
	if (c->mode == 1) {
		ret = -3405;
		if (anx_object_commit(&c->write) != ANX_EPERM || anx_object_abort(&c->write) != ANX_EPERM ||
		    anx_so_replace_payload(&c->write, "intruder", 8) != ANX_EPERM ||
		    anx_so_write_payload(&c->write, 0, "X", 1) != ANX_EPERM) goto out;
		ret = ANX_OK;
		goto out;
	}
	ret = anx_object_stage(&c->write, caller->cid);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&c->write, "prepared", 8);
	if (ret != ANX_OK) goto out;
	caller->execution.allow_side_effects = false;
	ret = -3401;
	if (anx_object_commit(&c->write) != ANX_EPERM ||
	    anx_effect_prepare(caller->cid, NULL, NULL, &effect) != ANX_EPERM || effect) goto out;
	ret = anx_object_abort(&c->write);
	if (ret != ANX_OK) goto out;
	ret = -3402;
	if (obj->version != version || anx_memcmp(obj->payload, "original", 8) ||
	    anx_memcmp(hash.bytes, obj->content_hash.bytes, sizeof(hash.bytes)) ||
	    anx_prov_log_count(obj->provenance) != provenance + 1 || obj->staged) goto out;
	caller->execution.allow_side_effects = true;
	ret = anx_object_stage(&c->write, caller->cid);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&c->write, "prepared", 8);
	if (ret == ANX_OK) ret = anx_effect_fence_hold(caller);
	if (ret != ANX_OK) goto out;
	ret = -3403;
	if (anx_object_commit(&c->write) != ANX_EBUSY ||
	    anx_effect_prepare(caller->cid, NULL, NULL, &effect) != ANX_EBUSY || effect ||
	    anx_external_invoke(call) != ANX_EBUSY || c->calls != 1) goto out;
	ret = anx_object_abort(&c->write);
	if (ret != ANX_OK) goto out;
	ret = -3404;
	if (anx_object_stage(&c->write, c->owner->cid) != ANX_EPERM || obj->staged) goto out;
	ret = ANX_OK;
out:
	caller->execution.allow_side_effects = true;
	anx_cell_store_release(caller);
	if (effect) anx_effect_destroy(effect);
	return ret;
}

int anx_research_day034(void)
{
	struct anx_state_object *obj = NULL;
	struct anx_so_create_params params = {0};
	struct anx_cell *caller = NULL, *other = NULL, *owner = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_effect_fence_view fence, current;
	struct anx_external_call *call = NULL;
	struct anx_object_handle read = {0};
	struct preparation_context c = {0};
	int ret = anx_effect_fence_create(&fence);
	if (ret != ANX_OK) return ret;
	params.object_type = ANX_OBJ_BYTE_DATA;
	params.payload = "original";
	params.payload_size = 8;
	ret = anx_so_create(&params, &obj);
	if (ret == ANX_OK) ret = anx_so_open(&obj->oid, ANX_OPEN_READWRITE, &c.write);
	if (ret == ANX_OK) ret = anx_so_open(&obj->oid, ANX_OPEN_READ, &read);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch034://prepare", sizeof(call->endpoint));
	anx_strlcpy(intent.name, "research-day-034", sizeof(intent.name));
	ret = anx_external_register_handler("anxresearch034", preparation_handler, &c);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &caller);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &other);
	if (ret != ANX_OK) goto out;
	c.owner = owner;
	caller->ext_call = other->ext_call = call;
	caller->execution.allow_side_effects = other->execution.allow_side_effects = true;
	ret = anx_effect_fence_bind(caller, &fence.id);
	if (ret == ANX_OK) ret = anx_cell_run(caller);
	if (ret != ANX_OK) goto out;
	ret = anx_object_stage(&c.write, owner->cid);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&c.write, "prepared", 8);
	if (ret != ANX_OK) goto out;
	c.mode = 1;
	ret = anx_cell_run(other);
	if (ret != ANX_OK) goto out;
	ret = -3406;
	if (anx_object_commit(&read) != ANX_EPERM || anx_object_abort(&read) != ANX_EPERM ||
	    anx_object_commit(&c.write) != ANX_EPERM) goto out;
	owner->execution.allow_side_effects = true;
	obj->access_policy.rule_count = 1;
	obj->access_policy.rules[0].operations = ANX_ACCESS_WRITE_PAYLOAD;
	obj->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	if (anx_object_commit(&c.write) != ANX_EPERM) goto out;
	obj->access_policy.rule_count = 0;
	obj->version++;
	if (anx_object_commit(&c.write) != ANX_EBUSY) goto out;
	ret = anx_object_abort(&c.write);
	if (ret == ANX_OK) ret = anx_object_stage(&c.write, owner->cid);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&c.write, "accepted", 8);
	if (ret == ANX_OK) ret = anx_object_commit(&c.write);
	if (ret != ANX_OK) goto out;
	ret = -3407;
	if (anx_memcmp(obj->payload, "accepted", 8) || obj->staged ||
	    anx_effect_fence_get(&fence.id, &current) != ANX_OK || current.state != ANX_FENCE_HELD) goto out;
	ret = ANX_OK;
out:
	if (obj && obj->staged) anx_object_abort(&c.write);
	if (read.obj) anx_so_close(&read);
	if (c.write.obj) anx_so_close(&c.write);
	if (obj) { anx_oid_t id = obj->oid; anx_objstore_release(obj); anx_so_delete(&id, false); }
	if (owner) anx_cell_destroy(owner);
	if (caller) anx_cell_destroy(caller);
	if (other) anx_cell_destroy(other);
	if (call) anx_free(call);
	anx_external_unregister_handler("anxresearch034");
	return ret;
}
#endif
