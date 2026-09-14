/* Wrong predictions stay private; matched publication still needs current authority. */
#if defined(ANX_RESEARCH_TEST) || defined(ANX_HOST_TEST)
#include <anx/research_test.h>
#include <anx/speculation.h>
#include <anx/cell.h>
#include <anx/effect_fence.h>
#include <anx/external_call.h>
#include <anx/alloc.h>
#include <anx/arch.h>
#include <anx/string.h>
#include <anx/uuid.h>

struct speculation_context { anx_oid_t branch; struct anx_speculation_request request; uint32_t calls; };
static int unused_handler(struct anx_external_call *call, void *arg)
{
	struct speculation_context *c = arg;
	(void)call; c->calls++;
	return ANX_OK;
}
static int foreign_handler(struct anx_external_call *call, void *arg)
{
	struct speculation_context *c = arg;
	struct anx_speculation_view view;
	anx_oid_t denied = ANX_UUID_NIL;
	(void)call;
	return anx_speculation_prepare(&c->request, &denied) == ANX_EPERM && anx_uuid_is_nil(&denied) &&
		anx_speculation_get(&c->branch, &view) == ANX_EPERM &&
		anx_speculation_commit(&c->branch, "replace", 7) == ANX_EPERM &&
		anx_speculation_discard(&c->branch) == ANX_EPERM &&
		anx_speculation_release(&c->branch) == ANX_EPERM ? ANX_OK : -4509;
}
static int prepare(struct anx_speculation_request *request, anx_oid_t *out)
{
	request->expires_at = arch_time_now() + ANX_SPECULATION_LIFETIME_MAX - 1000000000ULL;
	return anx_speculation_prepare(request, out);
}
int anx_research_day045(void)
{
	struct anx_so_create_params p = { .object_type = ANX_OBJ_BYTE_DATA, .payload = "original", .payload_size = 8 };
	struct anx_state_object *obj = NULL;
	struct anx_cell *owner = NULL, *foreign = NULL;
	struct anx_cell_intent intent = {0};
	struct anx_object_handle h = {0};
	struct anx_speculation_request request = { .action = "replace", .action_size = 7, .result = "predicted", .result_size = 9 };
	struct anx_speculation_view view;
	struct speculation_context context = {0};
	struct anx_effect_fence_view fence, held;
	struct anx_external_call *call = NULL;
	anx_oid_t branches[8] = {0}, denied = ANX_UUID_NIL;
	int ret = anx_so_create(&p, &obj);
	if (ret == ANX_OK) ret = anx_so_open(&obj->oid, ANX_OPEN_READWRITE, &h);
	anx_strlcpy(intent.name, "research-day-045", sizeof(intent.name));
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXECUTION, &intent, &owner);
	if (ret == ANX_OK) ret = anx_cell_create(ANX_CELL_TASK_EXTERNAL_CALL, &intent, &foreign);
	if (ret == ANX_OK) ret = anx_external_register_handler("anxresearch045", unused_handler, &context);
	if (ret != ANX_OK) goto out;
	request.owner = owner->cid; request.origin = obj->oid;
	uint64_t version = obj->version;
	uint32_t provenance = anx_prov_log_count(obj->provenance);
	ret = prepare(&request, &branches[0]);
	if (ret != ANX_OK) goto out;
	ret = -4501;
	if (anx_speculation_commit(&branches[0], "different", 9) != ANX_ECANCELED || context.calls || obj->staged ||
	    obj->version != version || obj->payload_size != 8 || anx_memcmp(obj->payload, "original", 8) ||
	    anx_prov_log_count(obj->provenance) != provenance) goto out;
	ret = -4502;
	if (anx_speculation_get(&branches[0], &view) != ANX_OK || view.state != ANX_SPECULATION_DISCARDED ||
	    anx_speculation_commit(&branches[0], "replace", 7) != ANX_ECANCELED) goto out;
	ret = prepare(&request, &branches[1]);
	if (ret == ANX_OK) ret = anx_so_replace_payload(&h, "current", 7);
	if (ret != ANX_OK) goto out;
	version = obj->version;
	ret = -4503;
	if (anx_speculation_commit(&branches[1], "replace", 7) != ANX_EBUSY || obj->version != version ||
	    obj->payload_size != 7 || anx_memcmp(obj->payload, "current", 7) || obj->staged) goto out;
	/* Preparation did not grant the owner permission to publish. */
	ret = prepare(&request, &branches[2]);
	if (ret != ANX_OK) goto out;
	ret = -4504;
	if (anx_cell_destroy(owner) != ANX_EBUSY ||
	    anx_speculation_commit(&branches[2], "replace", 7) != ANX_EPERM || obj->version != version || obj->staged) goto out;
	owner->execution.allow_side_effects = true;
	ret = anx_effect_fence_create(&fence);
	if (ret == ANX_OK) ret = anx_effect_fence_bind(owner, &fence.id);
	if (ret == ANX_OK) ret = prepare(&request, &branches[3]);
	if (ret == ANX_OK) ret = anx_effect_fence_transition(&fence.id, fence.generation, ANX_FENCE_HELD);
	if (ret != ANX_OK) goto out;
	ret = -4505;
	if (anx_speculation_commit(&branches[3], "replace", 7) != ANX_EBUSY || obj->version != version || obj->staged) goto out;
	ret = anx_effect_fence_get(&fence.id, &held);
	if (ret == ANX_OK) ret = anx_effect_fence_transition(&fence.id, held.generation, ANX_FENCE_RUNNING);
	if (ret == ANX_OK) ret = prepare(&request, &branches[4]);
	if (ret == ANX_OK) ret = prepare(&request, &branches[5]);
	if (ret != ANX_OK) goto out;
	ret = -4506;
	if (prepare(&request, &denied) != ANX_EFULL || !anx_uuid_is_nil(&denied)) goto out;
	context.branch = branches[4]; context.request = request;
	ret = anx_external_register_handler("anxresearch045foreign", foreign_handler, &context);
	if (ret != ANX_OK) goto out;
	call = anx_zalloc(sizeof(*call));
	ret = ANX_ENOMEM;
	if (!call) goto out;
	anx_strlcpy(call->endpoint, "anxresearch045foreign://check", sizeof(call->endpoint));
	foreign->ext_call = call; foreign->execution.allow_side_effects = true;
	ret = anx_cell_run(foreign);
	if (ret == ANX_OK) ret = anx_speculation_commit(&branches[4], "replace", 7);
	if (ret != ANX_OK) goto out;
	ret = -4507;
	if (obj->version != version + 1 || obj->payload_size != 9 || anx_memcmp(obj->payload, "predicted", 9) || obj->staged ||
	    anx_speculation_get(&branches[4], &view) != ANX_OK || view.state != ANX_SPECULATION_COMMITTED ||
	    anx_speculation_commit(&branches[4], "replace", 7) != ANX_EBUSY || context.calls) goto out;
	ret = anx_speculation_discard(&branches[5]);
	if (ret != ANX_OK) goto out;
	request.expires_at = arch_time_now() + 20000000ULL;
	ret = anx_speculation_prepare(&request, &branches[6]);
	if (ret != ANX_OK) goto out;
	while (arch_time_now() < request.expires_at) { }
	ret = -4508;
	if (anx_speculation_commit(&branches[6], "replace", 7) != ANX_ETIMEDOUT ||
	    anx_speculation_get(&branches[6], &view) != ANX_OK || view.state != ANX_SPECULATION_EXPIRED || obj->version != version + 1) goto out;
	ret = prepare(&request, &branches[7]);
	if (ret != ANX_OK) goto out;
	obj->access_policy.rule_count = 1;
	obj->access_policy.rules[0].operations = ANX_ACCESS_WRITE_PAYLOAD;
	obj->access_policy.rules[0].effect = ANX_EFFECT_DENY;
	ret = -4510;
	if (anx_speculation_commit(&branches[7], "replace", 7) != ANX_EPERM || obj->staged || obj->version != version + 1 || context.calls) goto out;
	obj->access_policy.rule_count = 0;
	ret = ANX_OK;
out:
	for (uint32_t i = 0; i < 8; i++) if (!anx_uuid_is_nil(&branches[i])) anx_speculation_release(&branches[i]);
	anx_external_unregister_handler("anxresearch045");
	anx_external_unregister_handler("anxresearch045foreign");
	anx_so_close(&h);
	if (obj) { anx_so_delete(&obj->oid, false); anx_objstore_release(obj); }
	if (owner) anx_cell_destroy(owner);
	if (foreign) anx_cell_destroy(foreign);
	anx_free(call);
	return ret;
}
#endif
